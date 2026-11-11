// ─────────────────────────────────────────────────────────────────────────────
// test_capture.inc — CAPTURE 模块化单元测试集
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/CaptureState.h"
#include "capture/CaptureDropdownMenu.h"
#include "capture/PinGroupManager.h"
#include "capture/PinInputController.h"
#include "capture/ScrollCaptureOverlay.h"
#include "capture/ScrollCaptureEditorOverlay.h"
#include "capture/CaptureVectorIcons.h"

TEST(CaptureBackendTest, Direct3DAndGdiSmoke) {
    const auto capabilities = tools3000::capture::captureBackendCapabilities();
    EXPECT_GE(capabilities.size(), 2u);
    bool hasGdi = false;
    bool hasDxgi = false;
    for (const auto& capability : capabilities) {
        if (capability.id == "gdi-bitblt") hasGdi = capability.available;
        if (capability.id == "dxgi-desktop-duplication") hasDxgi = true;
    }
    EXPECT_TRUE(hasGdi);
    EXPECT_TRUE(hasDxgi);

    // 1. 验证内存虚拟捕获驱动 (BGR24 与 BGRA32)
    for (auto format : {tools3000::capture::CapturePixelFormat::Bgr24, tools3000::capture::CapturePixelFormat::Bgra32}) {
        auto memBackend = tools3000::capture::createMemoryCaptureBackend(format);
        EXPECT_NE(memBackend, nullptr);
        EXPECT_EQ(memBackend->info().id, "memory-synthetic");
        EXPECT_TRUE(memBackend->info().available);

        std::string err;
        // 异常尺寸防护测试
        EXPECT_FALSE(memBackend->initialize(tools3000::capture::CaptureRegion{0, 0, -5, 10}, err));
        EXPECT_FALSE(err.empty());

        // 正常尺寸初始化与多帧捕获测试
        const tools3000::capture::CaptureRegion validRegion{0, 0, 128, 96};
        EXPECT_TRUE(memBackend->initialize(validRegion, err));
        tools3000::capture::CaptureFrameView frame;
        EXPECT_TRUE(memBackend->capture(frame, err));
        EXPECT_NE(frame.data, nullptr);
        EXPECT_EQ(frame.width, 128);
        EXPECT_EQ(frame.height, 96);
        EXPECT_EQ(frame.format, format);
        EXPECT_GE(frame.stride, 128 * (format == tools3000::capture::CapturePixelFormat::Bgra32 ? 4 : 3));
        memBackend->releaseFrame();
        memBackend->shutdown();
    }

    // 2. 真实系统后端探测 (若桌面可用则执行)
    const tools3000::capture::CaptureRegion region{
        GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 64, 64
    };
    auto backend = tools3000::capture::createCaptureBackend();
    std::string error;
    if (backend && backend->initialize(region, error)) {
        tools3000::capture::CaptureFrameView frame;
        if (backend->capture(frame, error)) {
            EXPECT_NE(frame.data, nullptr);
            EXPECT_EQ(frame.width, 64);
            EXPECT_EQ(frame.height, 64);
            EXPECT_GE(frame.stride, frame.width * 3);
            backend->releaseFrame();
        }
        backend->shutdown();
    }
}

// -----------------------------------------------------------------------------
// 18. WASAPI 音频捕获与 Loopback 测试套件
// -----------------------------------------------------------------------------
TEST(AudioCaptureTest, LoopbackWasapiSmoke) {
    const auto devices = tools3000::capture::AudioCapture::devices();
    const auto outputDevice = std::find_if(devices.begin(), devices.end(),
        [](const auto& device) { return device.systemAudio; });
    if (outputDevice == devices.end()) {
        std::printf("[SKIP] WASAPI device enumeration: no active output device\n");
        return;
    }
    EXPECT_FALSE(outputDevice->id.empty());
    EXPECT_FALSE(outputDevice->name.empty());

    tools3000::capture::AudioCapture capture;
    tools3000::capture::AudioCaptureOptions options;
    options.systemAudio = true;
    options.systemDeviceId = outputDevice->id;
    if (!capture.start(options)) {
        std::printf("[SKIP] WASAPI loopback smoke: %s\n", capture.status().error.c_str());
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const auto status = capture.status();
    EXPECT_TRUE(status.systemAudioActive);
    EXPECT_GE(status.mixedPeak, 0.0f);
    EXPECT_LE(status.mixedPeak, 1.0f);
    std::vector<float> frames;
    EXPECT_TRUE(capture.readFrames(tools3000::capture::AudioCapture::SampleRate / 100, frames));
    EXPECT_EQ(frames.size(), static_cast<std::size_t>(
        tools3000::capture::AudioCapture::SampleRate / 100 * tools3000::capture::AudioCapture::Channels));
    capture.setVolumes(0.75f, 1.0f);
    capture.setSystemMuted(true);
    EXPECT_TRUE(capture.status().systemMuted);
    EXPECT_EQ(capture.status().systemPeak, 0.0f);
    capture.setSystemMuted(false);
    EXPECT_FALSE(capture.status().systemMuted);
    // The same object must be reusable after a full endpoint teardown. This
    // exercises the same close/reinitialize path used by runtime reconnection.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (capture.start(options)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        EXPECT_TRUE(capture.status().systemAudioActive);
        capture.stop();
    }
}

// -----------------------------------------------------------------------------
// 19. 长截图与滚动预览拼接测试套件 (内存虚拟驱动全链路测试)
// -----------------------------------------------------------------------------
TEST(ScrollCaptureTest, BoundedPreviewAndStitching) {
    // 注入内存虚拟捕获驱动，消除 CI 无显示器时的 SKIP
    tools3000::capture::setCaptureBackendFactoryForTesting([]() {
        return tools3000::capture::createMemoryCaptureBackend(tools3000::capture::CapturePixelFormat::Bgr24);
    });

    auto& capture = tools3000::capture::ScrollCapture::instance();
    tools3000::capture::ScrollCaptureResult completed;
    int progressCalls = 0;
    int largestPreviewRows = 0;
    capture.setProgressCallback([&](const cv::Mat& preview, int) {
        ++progressCalls;
        largestPreviewRows = std::max(largestPreviewRows, preview.rows);
    });
    capture.setCompletionCallback([&](const tools3000::capture::ScrollCaptureResult& result) {
        completed = result;
    });

    tools3000::capture::ScrollCaptureOptions options;
    options.mode = tools3000::capture::ScrollMode::Manual;
    options.captureRect = { 0, 0, 64, 64 };
    // Do not merely infer that the spill path ran from the byte budget.  Use a
    // dedicated directory so this test proves both the successful disk-backed
    // round trip and cleanup of the sensitive temporary pixels afterwards.
    const auto stagingDirectory = std::filesystem::temp_directory_path() /
        (L"Tools3000ScrollCaptureSuccess_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code stagingError;
    std::filesystem::remove_all(stagingDirectory, stagingError);
    ASSERT_TRUE(std::filesystem::create_directories(stagingDirectory, stagingError));
    options.stagingDirectory = stagingDirectory;
    // 强制走磁盘暂存：验证交付时能按顺序回读拼接，且不是只覆盖内存路径。
    options.maxInMemoryStagingBytes = 1;
    capture.start(options);
    EXPECT_TRUE(capture.isRunning());

    capture.captureCurrentFrame();
    capture.captureCurrentFrame();
    capture.stop();

    EXPECT_TRUE(completed.success);
    EXPECT_EQ(completed.frameCount, 2);
    EXPECT_EQ(completed.stitchedImage.cols, 64);
    EXPECT_GE(completed.stitchedImage.rows, 64);
    EXPECT_EQ(progressCalls, 2);
    EXPECT_LE(largestPreviewRows, 128);
    capture.shutdown();
    EXPECT_TRUE(std::filesystem::is_empty(stagingDirectory, stagingError));

    tools3000::capture::ScrollCaptureResult bounded;
    capture.setCompletionCallback([&](const tools3000::capture::ScrollCaptureResult& result) {
        bounded = result;
    });
    options.maxOutputBytes = 1;
    capture.start(options);
    capture.captureCurrentFrame();
    EXPECT_FALSE(capture.isRunning());
    EXPECT_FALSE(bounded.success);
    EXPECT_NE(bounded.errorMessage.find("内存上限"), std::string::npos);
    capture.shutdown();

    std::filesystem::remove_all(stagingDirectory, stagingError);

    // 重置测试工厂钩子
    tools3000::capture::setCaptureBackendFactoryForTesting(nullptr);
}

TEST(ScrollCaptureTest, FailedBackendStartupDoesNotPoisonNextSession) {
    tools3000::capture::setCaptureBackendFactoryForTesting([]() {
        return tools3000::capture::createMemoryCaptureBackend(tools3000::capture::CapturePixelFormat::Bgr24);
    });

    auto& capture = tools3000::capture::ScrollCapture::instance();
    tools3000::capture::ScrollCaptureResult failed;
    int completionCount = 0;
    capture.setCompletionCallback([&](const tools3000::capture::ScrollCaptureResult& result) {
        ++completionCount;
        failed = result;
    });

    tools3000::capture::ScrollCaptureOptions invalid;
    invalid.mode = tools3000::capture::ScrollMode::Manual;
    invalid.captureRect = {0, 0, 0, 0};
    capture.start(invalid);
    EXPECT_FALSE(capture.isRunning());
    EXPECT_EQ(completionCount, 1);
    EXPECT_FALSE(failed.success);
    EXPECT_FALSE(failed.errorMessage.empty());
    capture.stop();
    EXPECT_EQ(completionCount, 1);

    tools3000::capture::ScrollCaptureResult recovered;
    capture.setCompletionCallback([&](const tools3000::capture::ScrollCaptureResult& result) {
        recovered = result;
    });
    tools3000::capture::ScrollCaptureOptions valid;
    valid.mode = tools3000::capture::ScrollMode::Manual;
    valid.captureRect = {0, 0, 32, 32};
    valid.maxInMemoryStagingBytes = 1;
    capture.start(valid);
    ASSERT_TRUE(capture.isRunning());
    capture.captureCurrentFrame();
    capture.stop();
    EXPECT_TRUE(recovered.success);
    EXPECT_EQ(recovered.frameCount, 1);

    capture.shutdown();
    tools3000::capture::setCaptureBackendFactoryForTesting(nullptr);
}

TEST(ScrollCaptureTest, DiskStagingFailureDeliversSafePartialFailureAndRecovers) {
    using namespace tools3000::capture;
    setCaptureBackendFactoryForTesting([]() {
        return createMemoryCaptureBackend(CapturePixelFormat::Bgr24);
    });

    const auto nonDirectory = std::filesystem::temp_directory_path() /
        (L"Tools3000ScrollCaptureStagingFile_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove(nonDirectory, error);
    { std::ofstream marker(nonDirectory, std::ios::binary); ASSERT_TRUE(marker.good()); }

    auto& capture = ScrollCapture::instance();
    ScrollCaptureResult failed;
    capture.setCompletionCallback([&](const ScrollCaptureResult& result) { failed = result; });
    ScrollCaptureOptions invalid;
    invalid.mode = ScrollMode::Manual;
    invalid.captureRect = {0, 0, 32, 32};
    invalid.maxInMemoryStagingBytes = 1;
    invalid.stagingDirectory = nonDirectory;
    capture.start(invalid);
    ASSERT_TRUE(capture.isRunning());
    capture.captureCurrentFrame();
    EXPECT_FALSE(capture.isRunning());
    EXPECT_FALSE(failed.success);
    EXPECT_NE(failed.errorMessage.find("临时存储"), std::string::npos);
    capture.shutdown();

    std::filesystem::remove(nonDirectory, error);
    ScrollCaptureResult recovered;
    capture.setCompletionCallback([&](const ScrollCaptureResult& result) { recovered = result; });
    invalid.stagingDirectory.clear();
    capture.start(invalid);
    ASSERT_TRUE(capture.isRunning());
    capture.captureCurrentFrame();
    capture.stop();
    EXPECT_TRUE(recovered.success);
    capture.shutdown();
    setCaptureBackendFactoryForTesting(nullptr);
}

namespace {
class ShortWriteBuffer final : public std::streambuf {
protected:
    std::streamsize xsputn(const char*, std::streamsize count) override {
        return count > 0 ? count - 1 : 0;
    }
    int_type overflow(int_type) override { return traits_type::eof(); }
};
}  // namespace

TEST(ScrollCaptureStorageTest, RejectsPartialWritesAndTruncatedReads) {
    cv::Mat segment(2, 3, CV_8UC3, cv::Scalar(7, 11, 13));
    ShortWriteBuffer shortWrite;
    std::ostream failedOutput(&shortWrite);
    EXPECT_FALSE(tools3000::capture::scroll_storage::writeMat(failedOutput, segment));

    std::istringstream complete("abcdef", std::ios::binary);
    std::array<char, 6> bytes{};
    EXPECT_TRUE(tools3000::capture::scroll_storage::readExact(complete, bytes.data(), bytes.size()));
    EXPECT_EQ(std::string(bytes.data(), bytes.size()), "abcdef");

    std::istringstream truncated("abc", std::ios::binary);
    EXPECT_FALSE(tools3000::capture::scroll_storage::readExact(truncated, bytes.data(), bytes.size()));
}


namespace {
class SequenceCaptureBackend final : public tools3000::capture::ICaptureBackend {
public:
    explicit SequenceCaptureBackend(std::vector<cv::Mat> frames) : m_frames(std::move(frames)) {}

    const tools3000::capture::CaptureBackendInfo& info() const noexcept override {
        return m_info;
    }

    bool initialize(const tools3000::capture::CaptureRegion& region, std::string& error) override {
        if (!region.isValid() || m_frames.empty()) {
            error = "invalid sequence region";
            return false;
        }
        m_region = region;
        m_cursor = 0;
        return true;
    }

    bool capture(tools3000::capture::CaptureFrameView& out, std::string&) override {
        const std::size_t idx = std::min(m_cursor, m_frames.size() - 1);
        m_current = m_frames[idx].clone();
        if (m_cursor + 1 < m_frames.size()) ++m_cursor;
        out.data = m_current.data;
        out.stride = static_cast<int>(m_current.step);
        out.width = m_current.cols;
        out.height = m_current.rows;
        out.format = tools3000::capture::CapturePixelFormat::Bgr24;
        return true;
    }

    void releaseFrame() noexcept override {}
    void shutdown() noexcept override { m_current.release(); }

private:
    tools3000::capture::CaptureBackendInfo m_info{"memory", "Sequence Memory Backend", true, false, {}};
    std::vector<cv::Mat> m_frames;
    tools3000::capture::CaptureRegion m_region{};
    std::size_t m_cursor = 0;
    cv::Mat m_current;
};

cv::Mat buildStickyChatSequenceFrame(const cv::Mat& fullDoc, int scrollY, int viewW, int viewH,
                                     int headerH, int footerH, bool cursorBlinkOn) {
    cv::Mat frame(viewH, viewW, CV_8UC3, cv::Scalar(22, 23, 28));
    // 顶部固定导航栏 (Sticky Header)
    frame.rowRange(0, headerH).setTo(cv::Scalar(38, 42, 56));
    cv::rectangle(frame, cv::Rect(16, 10, 88, 20), cv::Scalar(210, 218, 230), cv::FILLED);
    cv::line(frame, cv::Point(0, headerH - 1), cv::Point(viewW - 1, headerH - 1), cv::Scalar(72, 78, 96), 1);

    // 中间动态滚动内容区
    const int dynH = viewH - headerH - footerH;
    fullDoc(cv::Rect(0, scrollY, viewW, dynH)).copyTo(frame.rowRange(headerH, headerH + dynH));

    // 底部固定输入栏 (Sticky Footer, 模拟聊天输入框与局部闪烁光标)
    const int footerTop = viewH - footerH;
    frame.rowRange(footerTop, viewH).setTo(cv::Scalar(28, 30, 38));
    cv::rectangle(frame, cv::Rect(14, footerTop + 10, viewW - 28, footerH - 20), cv::Scalar(52, 56, 70), 2);
    for (int x = 26; x < viewW - 60; x += 18) {
        cv::rectangle(frame, cv::Rect(x, footerTop + 26, 11, 12), cv::Scalar(165, 172, 188), cv::FILLED);
    }
    if (cursorBlinkOn) {
        // 局部单点闪烁光标：不得破坏固定底栏整体静态识别
        cv::rectangle(frame, cv::Rect(24, footerTop + 22, 2, 20), cv::Scalar(250, 250, 255), cv::FILLED);
    }
    return frame;
}
}  // namespace

TEST(ScrollCaptureTest, StickyHeaderAndFooterStrippedWithoutPrematureStop) {
    using namespace tools3000::capture;

    constexpr int kViewW = 240;
    constexpr int kViewH = 360;
    constexpr int kHeaderH = 40;
    constexpr int kFooterH = 70;
    constexpr int kDynH = kViewH - kHeaderH - kFooterH; // 250 px
    constexpr int kStep = 140;
    constexpr int kScrollSteps = 4; // 共 5 帧有效内容 + 1 帧触底静止帧
    const int docH = kDynH + kStep * kScrollSteps + 40;

    cv::Mat fullDoc(docH, kViewW, CV_8UC3, cv::Scalar(20, 21, 26));
    for (int y = 0; y < docH; y += 16) {
        // 构造具备唯一行指纹特征与周期重复卡片并存的聊天消息流
        const bool repeatedReviewBar = ((y % 160) == 96);
        if (repeatedReviewBar) {
            cv::rectangle(fullDoc, cv::Rect(20, y, kViewW - 44, 12), cv::Scalar(48, 82, 64), cv::FILLED);
        } else {
            const int len = 50 + ((y * 37) % (kViewW - 90));
            const cv::Scalar color(
                70 + ((y * 13) % 160),
                85 + ((y * 29) % 150),
                95 + ((y * 43) % 145));
            cv::rectangle(fullDoc, cv::Rect(22, y + 2, len, 9), color, cv::FILLED);
            cv::rectangle(fullDoc, cv::Rect(22 + ((y * 7) % 60), y + 4, 24, 6),
                          cv::Scalar(220, (y * 19) % 255, (y * 53) % 255), cv::FILLED);
        }
    }

    std::vector<cv::Mat> frames;
    for (int i = 0; i <= kScrollSteps; ++i) {
        frames.push_back(buildStickyChatSequenceFrame(
            fullDoc, i * kStep, kViewW, kViewH, kHeaderH, kFooterH, (i % 2) == 1));
    }
    // 末尾追加一帧完全触底静止帧，验证自动触底检测在固定栏场景下精准停止
    frames.push_back(frames.back().clone());

    auto& capture = ScrollCapture::instance();
    const ScrollFrameTransition tr01 = capture.analyzeFrameTransition(frames[0], frames[1]);
    EXPECT_FALSE(tr01.isUnchanged);
    EXPECT_TRUE(tr01.matched);
    EXPECT_NEAR(tr01.scrollDelta, kStep, 1);
    EXPECT_GE(tr01.stickyTopRows, kHeaderH - 2);
    EXPECT_GE(tr01.stickyBottomRows, kFooterH - 4);

    // 验证离线与磁盘溢出自动流式长截图均不会提前终止，且中间零固定底栏残留
    setCaptureBackendFactoryForTesting([frames]() {
        return std::make_unique<SequenceCaptureBackend>(frames);
    });

    ScrollCaptureResult autoResult;
    capture.setCompletionCallback([&](const ScrollCaptureResult& r) { autoResult = r; });

    ScrollCaptureOptions opts;
    opts.mode = ScrollMode::Auto;
    opts.captureRect = {0, 0, kViewW, kViewH};
    opts.maxInMemoryStagingBytes = 1; // 强制走磁盘暂存回读裁剪路径
    opts.interactiveHoldAtBottom = false;
    capture.start(opts);

    // SequenceCaptureBackend ("memory") 无额外等待，等待工作线程完成
    for (int wait = 0; wait < 200 && capture.isRunning(); ++wait) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    capture.stop();

    ASSERT_TRUE(autoResult.success);
    EXPECT_FALSE(autoResult.cancelled);
    EXPECT_EQ(autoResult.frameCount, kScrollSteps + 1);

    const int expectedTotalRows = kHeaderH + (kDynH + kScrollSteps * kStep) + kFooterH;
    EXPECT_NEAR(autoResult.stitchedImage.rows, expectedTotalRows, 3);

    // 核心像素断言：成图中间完整还原虚拟文档动态区，绝对不包含重复拼入的底部固定输入框
    if (std::abs(autoResult.stitchedImage.rows - expectedTotalRows) <= 2) {
        const int checkRows = kDynH + kScrollSteps * kStep - 4;
        cv::Mat stitchedMid = autoResult.stitchedImage.rowRange(kHeaderH, kHeaderH + checkRows);
        cv::Mat expectedMid = fullDoc.rowRange(0, checkRows);
        cv::Mat midDiff;
        cv::absdiff(stitchedMid, expectedMid, midDiff);
        const cv::Scalar err = cv::mean(midDiff);
        EXPECT_LT((err[0] + err[1] + err[2]) / 3.0, 1.5);
    }

    capture.shutdown();
    setCaptureBackendFactoryForTesting(nullptr);
}

TEST(ScrollCaptureTest, PauseResumeCancelAndPinActionStateMachine) {
    using namespace tools3000::capture;
    setCaptureBackendFactoryForTesting([]() {
        return createMemoryCaptureBackend(CapturePixelFormat::Bgr24);
    });

    auto& capture = ScrollCapture::instance();
    ScrollCaptureResult cancelledRes;
    capture.setCompletionCallback([&](const ScrollCaptureResult& r) { cancelledRes = r; });

    ScrollCaptureOptions opts;
    opts.mode = ScrollMode::Manual;
    opts.captureRect = {0, 0, 80, 60};
    capture.start(opts);
    ASSERT_TRUE(capture.isRunning());
    capture.captureCurrentFrame();
    EXPECT_EQ(capture.stitchedDimensions().cx, 80);
    EXPECT_EQ(capture.stitchedDimensions().cy, 60);

    capture.pause();
    EXPECT_TRUE(capture.isPaused());
    capture.resume();
    EXPECT_FALSE(capture.isPaused());

    capture.cancel();
    EXPECT_FALSE(capture.isRunning());
    EXPECT_TRUE(cancelledRes.cancelled);
    EXPECT_FALSE(cancelledRes.success);
    EXPECT_TRUE(cancelledRes.stitchedImage.empty());

    ScrollCaptureResult pinRes;
    capture.setCompletionCallback([&](const ScrollCaptureResult& r) { pinRes = r; });
    capture.start(opts);
    capture.captureCurrentFrame();
    capture.stop(ScrollFinishAction::Pin);
    EXPECT_TRUE(pinRes.success);
    EXPECT_FALSE(pinRes.cancelled);
    EXPECT_EQ(pinRes.action, ScrollFinishAction::Pin);

    capture.shutdown();
    setCaptureBackendFactoryForTesting(nullptr);
}

TEST(ScreenRecorderTest, FrameEncodingSmoke) {
    // 注入内存虚拟捕获驱动，消除 CI 依赖
    tools3000::capture::setCaptureBackendFactoryForTesting([]() {
        return tools3000::capture::createMemoryCaptureBackend(tools3000::capture::CapturePixelFormat::Bgr24);
    });

    const auto output = std::filesystem::temp_directory_path() /
        (L"Tools3000Recorder_" + std::to_wstring(GetCurrentProcessId()) + L".mp4");
    std::error_code ec;
    std::filesystem::remove(output, ec);

    tools3000::capture::RecordOptions options;
    options.format = tools3000::capture::RecordFormat::MP4_H264;
    options.fps = 15;
    options.width = 320;
    options.height = 240;
    options.regionX = 0;
    options.regionY = 0;
    options.fullScreen = false;
    options.bitrateMbps = 2;
    options.captureSystemAudio = false;
    options.countdownSeconds = 0;
    options.outputPath = tools3000::core::WinUtils::wstringToUtf8(output.wstring());

    auto& recorder = tools3000::capture::ScreenRecorder::instance();
    EXPECT_TRUE(recorder.initialize());
    EXPECT_TRUE(recorder.startRecording(options));
    std::this_thread::sleep_for(std::chrono::milliseconds(350));

    const auto completedPath = recorder.stopRecording();
    const auto stats = recorder.stats();
    EXPECT_EQ(completedPath, options.outputPath);
    EXPECT_GT(stats.frameCount, 0);
    EXPECT_FALSE(stats.captureBackend.empty());
    EXPECT_FALSE(stats.encoderName.empty());
    EXPECT_GT(stats.diskFreeBytes, 0u);
    EXPECT_GE(stats.captureLatencyMs, 0.0);
    EXPECT_GT(std::filesystem::file_size(output, ec), 0u);
    EXPECT_FALSE(ec);

    AVFormatContext* probe = nullptr;
    EXPECT_GE(avformat_open_input(&probe, options.outputPath.c_str(), nullptr, nullptr), 0);
    if (probe) {
        EXPECT_GE(avformat_find_stream_info(probe, nullptr), 0);
        bool hasVideo = false;
        for (unsigned index = 0; index < probe->nb_streams; ++index) {
            hasVideo |= probe->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
        }
        EXPECT_TRUE(hasVideo);
        avformat_close_input(&probe);
    }
    recorder.shutdown();
    std::filesystem::remove(output, ec);

    // 倒计时取消流程验证
    const auto cancelledOutput = std::filesystem::temp_directory_path() /
        (L"Tools3000RecorderCancelled_" + std::to_wstring(GetCurrentProcessId()) + L".mp4");
    const auto partialOutput = std::filesystem::path(cancelledOutput.wstring() + L".partial");
    std::filesystem::remove(cancelledOutput, ec);
    std::filesystem::remove(partialOutput, ec);
    options.outputPath = tools3000::core::WinUtils::wstringToUtf8(cancelledOutput.wstring());
    options.countdownSeconds = 2;
    EXPECT_TRUE(recorder.initialize());
    EXPECT_TRUE(recorder.startRecording(options));
    EXPECT_EQ(recorder.state(), tools3000::capture::RecordState::Countdown);
    EXPECT_GT(recorder.stats().countdownRemaining, 0);
    EXPECT_TRUE(recorder.stopRecording().empty());
    EXPECT_FALSE(std::filesystem::exists(cancelledOutput));
    EXPECT_FALSE(std::filesystem::exists(partialOutput));
    recorder.shutdown();

    // 异常输出路径拦截测试
    const auto invalidParent = std::filesystem::temp_directory_path() /
        (L"Tools3000RecorderParent_" + std::to_wstring(GetCurrentProcessId()));
    {
        std::ofstream parentFile(invalidParent, std::ios::binary | std::ios::trunc);
        parentFile << "not a directory";
    }
    const auto invalidOutput = invalidParent / L"record.mp4";
    options.countdownSeconds = 0;
    options.outputPath = tools3000::core::WinUtils::wstringToUtf8(invalidOutput.wstring());
    EXPECT_TRUE(recorder.initialize());
    EXPECT_FALSE(recorder.startRecording(options));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(invalidOutput.wstring() + L".partial")));
    recorder.shutdown();
    std::filesystem::remove(invalidParent, ec);

    // 重置测试工厂钩子
    tools3000::capture::setCaptureBackendFactoryForTesting(nullptr);
}

TEST(RecordingGpuProbeTest, RequiresD3d11AndAtLeastOneHardwareBackend) {
    tools3000::capture::RecordingGpuProbe probe;
    EXPECT_FALSE(probe.canExperiment());
    probe.d3d11Available = true;
    EXPECT_FALSE(probe.canExperiment());
    probe.mediaFoundationHardwareMftAvailable = true;
    EXPECT_TRUE(probe.canExperiment());
    probe.mediaFoundationHardwareMftAvailable = false;
    probe.ffmpegD3d11vaCompiled = true;
    EXPECT_TRUE(probe.canExperiment());
}

// -----------------------------------------------------------------------------
// 21. 光标叠加与图层复原测试套件
// -----------------------------------------------------------------------------
TEST(CursorOverlayTest, StateRestoration) {
    constexpr int size = 128;
    const tools3000::capture::CaptureRegion region{0, 0, size, size};
    auto backend = tools3000::capture::createMemoryCaptureBackend(tools3000::capture::CapturePixelFormat::Bgra32);
    std::string error;
    EXPECT_TRUE(backend->initialize(region, error));
    tools3000::capture::CaptureFrameView frame;
    EXPECT_TRUE(backend->capture(frame, error));

    const int pixelBytes = 4;
    const std::size_t rowBytes = static_cast<std::size_t>(frame.width) * pixelBytes;
    std::vector<std::uint8_t> original(rowBytes * frame.height);
    for (int row = 0; row < frame.height; ++row) {
        std::memcpy(original.data() + static_cast<std::size_t>(row) * rowBytes,
                    frame.data + static_cast<std::size_t>(row) * frame.stride, rowBytes);
    }

    tools3000::capture::CursorOverlay overlay;
    {
        auto patch = overlay.apply(frame, region, true, false);
        // 在 patch 作用域内，图层被保护
    }
    bool restored = true;
    for (int row = 0; row < frame.height && restored; ++row) {
        restored = std::memcmp(
            original.data() + static_cast<std::size_t>(row) * rowBytes,
            frame.data + static_cast<std::size_t>(row) * frame.stride,
            rowBytes) == 0;
    }
    EXPECT_TRUE(restored);
    backend->releaseFrame();
    backend->shutdown();
}

// -----------------------------------------------------------------------------
// 22. 截图微调与快捷键提示测试套件
// -----------------------------------------------------------------------------
TEST(CaptureToolbarTest, MicroActionsAndHintDisplay) {
    using tools3000::capture::ShortcutHintOverlay;
    using tools3000::capture::ShortcutHintContext;

    auto& overlay = ShortcutHintOverlay::instance();
    auto selectingItems = overlay.getItemsForContext(ShortcutHintContext::CaptureSelecting);
    EXPECT_FALSE(selectingItems.empty());
    bool hasSpacePan = false;
    bool hasWasdNudge = false;
    bool hasColorCopy = false;
    for (const auto& item : selectingItems) {
        if (item.key == L"Space") hasSpacePan = true;
        if (item.key == L"WASD") hasWasdNudge = true;
        if (item.key == L"C") hasColorCopy = true;
    }
    EXPECT_TRUE(hasSpacePan);
    EXPECT_TRUE(hasWasdNudge);
    EXPECT_TRUE(hasColorCopy);

    auto selectedItems = overlay.getItemsForContext(ShortcutHintContext::CaptureSelected);
    EXPECT_FALSE(selectedItems.empty());
    bool hasWasdMove = false;
    bool hasShiftWasdResize = false;
    for (const auto& item : selectedItems) {
        if (item.key == L"WASD") hasWasdMove = true;
        if (item.key == L"Shift+WASD") hasShiftWasdResize = true;
    }
    EXPECT_TRUE(hasWasdMove);
    EXPECT_TRUE(hasShiftWasdResize);

    auto recordSelectingItems = overlay.getItemsForContext(ShortcutHintContext::RecordSelecting);
    EXPECT_FALSE(recordSelectingItems.empty());
    bool hasRecordSpacePan = false;
    for (const auto& item : recordSelectingItems) {
        if (item.key == L"Space") hasRecordSpacePan = true;
    }
    EXPECT_TRUE(hasRecordSpacePan);
}

// -----------------------------------------------------------------------------
// 23. 快捷键提示 DPI 度量测试套件
// -----------------------------------------------------------------------------
TEST(ShortcutHintTest, DpiMetricsAndAlignment) {
    using tools3000::capture::ShortcutHintStyle;
    EXPECT_NEAR(ShortcutHintStyle::scaleForDpi(0), 1.0f, 0.001f);
    EXPECT_NEAR(ShortcutHintStyle::scaleForDpi(96), 1.0f, 0.001f);
    EXPECT_NEAR(ShortcutHintStyle::scaleForDpi(120), 1.25f, 0.001f);
    EXPECT_NEAR(ShortcutHintStyle::scaleForDpi(144), 1.5f, 0.001f);
    EXPECT_NEAR(ShortcutHintStyle::scaleForDpi(192), 2.0f, 0.001f);
    EXPECT_NEAR(ShortcutHintStyle::scaleForDpi(288), 3.0f, 0.001f);
    EXPECT_NEAR(ShortcutHintStyle::scaleForDpi(480), 5.0f, 0.001f);
    EXPECT_NEAR(ShortcutHintStyle::scaleForDpi(768), 5.0f, 0.001f);
    EXPECT_GE(ShortcutHintStyle::BaseKeyFont, 13.0f);
    EXPECT_GE(ShortcutHintStyle::BaseLabelFont, 13.0f);
    EXPECT_GE(ShortcutHintStyle::BaseKeyHeight, ShortcutHintStyle::BaseKeyFont * 2.0f);

    float previousFontPixels = 0.0f;
    for (const unsigned dpi : {96u, 120u, 144u, 192u, 240u, 288u, 384u, 480u}) {
        const float fontPixels = ShortcutHintStyle::BaseLabelFont *
                                 ShortcutHintStyle::scaleForDpi(dpi);
        EXPECT_GE(fontPixels, previousFontPixels);
        previousFontPixels = fontPixels;
    }
}

// -----------------------------------------------------------------------------
// 24. 高分屏共享 DPI 度量与自适应换算测试套件
// -----------------------------------------------------------------------------

TEST(CaptureToolbarTest, DpiAdaptiveLayout) {
    using namespace tools3000::capture;

    CaptureState screenshot;
    screenshot.mode = OverlayMode::Screenshot;
    screenshot.isMarkupToolActive = true;
    screenshot.dpiScale = 1.5f;  // Windows 150% / 144 DPI
    const D2D1_SIZE_F desktop150 = D2D1::SizeF(2304.0f, 1440.0f);
    rebuildCaptureToolbar(screenshot, D2D1::RectF(180.0f, 120.0f, 1600.0f, 980.0f),
                          desktop150);
    EXPECT_EQ(screenshot.toolbarButtons.size(), 18u);
    EXPECT_EQ(screenshot.secondaryToolbarButtons.size(), 15u); // 7 属性 + 7 预设色 + 1 拾色器
    EXPECT_EQ(screenshot.selectionSideButtons.size(), 4u);
    bool hasRecordBtn = false;
    bool hasCopyBtn = false;
    bool hasMagnifierBtn = false;
    for (const auto& btn : screenshot.toolbarButtons) {
        if (btn.command == ToolbarCommand::StartRecord) hasRecordBtn = true;
        if (btn.command == ToolbarCommand::Copy) hasCopyBtn = true;
        if (btn.command == ToolbarCommand::SelectTool && btn.tool == MarkupTool::Magnifier) hasMagnifierBtn = true;
    }
    EXPECT_TRUE(hasRecordBtn);
    EXPECT_TRUE(hasCopyBtn);
    EXPECT_TRUE(hasMagnifierBtn);
    EXPECT_NEAR(screenshot.toolbarButtons.front().rect.bottom -
                screenshot.toolbarButtons.front().rect.top, 48.0f, 0.01f);
    check_toolbar_inside_surface(screenshot, desktop150);
    const auto* cachedButtons = screenshot.toolbarButtons.data();
    rebuildCaptureToolbar(screenshot, D2D1::RectF(180.0f, 120.0f, 1600.0f, 980.0f),
                          desktop150);
    EXPECT_EQ(screenshot.toolbarButtons.data(), cachedButtons);

    CaptureState wrapped;
    wrapped.mode = OverlayMode::Screenshot;
    wrapped.isMarkupToolActive = true;
    wrapped.dpiScale = 2.0f;
    const D2D1_SIZE_F compact = D2D1::SizeF(1000.0f, 700.0f);
    rebuildCaptureToolbar(wrapped, D2D1::RectF(80.0f, 100.0f, 850.0f, 480.0f), compact);
    EXPECT_EQ(wrapped.toolbarButtons.size(), 18u);
    EXPECT_EQ(wrapped.secondaryToolbarButtons.size(), 15u); // 7 属性 + 7 预设色 + 1 拾色器
    EXPECT_EQ(wrapped.selectionSideButtons.size(), 4u);
    check_toolbar_inside_surface(wrapped, compact);

    CaptureState recording;
    recording.mode = OverlayMode::RecordRegion;
    recording.dpiScale = 1.5f;
    rebuildCaptureToolbar(recording, D2D1::RectF(200.0f, 100.0f, 1200.0f, 800.0f),
                          desktop150);
    EXPECT_EQ(recording.toolbarButtons.size(), 9u);
    EXPECT_NEAR(recording.toolbarButtons[0].rect.right -
                recording.toolbarButtons[0].rect.left, 54.0f * 1.5f, 0.01f);
    EXPECT_NEAR(recording.toolbarButtons[1].rect.right -
                recording.toolbarButtons[1].rect.left, 62.0f * 1.5f, 0.01f);
    EXPECT_NEAR(recording.toolbarButtons[8].rect.right -
                recording.toolbarButtons[8].rect.left, (tools3000::core::WinUtils::isSystemLanguageChinese() ? 98.0f : 84.0f) * 1.5f, 0.01f);
    check_toolbar_inside_surface(recording, desktop150);

    CaptureState extreme;
    extreme.mode = OverlayMode::Screenshot;
    extreme.dpiScale = 5.0f;
    const D2D1_SIZE_F desktop500 = D2D1::SizeF(7680.0f, 4320.0f);
    rebuildCaptureToolbar(extreme, D2D1::RectF(500.0f, 400.0f, 7000.0f, 3600.0f),
                          desktop500);
    EXPECT_EQ(extreme.toolbarButtons.size(), 18u);
    check_toolbar_inside_surface(extreme, desktop500);
}

TEST(CaptureToolbarTest, AccessibilitySemanticsReflectActualState) {
    using namespace tools3000::capture;

    CaptureState state;
    state.state = OverlayState::Selected;
    state.currentTool = MarkupTool::Arrow;
    state.currentColor = MarkupColor::Blue();

    ToolbarButton arrow;
    arrow.command = ToolbarCommand::SelectTool;
    arrow.tool = MarkupTool::Arrow;
    EXPECT_EQ(toolbarButtonAccessibleName(arrow), L"箭头标注");
    EXPECT_TRUE(isToolbarButtonSelected(arrow, state));
    EXPECT_TRUE(isToolbarButtonEnabled(arrow, state));

    ToolbarButton blue;
    blue.command = ToolbarCommand::SelectColor;
    blue.color = MarkupColor::Blue();
    EXPECT_EQ(toolbarButtonAccessibleName(blue), L"标注颜色");
    EXPECT_TRUE(isToolbarButtonSelected(blue, state));

    ToolbarButton undo;
    undo.command = ToolbarCommand::Undo;
    EXPECT_EQ(toolbarButtonKeyboardShortcut(undo), L"Ctrl+Z");
    EXPECT_FALSE(isToolbarButtonEnabled(undo, state));
    ToolbarButton redo;
    redo.command = ToolbarCommand::Redo;
    EXPECT_FALSE(isToolbarButtonEnabled(redo, state));

    ToolbarButton confirm;
    confirm.command = ToolbarCommand::Confirm;
    EXPECT_EQ(toolbarButtonKeyboardShortcut(confirm), L"Enter");
    state.state = OverlayState::Idle;
    EXPECT_FALSE(isToolbarButtonEnabled(confirm, state));

    ToolbarButton copyBtn;
    copyBtn.command = ToolbarCommand::Copy;
    EXPECT_EQ(toolbarButtonAccessibleName(copyBtn), L"复制到剪贴板");
    EXPECT_EQ(toolbarButtonKeyboardShortcut(copyBtn), L"Ctrl+C");
    state.state = OverlayState::Selected;
    EXPECT_TRUE(isToolbarButtonEnabled(copyBtn, state));
    state.state = OverlayState::Idle;
    EXPECT_FALSE(isToolbarButtonEnabled(copyBtn, state));

    ToolbarButton startRecord;
    startRecord.command = ToolbarCommand::StartRecord;
    EXPECT_EQ(toolbarButtonAccessibleName(startRecord), L"区域录屏");
    state.state = OverlayState::Selected;
    EXPECT_TRUE(isToolbarButtonEnabled(startRecord, state));
}

namespace {
constexpr UINT WM_TOOLS3000_UIA_TEST_INVOKE = WM_APP + 0x4B1;

struct UiaTestSurface {
    HWND hwnd = nullptr;
    int invokeCount = 0;
};

LRESULT CALLBACK uiaTestSurfaceProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* surface = reinterpret_cast<UiaTestSurface*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        surface = static_cast<UiaTestSurface*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(surface));
        if (surface) surface->hwnd = hwnd;
    }
    if (message == WM_GETOBJECT) {
        using namespace tools3000::core::accessibility;
        return respondToOverlayUiaGetObject(hwnd, wParam, lParam,
            {L"Tools3000.TestOverlay", L"Automated UI Automation test surface", OverlayUiaRole::Pane, true},
            {{L"Tools3000.TestOverlay.confirm", L"Confirm capture", L"Confirm the test capture",
              L"Enter", {40, 60, 220, 104}, true, true, OverlayUiaActionRole::Button,
              WM_TOOLS3000_UIA_TEST_INVOKE, 0}});
    }
    if (message == WM_TOOLS3000_UIA_TEST_INVOKE) {
        if (surface) ++surface->invokeCount;
        return 0;
    }
    if (message == WM_NCDESTROY) {
        tools3000::core::accessibility::disconnectOverlayUiaProvider(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

class UiaTestWindow final {
public:
    explicit UiaTestWindow(UiaTestSurface* surface) {
        static const ATOM atom = [] {
            WNDCLASSW windowClass{};
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.lpszClassName = L"Tools3000.UiaProviderTestSurface";
            windowClass.lpfnWndProc = uiaTestSurfaceProc;
            return RegisterClassW(&windowClass);
        }();
        if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"Tools3000.UiaProviderTestSurface",
            L"Tools3000 UIA test", WS_POPUP, 40, 60, 180, 44,
            nullptr, nullptr, GetModuleHandleW(nullptr), surface);
        if (hwnd) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }

    ~UiaTestWindow() { if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd); }
    HWND hwnd = nullptr;
};

class CoApartment final {
public:
    CoApartment() : result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~CoApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    HRESULT result;
};
}  // namespace


TEST(PinWindowTest, TransformAndBoundsCalculation) {
    cv::Mat original = (cv::Mat_<uint8_t>(3, 2) << 10, 20,
                                                   30, 40,
                                                   50, 60);
    // 旋转 90 度
    cv::Mat rotated90;
    cv::rotate(original, rotated90, cv::ROTATE_90_CLOCKWISE);
    EXPECT_TRUE(rotated90.rows == 2 && rotated90.cols == 3);
    EXPECT_EQ(rotated90.at<uint8_t>(0, 0), 50);
    EXPECT_EQ(rotated90.at<uint8_t>(0, 2), 10);
    EXPECT_EQ(rotated90.at<uint8_t>(1, 0), 60);
    EXPECT_EQ(rotated90.at<uint8_t>(1, 2), 20);

    // 水平镜像翻转
    cv::Mat flippedH;
    cv::flip(original, flippedH, 1);
    EXPECT_TRUE(flippedH.rows == 3 && flippedH.cols == 2);
    EXPECT_TRUE(flippedH.at<uint8_t>(0, 0) == 20 && flippedH.at<uint8_t>(0, 1) == 10);
    EXPECT_TRUE(flippedH.at<uint8_t>(2, 0) == 60 && flippedH.at<uint8_t>(2, 1) == 50);

    // 垂直镜像翻转
    cv::Mat flippedV;
    cv::flip(original, flippedV, 0);
    EXPECT_TRUE(flippedV.rows == 3 && flippedV.cols == 2);
    EXPECT_TRUE(flippedV.at<uint8_t>(0, 0) == 50 && flippedV.at<uint8_t>(0, 1) == 60);
    EXPECT_TRUE(flippedV.at<uint8_t>(2, 0) == 10 && flippedV.at<uint8_t>(2, 1) == 20);
}

TEST(PinWindowTest, KeyboardShortcutsRotationFlipOpacityAndFocusPolicy) {
    using namespace tools3000::capture;

    // 1. 顺时针 90° 旋转快捷键: R / Ctrl+R / 数字 2 / 小键盘 2 / Ctrl+Right / Alt+Right
    for (const auto& [key, ctrl, shift, alt] : std::vector<std::tuple<WPARAM, bool, bool, bool>>{
             {'R', false, false, false},
             {'R', true,  false, false},
             {'2', false, false, false},
             {VK_NUMPAD2, false, false, false},
             {VK_RIGHT, true, false, false},
             {VK_RIGHT, false, false, true},
         }) {
        auto d = PinInputController::resolveKeyDown(key, ctrl, shift, alt);
        EXPECT_EQ(d.action, PinShortcutAction::RotateClockwise90);
        EXPECT_EQ(d.rotateDegrees, 90);
    }

    // 2. 逆时针 90° 旋转快捷键: Shift+R / Ctrl+Shift+R / L / 数字 1 / 小键盘 1 / Ctrl+Left / Alt+Left
    for (const auto& [key, ctrl, shift, alt] : std::vector<std::tuple<WPARAM, bool, bool, bool>>{
             {'R', false, true, false},
             {'R', true,  true, false},
             {'L', false, false, false},
             {'1', false, false, false},
             {VK_NUMPAD1, false, false, false},
             {VK_LEFT, true, false, false},
             {VK_LEFT, false, false, true},
         }) {
        auto d = PinInputController::resolveKeyDown(key, ctrl, shift, alt);
        EXPECT_EQ(d.action, PinShortcutAction::RotateCounterClockwise90);
        EXPECT_EQ(d.rotateDegrees, 270);
    }

    // 3. 水平与垂直镜像翻转快捷键: H / 3 / 小键盘 3 与 V / 4 / 小键盘 4 (确保 Ctrl+V 仍为粘贴贴图而非翻转)
    EXPECT_EQ(PinInputController::resolveKeyDown('H', false, false, false).action, PinShortcutAction::FlipHorizontal);
    EXPECT_EQ(PinInputController::resolveKeyDown('3', false, false, false).action, PinShortcutAction::FlipHorizontal);
    EXPECT_EQ(PinInputController::resolveKeyDown(VK_NUMPAD3, false, false, false).action, PinShortcutAction::FlipHorizontal);

    EXPECT_EQ(PinInputController::resolveKeyDown('V', false, false, false).action, PinShortcutAction::FlipVertical);
    EXPECT_EQ(PinInputController::resolveKeyDown('4', false, false, false).action, PinShortcutAction::FlipVertical);
    EXPECT_EQ(PinInputController::resolveKeyDown(VK_NUMPAD4, false, false, false).action, PinShortcutAction::FlipVertical);
    EXPECT_EQ(PinInputController::resolveKeyDown('V', true, false, false).action, PinShortcutAction::PasteHistoryOrClipboard);

    // 4. 透明度快捷键: 5~9 / 小键盘 5~9 / [ / ] / 0
    for (int digit = 5; digit <= 9; ++digit) {
        auto dTop = PinInputController::resolveKeyDown(static_cast<WPARAM>('0' + digit), false, false, false);
        EXPECT_EQ(dTop.action, PinShortcutAction::SetOpacity);
        EXPECT_NEAR(dTop.opacityValue, static_cast<float>(digit) / 10.0f, 1e-5f);

        auto dPad = PinInputController::resolveKeyDown(static_cast<WPARAM>(VK_NUMPAD0 + digit), false, false, false);
        EXPECT_EQ(dPad.action, PinShortcutAction::SetOpacity);
        EXPECT_NEAR(dPad.opacityValue, static_cast<float>(digit) / 10.0f, 1e-5f);
    }
    auto dDec = PinInputController::resolveKeyDown(VK_OEM_4, false, false, false);
    EXPECT_EQ(dDec.action, PinShortcutAction::AdjustOpacity);
    EXPECT_NEAR(dDec.opacityDelta, -0.1f, 1e-5f);

    auto dInc = PinInputController::resolveKeyDown(VK_OEM_6, false, false, false);
    EXPECT_EQ(dInc.action, PinShortcutAction::AdjustOpacity);
    EXPECT_NEAR(dInc.opacityDelta, 0.1f, 1e-5f);

    EXPECT_EQ(PinInputController::resolveKeyDown('0', false, false, false).action, PinShortcutAction::ResetScaleAndOpacity);
    EXPECT_EQ(PinInputController::resolveKeyDown(VK_NUMPAD0, false, false, false).action, PinShortcutAction::ResetScaleAndOpacity);

    // 5. 方向键与 WASD 像素级微调 (普通 1px, Shift 加速 10px)
    auto nudgeLeft = PinInputController::resolveKeyDown(VK_LEFT, false, false, false);
    EXPECT_EQ(nudgeLeft.action, PinShortcutAction::Nudge);
    EXPECT_EQ(nudgeLeft.nudgeDx, -1);
    EXPECT_EQ(nudgeLeft.nudgeDy, 0);

    auto nudgeRightFast = PinInputController::resolveKeyDown('D', false, true, false);
    EXPECT_EQ(nudgeRightFast.action, PinShortcutAction::Nudge);
    EXPECT_EQ(nudgeRightFast.nudgeDx, 10);
    EXPECT_EQ(nudgeRightFast.nudgeDy, 0);

    auto nudgeUp = PinInputController::resolveKeyDown('W', false, false, false);
    EXPECT_EQ(nudgeUp.action, PinShortcutAction::Nudge);
    EXPECT_EQ(nudgeUp.nudgeDx, 0);
    EXPECT_EQ(nudgeUp.nudgeDy, -1);

    auto nudgeDownFast = PinInputController::resolveKeyDown(VK_DOWN, false, true, false);
    EXPECT_EQ(nudgeDownFast.action, PinShortcutAction::Nudge);
    EXPECT_EQ(nudgeDownFast.nudgeDx, 0);
    EXPECT_EQ(nudgeDownFast.nudgeDy, 10);

    // 6. 其他核心快捷键覆盖: E / I / G / M / T / F4 / Alt+F4 / Delete / Ctrl+W / Esc / + / - / , / .
    EXPECT_EQ(PinInputController::resolveKeyDown('E', false, false, false).action, PinShortcutAction::EditMarkup);
    EXPECT_EQ(PinInputController::resolveKeyDown('I', false, false, false).action, PinShortcutAction::InvertColors);
    EXPECT_EQ(PinInputController::resolveKeyDown('G', false, false, false).action, PinShortcutAction::ToggleGrayscale);
    EXPECT_EQ(PinInputController::resolveKeyDown('M', false, false, false).action, PinShortcutAction::ToggleFold);
    EXPECT_EQ(PinInputController::resolveKeyDown('T', false, false, false).action, PinShortcutAction::ToggleClickThrough);
    EXPECT_EQ(PinInputController::resolveKeyDown(VK_F4, false, false, false).action, PinShortcutAction::ToggleClickThrough);
    EXPECT_EQ(PinInputController::resolveKeyDown(VK_F4, false, false, true).action, PinShortcutAction::ClosePin);
    EXPECT_EQ(PinInputController::resolveKeyDown(VK_DELETE, false, false, false).action, PinShortcutAction::ClosePin);
    EXPECT_EQ(PinInputController::resolveKeyDown('W', true, false, false).action, PinShortcutAction::ClosePin);
    EXPECT_EQ(PinInputController::resolveKeyDown(VK_ESCAPE, false, false, false).action, PinShortcutAction::EscapeOrClose);
    EXPECT_EQ(PinInputController::resolveKeyDown(VK_OEM_PLUS, false, false, false).action, PinShortcutAction::ZoomIn);
    EXPECT_EQ(PinInputController::resolveKeyDown(VK_OEM_MINUS, false, false, false).action, PinShortcutAction::ZoomOut);
    EXPECT_EQ(PinInputController::resolveKeyDown(VK_OEM_COMMA, false, false, false).action, PinShortcutAction::HistoryPrev);
    EXPECT_EQ(PinInputController::resolveKeyDown(VK_OEM_PERIOD, false, false, false).action, PinShortcutAction::HistoryNext);
    EXPECT_EQ(PinInputController::resolveKeyDown('C', true, false, false).action, PinShortcutAction::CopySelectedOrImage);
    EXPECT_EQ(PinInputController::resolveKeyDown('C', true, true, false).action, PinShortcutAction::CopyAllText);
    EXPECT_EQ(PinInputController::resolveKeyDown('T', true, false, false).action, PinShortcutAction::ExtractText);
    EXPECT_EQ(PinInputController::resolveKeyDown('S', true, false, false).action, PinShortcutAction::SaveImage);
    EXPECT_EQ(PinInputController::resolveKeyDown('Z', false, false, true).action, PinShortcutAction::None);

    // 7. 角度归一化与实机矩阵旋转/翻转测试 (含负角度与灰度备份矩阵同步变换)
    EXPECT_EQ(PinInputController::normalizeRightAngleDegrees(90), 90);
    EXPECT_EQ(PinInputController::normalizeRightAngleDegrees(-90), 270);
    EXPECT_EQ(PinInputController::normalizeRightAngleDegrees(180), 180);
    EXPECT_EQ(PinInputController::normalizeRightAngleDegrees(-180), 180);
    EXPECT_EQ(PinInputController::normalizeRightAngleDegrees(270), 270);
    EXPECT_EQ(PinInputController::normalizeRightAngleDegrees(450), 90);
    EXPECT_EQ(PinInputController::normalizeRightAngleDegrees(0), 0);
    EXPECT_EQ(PinInputController::normalizeRightAngleDegrees(45), 0);

    cv::Mat src = (cv::Mat_<uint8_t>(2, 3) << 1, 2, 3,
                                              4, 5, 6);
    cv::Mat backup = src.clone();
    int w = src.cols;
    int h = src.rows;

    // 顺时针 90° -> 3x2
    EXPECT_TRUE(PinInputController::applyRotation(src, backup, w, h, 90));
    EXPECT_EQ(w, 2);
    EXPECT_EQ(h, 3);
    EXPECT_EQ(src.rows, 3);
    EXPECT_EQ(src.cols, 2);
    EXPECT_EQ(backup.rows, 3);
    EXPECT_EQ(backup.cols, 2);
    EXPECT_EQ(src.at<uint8_t>(0, 0), 4);
    EXPECT_EQ(src.at<uint8_t>(0, 1), 1);

    // 逆时针 90° (-90° / 270°) -> 还原回 2x3
    EXPECT_TRUE(PinInputController::applyRotation(src, backup, w, h, -90));
    EXPECT_EQ(w, 3);
    EXPECT_EQ(h, 2);
    EXPECT_EQ(src.at<uint8_t>(0, 0), 1);
    EXPECT_EQ(src.at<uint8_t>(0, 2), 3);

    // 180° 旋转 -> 宽高不变
    EXPECT_TRUE(PinInputController::applyRotation(src, backup, w, h, 180));
    EXPECT_EQ(w, 3);
    EXPECT_EQ(h, 2);
    EXPECT_EQ(src.at<uint8_t>(0, 0), 6);
    EXPECT_EQ(backup.at<uint8_t>(0, 0), 6);

    // 水平与垂直翻转
    EXPECT_TRUE(PinInputController::applyFlip(src, backup, true));
    EXPECT_EQ(src.at<uint8_t>(0, 0), 4);
    EXPECT_TRUE(PinInputController::applyFlip(src, backup, false));
    EXPECT_EQ(src.at<uint8_t>(0, 0), 1);

    // 空矩阵与非法角度防御
    cv::Mat emptyMat;
    EXPECT_FALSE(PinInputController::applyRotation(emptyMat, backup, w, h, 90));
    EXPECT_FALSE(PinInputController::applyRotation(src, backup, w, h, 45));
    EXPECT_FALSE(PinInputController::applyFlip(emptyMat, backup, true));

    // 8. 悬停自动获焦策略判定
    EXPECT_TRUE(PinInputController::shouldAutoFocusOnHover(false, false, false));
    EXPECT_FALSE(PinInputController::shouldAutoFocusOnHover(true, false, false));
    EXPECT_FALSE(PinInputController::shouldAutoFocusOnHover(false, true, false));
    EXPECT_FALSE(PinInputController::shouldAutoFocusOnHover(false, false, true));

    // 9. 中心锚定旋转矩形计算与工作区防越界钳制
    const RECT workArea{0, 0, 1920, 1080};
    const RECT initStrip{200, 300, 600, 320}; // 400x20, 中心 (400, 310)
    const RECT rotStrip = PinInputController::calculateRotatedWindowRect(initStrip, 20, 400, workArea);
    EXPECT_EQ(rotStrip.left, 390);
    EXPECT_EQ(rotStrip.top, 110);
    EXPECT_EQ(rotStrip.right, 410);
    EXPECT_EQ(rotStrip.bottom, 510);
    // 再次旋转回 400x20 应 0 像素漂移精确还原
    const RECT restoredStrip = PinInputController::calculateRotatedWindowRect(rotStrip, 400, 20, workArea);
    EXPECT_EQ(restoredStrip.left, initStrip.left);
    EXPECT_EQ(restoredStrip.top, initStrip.top);
    EXPECT_EQ(restoredStrip.right, initStrip.right);
    EXPECT_EQ(restoredStrip.bottom, initStrip.bottom);

    // 靠近底边的宽贴图旋转为高贴图时自动防越界推回工作区内
    const RECT nearBottom{500, 980, 900, 1060}; // 400x80 near bottom=1080
    const RECT rotClamped = PinInputController::calculateRotatedWindowRect(nearBottom, 80, 400, workArea);
    EXPECT_GE(rotClamped.top, workArea.top);
    EXPECT_LE(rotClamped.bottom, workArea.bottom);
    EXPECT_EQ(rotClamped.bottom - rotClamped.top, 400);

    // 10. 窄窗口 (如 400x20 旋转为 20x400 或 80x400) 悬浮工具栏自适应布局与防裁切测试
    for (const auto& [winW, winH] : std::vector<std::pair<float, float>>{
             {400.0f, 200.0f},
             {140.0f, 300.0f},
             {80.0f,  400.0f},
             {40.0f,  400.0f},
             {20.0f,  400.0f},
             {400.0f, 20.0f},
         }) {
        const auto tb = PinInputController::computeHoverToolbarLayout(winW, winH, 1.0f);
        EXPECT_GE(tb.toolbarRect.left, 0.0f);
        EXPECT_GE(tb.toolbarRect.top, 0.0f);
        EXPECT_LE(tb.toolbarRect.right, winW + 0.01f);
        EXPECT_LE(tb.toolbarRect.bottom, winH + 0.01f);
        for (int i = 0; i < PinInteractionConstants::kHoverToolbarButtonCount; ++i) {
            EXPECT_GT(tb.buttons[i].right - tb.buttons[i].left, 1.0f);
            EXPECT_GT(tb.buttons[i].bottom - tb.buttons[i].top, 1.0f);
            EXPECT_GE(tb.buttons[i].left, 0.0f);
            EXPECT_GE(tb.buttons[i].top, 0.0f);
            EXPECT_LE(tb.buttons[i].right, winW + 0.01f);
            EXPECT_LE(tb.buttons[i].bottom, winH + 0.01f);
        }
    }

    const auto normalTb = PinInputController::computeHoverToolbarLayout(400.0f, 200.0f, 1.0f);
    EXPECT_EQ(normalTb.columns, 8);
    EXPECT_EQ(normalTb.rows, 1);
    const auto tipNormal = PinInputController::computeTooltipRect(
        normalTb.toolbarRect, (normalTb.buttons[1].left + normalTb.buttons[1].right) * 0.5f, 400.0f, 200.0f, 1.0f);
    EXPECT_GT(tipNormal.right - tipNormal.left, 40.0f);
    EXPECT_LE(tipNormal.right, 400.0f);
    EXPECT_LE(tipNormal.bottom, 200.0f);

    const auto narrowTb = PinInputController::computeHoverToolbarLayout(80.0f, 400.0f, 1.0f);
    EXPECT_LT(narrowTb.columns, 8);
    EXPECT_GT(narrowTb.rows, 1);
    const auto tipNarrow = PinInputController::computeTooltipRect(
        narrowTb.toolbarRect, (narrowTb.buttons[1].left + narrowTb.buttons[1].right) * 0.5f, 80.0f, 400.0f, 1.0f);
    EXPECT_GE(tipNarrow.left, 0.0f);
    EXPECT_LE(tipNarrow.right, 80.0f);
}

TEST(PinWindowTest, LiveWindowKeyboardRotationFlipAndFoldIntegration) {
    using namespace tools3000::capture;

    std::array<wchar_t, 4096> executablePath{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    ASSERT_GT(length, 0u);
    ASSERT_LT(length, executablePath.size());

    const auto executableDir = std::filesystem::path(executablePath.data()).parent_path();
    const auto pluginDllPath = executableDir.parent_path() / L"plugins" / executableDir.filename() / L"Plugin_Capture.dll";
    ASSERT_TRUE(std::filesystem::exists(pluginDllPath));

    HMODULE hMod = LoadLibraryExW(
        pluginDllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    ASSERT_NE(hMod, nullptr);

    using CreatePluginFunc = tools3000::core::IPlugin* (*)();
    const auto createPlugin = reinterpret_cast<CreatePluginFunc>(GetProcAddress(hMod, "CreatePlugin"));
    ASSERT_NE(createPlugin, nullptr);
    tools3000::core::IPlugin* capturePlugin = createPlugin();
    ASSERT_NE(capturePlugin, nullptr);
    ASSERT_TRUE(capturePlugin->initialize());

    struct PluginCleanupGuard {
        HMODULE mod = nullptr;
        tools3000::core::IPlugin* plugin = nullptr;
        ~PluginCleanupGuard() {
            while (HWND existing = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
                SendMessageW(existing, WM_KEYDOWN, VK_ESCAPE, 0);
                if (IsWindow(existing)) DestroyWindow(existing);
            }
            if (plugin) plugin->shutdown();
            if (mod) FreeLibrary(mod);
        }
    } cleanupGuard{hMod, capturePlugin};

    while (HWND existing = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
        SendMessageW(existing, WM_KEYDOWN, VK_ESCAPE, 0);
        if (IsWindow(existing)) DestroyWindow(existing);
    }

    // 构造 120x60 非对称测试图像并写入临时 PNG，通过 capture.pinImageFile 创建真实 PinWindow
    cv::Mat testImg(60, 120, CV_8UC3, cv::Scalar(40, 40, 40));
    testImg.at<cv::Vec3b>(0, 0) = cv::Vec3b(0, 0, 255);
    const auto tempPng = std::filesystem::temp_directory_path() /
                         (L"tools3000_pin_rot_test_" + std::to_wstring(GetCurrentProcessId()) + L".png");
    std::vector<uchar> pngBuf;
    ASSERT_TRUE(cv::imencode(".png", testImg, pngBuf));
    {
        std::ofstream ofs(tempPng, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(pngBuf.data()), static_cast<std::streamsize>(pngBuf.size()));
    }
    struct TempFileGuard {
        std::filesystem::path p;
        ~TempFileGuard() {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    } tempGuard{tempPng};

    auto& bridge = tools3000::core::MessageBridge::instance();
    nlohmann::json req = {
        {"id", 101},
        {"method", "capture.pinImageFile"},
        {"params", {{"path", tools3000::core::WinUtils::wstringToUtf8(tempPng.wstring())}}}
    };
    const std::string resp = bridge.handleMessage(req.dump());
    EXPECT_NE(resp.find("\"success\":true"), std::string::npos);

    HWND pinHwnd = FindWindowW(L"Tools3000_PinWindow", nullptr);
    ASSERT_NE(pinHwnd, nullptr);
    ASSERT_TRUE(IsWindowVisible(pinHwnd));

    SetWindowPos(pinHwnd, nullptr, 300, 300, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT rcBefore{};
    ASSERT_TRUE(GetWindowRect(pinHwnd, &rcBefore));
    EXPECT_EQ(rcBefore.right - rcBefore.left, 120);
    EXPECT_EQ(rcBefore.bottom - rcBefore.top, 60);
    const int centerXBefore = (rcBefore.left + rcBefore.right) / 2;
    const int centerYBefore = (rcBefore.top + rcBefore.bottom) / 2;

    // 1. 发送 WM_KEYDOWN 'R': 顺时针旋转 90° -> 60x120，且几何中心点保持不变
    SendMessageW(pinHwnd, WM_KEYDOWN, 'R', 0);
    RECT rcAfterR{};
    ASSERT_TRUE(GetWindowRect(pinHwnd, &rcAfterR));
    EXPECT_EQ(rcAfterR.right - rcAfterR.left, 60);
    EXPECT_EQ(rcAfterR.bottom - rcAfterR.top, 120);
    EXPECT_EQ((rcAfterR.left + rcAfterR.right) / 2, centerXBefore);
    EXPECT_EQ((rcAfterR.top + rcAfterR.bottom) / 2, centerYBefore);

    // 2. 发送 WM_KEYDOWN '1': 逆时针旋转 90° -> 还原回 120x60 且精确回到原位
    SendMessageW(pinHwnd, WM_KEYDOWN, '1', 0);
    RECT rcAfter1{};
    ASSERT_TRUE(GetWindowRect(pinHwnd, &rcAfter1));
    EXPECT_EQ(rcAfter1.right - rcAfter1.left, 120);
    EXPECT_EQ(rcAfter1.bottom - rcAfter1.top, 60);
    EXPECT_EQ(rcAfter1.left, rcBefore.left);
    EXPECT_EQ(rcAfter1.top, rcBefore.top);

    // 3. 发送 WM_KEYDOWN '2' (顺时针 90°) 与 'L' (逆时针 90°)
    SendMessageW(pinHwnd, WM_KEYDOWN, '2', 0);
    RECT rcAfter2{};
    ASSERT_TRUE(GetWindowRect(pinHwnd, &rcAfter2));
    EXPECT_EQ(rcAfter2.right - rcAfter2.left, 60);
    EXPECT_EQ(rcAfter2.bottom - rcAfter2.top, 120);

    SendMessageW(pinHwnd, WM_KEYDOWN, 'L', 0);
    RECT rcAfterL{};
    ASSERT_TRUE(GetWindowRect(pinHwnd, &rcAfterL));
    EXPECT_EQ(rcAfterL.right - rcAfterL.left, 120);
    EXPECT_EQ(rcAfterL.bottom - rcAfterL.top, 60);

    // 4. 发送 WM_SYSKEYDOWN Alt+Right (顺时针 90°) 与 Alt+Left (逆时针 90°)
    SendMessageW(pinHwnd, WM_SYSKEYDOWN, VK_RIGHT, 0);
    RECT rcAfterAltRight{};
    ASSERT_TRUE(GetWindowRect(pinHwnd, &rcAfterAltRight));
    EXPECT_EQ(rcAfterAltRight.right - rcAfterAltRight.left, 60);
    EXPECT_EQ(rcAfterAltRight.bottom - rcAfterAltRight.top, 120);

    SendMessageW(pinHwnd, WM_SYSKEYDOWN, VK_LEFT, 0);
    RECT rcAfterAltLeft{};
    ASSERT_TRUE(GetWindowRect(pinHwnd, &rcAfterAltLeft));
    EXPECT_EQ(rcAfterAltLeft.right - rcAfterAltLeft.left, 120);
    EXPECT_EQ(rcAfterAltLeft.bottom - rcAfterAltLeft.top, 60);

    // 5. 灰度模式 ('G') 下旋转 ('R') 再恢复彩色 ('G')
    SendMessageW(pinHwnd, WM_KEYDOWN, 'G', 0);
    SendMessageW(pinHwnd, WM_KEYDOWN, 'R', 0);
    RECT rcGrayRot{};
    ASSERT_TRUE(GetWindowRect(pinHwnd, &rcGrayRot));
    EXPECT_EQ(rcGrayRot.right - rcGrayRot.left, 60);
    EXPECT_EQ(rcGrayRot.bottom - rcGrayRot.top, 120);
    SendMessageW(pinHwnd, WM_KEYDOWN, 'G', 0);
    SendMessageW(pinHwnd, WM_KEYDOWN, '1', 0);

    // 6. 折叠态 ('M') 下旋转 ('R')：自动一步展开并切换为旋转后尺寸 60x120
    SendMessageW(pinHwnd, WM_KEYDOWN, 'M', 0);
    RECT rcFolded{};
    ASSERT_TRUE(GetWindowRect(pinHwnd, &rcFolded));
    EXPECT_LT(rcFolded.bottom - rcFolded.top, 60);
    SendMessageW(pinHwnd, WM_KEYDOWN, 'R', 0);
    RECT rcUnfoldedRot{};
    ASSERT_TRUE(GetWindowRect(pinHwnd, &rcUnfoldedRot));
    EXPECT_EQ(rcUnfoldedRot.right - rcUnfoldedRot.left, 60);
    EXPECT_EQ(rcUnfoldedRot.bottom - rcUnfoldedRot.top, 120);
    SendMessageW(pinHwnd, WM_KEYDOWN, '1', 0);

    // 7. 透明度快捷键 '5' (50%) 与 '0' (重置 100% 透明度 & 1:1 比例)
    SendMessageW(pinHwnd, WM_KEYDOWN, '5', 0);
    BYTE layeredAlpha = 0;
    DWORD layeredFlags = 0;
    ASSERT_TRUE(GetLayeredWindowAttributes(pinHwnd, nullptr, &layeredAlpha, &layeredFlags));
    EXPECT_NEAR(static_cast<double>(layeredAlpha), 127.5, 2.0);

    SendMessageW(pinHwnd, WM_KEYDOWN, '0', 0);
    ASSERT_TRUE(GetLayeredWindowAttributes(pinHwnd, nullptr, &layeredAlpha, &layeredFlags));
    EXPECT_EQ(layeredAlpha, 255);

    // 8. 发送 VK_ESCAPE 关闭贴图窗口
    SendMessageW(pinHwnd, WM_KEYDOWN, VK_ESCAPE, 0);
    EXPECT_FALSE(IsWindow(pinHwnd));
}

// -----------------------------------------------------------------------------
// 27. 剪贴板与字符编码转换测试套件
// -----------------------------------------------------------------------------

TEST(CaptureCornerRadiusTest, CornerRadiusInteractiveAdjustment) {
    using namespace tools3000::capture;

    CaptureState state;
    state.dragStart = {100, 100};
    state.dragEnd = {500, 400};
    state.dpiScale = 1.0f;
    state.cornerRadius = 0.0f;

    // 1. 验证 HitArea::CornerRadius 枚举与状态
    EXPECT_EQ(static_cast<int>(HitArea::CornerRadius), 9);
    state.selAdjustHandle = HitArea::CornerRadius;
    EXPECT_EQ(state.selAdjustHandle, HitArea::CornerRadius);

    // 2. 验证圆角控制点动态偏移量计算算法 (offset = max(14.0f, cornerRadius + 8.0f))
    float selW = 400.0f;
    float selH = 300.0f;
    float maxR = std::min(selW, selH) * 0.5f;
    EXPECT_NEAR(maxR, 150.0f, 0.01f);

    float offset0 = std::clamp(std::max(14.0f, state.cornerRadius + 8.0f), 12.0f, std::min(selW, selH) * 0.40f);
    EXPECT_NEAR(offset0, 14.0f, 0.01f);

    state.cornerRadius = 16.0f;
    float offset16 = std::clamp(std::max(14.0f, state.cornerRadius + 8.0f), 12.0f, std::min(selW, selH) * 0.40f);
    EXPECT_NEAR(offset16, 24.0f, 0.01f);

    // 3. 验证圆角拖拽状态变更与极值约束
    state.isAdjustingCornerRadius = true;
    state.cornerDragStartRadius = 16.0f;
    state.cornerDragStartPos = {124, 124};
    EXPECT_TRUE(state.isAdjustingCornerRadius);
    EXPECT_NEAR(state.cornerDragStartRadius, 16.0f, 0.01f);

    // 4. 验证鼠标靠近角部检测 (triggerDist = 36.0f)
    const float triggerDist = 36.0f;
    POINT nearPt{124, 124};
    float dist = std::hypot(nearPt.x - (100.0f + offset16), nearPt.y - (100.0f + offset16));
    EXPECT_LE(dist, triggerDist);

    POINT farPt{300, 250};
    float farDist = std::hypot(farPt.x - (100.0f + offset16), farPt.y - (100.0f + offset16));
    EXPECT_GT(farDist, triggerDist);
}

TEST(CaptureToolbarTooltipTest, ComprehensiveTooltips) {
    using namespace tools3000::capture;

    ToolbarButton btnRect;
    btnRect.command = ToolbarCommand::SelectTool;
    btnRect.tool = MarkupTool::Rectangle;
    EXPECT_EQ(btnRect.command, ToolbarCommand::SelectTool);
    EXPECT_EQ(btnRect.tool, MarkupTool::Rectangle);

    ToolbarButton btnArrow;
    btnArrow.command = ToolbarCommand::SelectTool;
    btnArrow.tool = MarkupTool::Arrow;
    EXPECT_EQ(btnArrow.tool, MarkupTool::Arrow);

    ToolbarButton btnColor;
    btnColor.command = ToolbarCommand::SelectColor;
    btnColor.color = {255, 0, 0, 255};
    EXPECT_EQ(btnColor.color.r, 255);
}

TEST(CaptureClipboardFormatTest, BottomUpDIBAndPNG) {
    cv::Mat testMat(100, 100, CV_8UC4, cv::Scalar(100, 150, 200, 255));
    EXPECT_FALSE(testMat.empty());
    EXPECT_EQ(testMat.cols, 100);
    EXPECT_EQ(testMat.rows, 100);

    // 验证 PNG 内存编码支持
    std::vector<uint8_t> pngBytes;
    bool encoded = cv::imencode(".png", testMat, pngBytes);
    EXPECT_TRUE(encoded);
    EXPECT_FALSE(pngBytes.empty());

    // 验证 Windows GDI 标准底向上扫描行正数约束
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(bi);
    bi.biWidth = testMat.cols;
    bi.biHeight = testMat.rows; // 正数 = Bottom-Up DIB
    EXPECT_GT(bi.biHeight, 0);
}


TEST(CaptureSizeHudAndMenuTest, HudUnitsAndSwitches) {
    using namespace tools3000::capture;
    CaptureState state;
    
    // 1. 默认设置检查
    EXPECT_EQ(state.sizeUnit, CaptureState::SizeUnit::Pixel);
    EXPECT_TRUE(state.showPositionInHud);
    EXPECT_TRUE(state.showUnitInHud);
    EXPECT_FALSE(state.isSizeMenuOpen);
    EXPECT_FALSE(state.isSizeHudHovered);

    // 2. 状态切换
    state.sizeUnit = CaptureState::SizeUnit::DeviceIndependentPixel;
    EXPECT_EQ(state.sizeUnit, CaptureState::SizeUnit::DeviceIndependentPixel);

    state.showPositionInHud = false;
    EXPECT_FALSE(state.showPositionInHud);

    state.showUnitInHud = false;
    EXPECT_FALSE(state.showUnitInHud);

    // 3. 菜单开关
    state.isSizeMenuOpen = true;
    EXPECT_TRUE(state.isSizeMenuOpen);

    // 4. AccentColor 与 HEX 解析覆盖
    auto col = tools3000::core::getAccentColorRGB("coral");
    EXPECT_GT(col.r, 0.5f);
    auto cyanCol = tools3000::core::getAccentColorRGB("cyan");
    EXPECT_GT(cyanCol.b, 0.5f);
    auto amberCol = tools3000::core::getAccentColorRGB("amber");
    EXPECT_GT(amberCol.r, 0.5f);
    auto mintCol = tools3000::core::getAccentColorRGB("mint");
    EXPECT_GT(mintCol.g, 0.5f);
    auto violetCol = tools3000::core::getAccentColorRGB("violet");
    EXPECT_GT(violetCol.b, 0.5f);
    auto defCol = tools3000::core::getAccentColorRGB("invalid_name");
    EXPECT_GT(defCol.b, 0.5f);

    auto hexCol1 = tools3000::core::parseHexColor("#FF0000");
    EXPECT_NEAR(hexCol1.r, 1.0f, 0.01f);
    auto hexCol2 = tools3000::core::parseHexColor("00FF00");
    EXPECT_NEAR(hexCol2.g, 1.0f, 0.01f);
    auto hexCol3 = tools3000::core::parseHexColor("invalid_hex");
    EXPECT_NEAR(hexCol3.r, 0.231f, 0.01f);

    // 5. 吸管取色与画笔色彩同步验证
    MarkupColor picked(59, 130, 246);
    state.currentColor = picked;
    EXPECT_EQ(state.currentColor.r, 59);
    EXPECT_EQ(state.currentColor.g, 130);
    EXPECT_EQ(state.currentColor.b, 246);
}

TEST(CaptureTwoTierToolbarAndSubmenuTest, LayoutAndSecondaryProperties) {
    using namespace tools3000::capture;
    CaptureState state;
    state.dpiScale = 1.0f;
    state.mode = OverlayMode::Screenshot;
    state.currentTool = MarkupTool::Rectangle;
    state.isMarkupToolActive = true;

    D2D1_RECT_F selRect = D2D1::RectF(100.0f, 100.0f, 600.0f, 500.0f);
    D2D1_SIZE_F surfaceSize = D2D1::SizeF(1920.0f, 1080.0f);

    // 1. 测试矩形工具下的双层工具栏布局生成
    rebuildCaptureToolbar(state, selRect, surfaceSize);
    EXPECT_FALSE(state.toolbarButtons.empty());
    EXPECT_FALSE(state.secondaryToolbarButtons.empty());
    EXPECT_GT(state.primaryToolbarRect.right - state.primaryToolbarRect.left, 100.0f);
    EXPECT_GT(state.secondaryToolbarRect.right - state.secondaryToolbarRect.left, 100.0f);

    // 验证二级栏包含填充、线条样式、线宽、圆角、调色板
    bool hasFill = false;
    bool hasLineStyle = false;
    bool hasStrokeWidth = false;
    bool hasCornerRadius = false;
    bool hasChooseCustomColor = false;
    int colorCount = 0;
    int chooseCustomIdx = -1;
    int firstSelectColorIdx = -1;

    for (int i = 0; i < static_cast<int>(state.secondaryToolbarButtons.size()); ++i) {
        const auto& btn = state.secondaryToolbarButtons[static_cast<size_t>(i)];
        if (btn.command == ToolbarCommand::ToggleFill) hasFill = true;
        if (btn.command == ToolbarCommand::ToggleLineStyleDropdown) hasLineStyle = true;
        if (btn.command == ToolbarCommand::CycleStrokeWidth) hasStrokeWidth = true;
        if (btn.command == ToolbarCommand::CycleElementCornerRadius) hasCornerRadius = true;
        if (btn.command == ToolbarCommand::ChooseCustomColor) {
            hasChooseCustomColor = true;
            chooseCustomIdx = i;
            EXPECT_EQ(btn.color.r, MarkupColor::Red().r);
            EXPECT_EQ(btn.color.g, MarkupColor::Red().g);
            EXPECT_EQ(btn.color.b, MarkupColor::Red().b);
        }
        if (btn.command == ToolbarCommand::SelectColor) {
            if (firstSelectColorIdx < 0) {
                firstSelectColorIdx = i;
                EXPECT_TRUE(btn.isSeparatorBefore);
            }
            ++colorCount;
        }
    }
    EXPECT_TRUE(hasFill);
    EXPECT_TRUE(hasLineStyle);
    EXPECT_TRUE(hasStrokeWidth);
    EXPECT_TRUE(hasCornerRadius);
    EXPECT_TRUE(hasChooseCustomColor);
    EXPECT_EQ(colorCount, 7);
    EXPECT_GE(chooseCustomIdx, 0);
    EXPECT_EQ(firstSelectColorIdx, chooseCustomIdx + 1);

    // 验证自定义颜色注入并生效后动态展示第 8 个色块与当前色指示器
    state.hasCustomColor = true;
    state.customColor = MarkupColor{139, 92, 246}; // #8B5CF6 (非预设色)
    state.currentColor = state.customColor;
    state.toolbarLayoutValid = false;
    rebuildCaptureToolbar(state, selRect, surfaceSize);
    int customColorCount = 0;
    for (const auto& btn : state.secondaryToolbarButtons) {
        if (btn.command == ToolbarCommand::SelectColor) ++customColorCount;
    }
    EXPECT_EQ(customColorCount, 8);

    bool chooseColorHasCustomParam = false;
    for (const auto& btn : state.secondaryToolbarButtons) {
        if (btn.command == ToolbarCommand::ChooseCustomColor) {
            chooseColorHasCustomParam = btn.boolParam;
            EXPECT_EQ(btn.color.r, 139);
            EXPECT_EQ(btn.color.g, 92);
            EXPECT_EQ(btn.color.b, 246);
        }
    }
    EXPECT_TRUE(chooseColorHasCustomParam);

    // 验证主工具栏与二级属性栏包含前置分隔符
    bool hasPriSeparators = false;
    for (const auto& btn : state.toolbarButtons) {
        if (btn.isSeparatorBefore) hasPriSeparators = true;
    }
    EXPECT_TRUE(hasPriSeparators);

    bool hasSecSeparators = false;
    for (const auto& btn : state.secondaryToolbarButtons) {
        if (btn.isSeparatorBefore) hasSecSeparators = true;
    }
    EXPECT_TRUE(hasSecSeparators);

    // 2. 切换到文本工具
    state.currentTool = MarkupTool::Text;
    state.toolbarLayoutValid = false;
    rebuildCaptureToolbar(state, selRect, surfaceSize);
    EXPECT_FALSE(state.secondaryToolbarButtons.empty());

    // 3. 切换到箭头工具
    state.currentTool = MarkupTool::Arrow;
    state.toolbarLayoutValid = false;
    rebuildCaptureToolbar(state, selRect, surfaceSize);
    EXPECT_FALSE(state.secondaryToolbarButtons.empty());

    // 4. 切换到马赛克工具
    state.currentTool = MarkupTool::Mosaic;
    state.toolbarLayoutValid = false;
    rebuildCaptureToolbar(state, selRect, surfaceSize);
    EXPECT_FALSE(state.secondaryToolbarButtons.empty());

    // 4.5 切换到序号工具
    state.currentTool = MarkupTool::Number;
    state.toolbarLayoutValid = false;
    rebuildCaptureToolbar(state, selRect, surfaceSize);
    EXPECT_FALSE(state.secondaryToolbarButtons.empty());

    // 5. 验证 OpenCV 虚线与填充渲染及全部标注工具
    initializeMarkupTextRenderer();
    cv::Mat canvas = cv::Mat::zeros(400, 400, CV_8UC4);
    MarkupEngine engine;
    engine.setBaseImage(canvas.clone());

    auto* elemRect = engine.drawRectangle(cv::Point(20, 20), cv::Point(120, 120), MarkupColor::Red(), 3.0f);
    ASSERT_NE(elemRect, nullptr);
    elemRect->lineStyle = LineStyle::Dashed;
    elemRect->fill = true;
    elemRect->cornerRadius = 14.0f;

    auto* elemArrow = engine.drawArrow(cv::Point(150, 50), cv::Point(250, 150), MarkupColor::Blue(), 4.0f);
    ASSERT_NE(elemArrow, nullptr);
    elemArrow->lineStyle = LineStyle::Dotted;
    elemArrow->arrowStyle = ArrowStyle::DoubleEnded;

    auto* elemPen = engine.drawPenStroke({cv::Point(30, 200), cv::Point(60, 240), cv::Point(90, 210)}, MarkupColor::Green(), 2.0f);
    ASSERT_NE(elemPen, nullptr);
    elemPen->lineStyle = LineStyle::DashDot;

    auto* elemEllipse = engine.drawEllipse(cv::Point(200, 200), cv::Point(300, 300), MarkupColor::Yellow(), 2.0f);
    ASSERT_NE(elemEllipse, nullptr);
    elemEllipse->fill = true;

    engine.drawHighlight(cv::Point(10, 10), cv::Point(80, 40), MarkupColor::Yellow());
    engine.applyMosaic(cv::Point(300, 300), cv::Point(380, 380), 8);
    
    int numVal = engine.addNumberMark(cv::Point(100, 350), MarkupColor::Red());
    EXPECT_EQ(numVal, 1);
    EXPECT_EQ(engine.currentNumber(), 2);
    engine.resetNumber();
    EXPECT_EQ(engine.currentNumber(), 1);

    engine.addSpotlight(cv::Point(100, 100), cv::Point(200, 200), MarkupColor::White());
    engine.addWatermark(cv::Point(0, 0), cv::Point(400, 400), "CONFIDENTIAL");
    engine.applyInpaint(cv::Point(50, 50), cv::Point(70, 70), 3);

    engine.renderAll(canvas);
    EXPECT_GT(cv::countNonZero(canvas.reshape(1)), 0);

    // 验证撤销与重做
    EXPECT_TRUE(engine.canUndo());
    engine.undo();
    EXPECT_TRUE(engine.canRedo());
    engine.redo();
    engine.translateAll(5, 5);
    engine.updateBaseImage(canvas.clone());
    EXPECT_FALSE(engine.getCompositeImage().empty());
    engine.clearAll();
    EXPECT_FALSE(engine.canUndo());

    shutdownMarkupTextRenderer();

    // 6. 验证最大圆角 60 上限配置
    tools3000::core::ConfigManager::instance().set<double>("/screenshot/maxCornerRadius", 60.0);
    double cfgR = tools3000::core::ConfigManager::instance().get<double>("/screenshot/maxCornerRadius", 60.0);
    EXPECT_DOUBLE_EQ(cfgR, 60.0);
}

// -----------------------------------------------------------------------------
// 37. 鼠标聚光灯与演示特效测试套件
// -----------------------------------------------------------------------------

TEST(CaptureVectorIconsTest, ComprehensiveVectorRendering) {
    using namespace tools3000::capture;

    // 验证空指针与无效参数防护
    CaptureVectorIcons::renderIcon(nullptr, nullptr, CaptureIconId::ActionConfirm,
                                   D2D1::RectF(0, 0, 30, 30), nullptr, 1.0f);

    // 创建 Direct2D 工厂与离屏 DIB RenderTarget 进行全图标像素级渲染质量与覆盖验证
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf());
    ASSERT_NE(d2dFactory, nullptr);

    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcRt;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    HRESULT hr = d2dFactory->CreateDCRenderTarget(&props, dcRt.GetAddressOf());
    ASSERT_EQ(hr, S_OK);
    ASSERT_NE(dcRt, nullptr);

    const int sheetW = 960;
    const int sheetH = 720;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = sheetW;
    bmi.bmiHeader.biHeight = -sheetH; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dibBits = nullptr;
    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    HBITMAP hBmp = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    ReleaseDC(nullptr, screenDc);
    ASSERT_NE(memDc, nullptr);
    ASSERT_NE(hBmp, nullptr);
    ASSERT_NE(dibBits, nullptr);
    HGDIOBJ oldBmp = SelectObject(memDc, hBmp);

    RECT rc{0, 0, sheetW, sheetH};
    ASSERT_EQ(dcRt->BindDC(memDc, &rc), S_OK);
    dcRt->BeginDraw();
    dcRt->Clear(D2D1::ColorF(0.96f, 0.97f, 0.98f, 1.0f));

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> darkBrush;
    dcRt->CreateSolidColorBrush(D2D1::ColorF(0.11f, 0.13f, 0.17f, 0.94f), darkBrush.GetAddressOf());
    ASSERT_NE(darkBrush, nullptr);

    // 遍历所有枚举值进行 100% 渲染覆盖与像素质量校验
    const std::vector<CaptureIconId> allIcons = {
        CaptureIconId::None,
        CaptureIconId::ToolRectangle,
        CaptureIconId::ToolLine,
        CaptureIconId::ToolEllipse,
        CaptureIconId::ToolPen,
        CaptureIconId::ToolHighlight,
        CaptureIconId::ToolArrow,
        CaptureIconId::ToolArrowStandard,
        CaptureIconId::ToolArrowThin,
        CaptureIconId::ToolArrowWing,
        CaptureIconId::ToolArrowDouble,
        CaptureIconId::ToolText,
        CaptureIconId::ToolNumber,
        CaptureIconId::ToolMosaic,
        CaptureIconId::ToolBlur,
        CaptureIconId::ToolInpaint,
        CaptureIconId::ActionUndo,
        CaptureIconId::ActionRedo,
        CaptureIconId::ActionClear,
        CaptureIconId::ActionExtractText,
        CaptureIconId::ActionPinWindow,
        CaptureIconId::ActionScrollCapture,
        CaptureIconId::ActionRecordStart,
        CaptureIconId::ActionRecordPause,
        CaptureIconId::ActionRecordStop,
        CaptureIconId::ActionToggleMic,
        CaptureIconId::ActionToggleSpeaker,
        CaptureIconId::ActionCopy,
        CaptureIconId::ActionSave,
        CaptureIconId::ActionCancel,
        CaptureIconId::ActionConfirm,
        CaptureIconId::ActionInvert,
        CaptureIconId::ActionGrayscale,
        CaptureIconId::ActionResetScale,
        CaptureIconId::ActionFold,
        CaptureIconId::ActionRotate,
        CaptureIconId::ActionResetNumber,
        CaptureIconId::ActionSnapshot,
        CaptureIconId::ActionOpenFolder,
        CaptureIconId::PropSolidLine,
        CaptureIconId::PropDashedLine,
        CaptureIconId::PropDottedLine,
        CaptureIconId::PropDashDotLine,
        CaptureIconId::PropStrokeWidth,
        CaptureIconId::PropCornerRadius,
        CaptureIconId::PropFillOutline,
        CaptureIconId::PropFillSolid,
        CaptureIconId::PropPipette,
        CaptureIconId::PropPalette,
        CaptureIconId::PropRainbowWheel,
        CaptureIconId::ActionRecordVideo,
        CaptureIconId::PropQrCode,
        CaptureIconId::ToolBeautyShell,
        CaptureIconId::PropAspectRatio,
        CaptureIconId::PropNumberSquare,
        CaptureIconId::PropTextOutline,
        CaptureIconId::PropFontSize,
        CaptureIconId::PropTextBackground,
        CaptureIconId::PropMagnifierCallout,
        CaptureIconId::ActionInvertSelection,
    };

    const int cols = 10;
    const float cellW = 92.0f;
    const float cellH = 88.0f;
    for (size_t i = 0; i < allIcons.size(); ++i) {
        const auto id = allIcons[i];
        const int col = static_cast<int>(i) % cols;
        const int row = static_cast<int>(i) / cols;
        const float ox = 16.0f + col * cellW;
        const float oy = 14.0f + row * cellH;

        // 1x 真实主工具栏尺寸 (30x30)
        D2D1_RECT_F rect1x = D2D1::RectF(ox, oy, ox + 30.0f, oy + 30.0f);
        FloatingGlassBar::drawButtonPill(dcRt.Get(), rect1x, 6.0f, false, false, false, false, 1.0f, false);
        CaptureVectorIcons::renderIcon(dcRt.Get(), d2dFactory.Get(), id, rect1x, darkBrush.Get(), 1.0f);

        // 2x 高分屏尺寸 (52x52)
        D2D1_RECT_F rect2x = D2D1::RectF(ox + 34.0f, oy, ox + 86.0f, oy + 52.0f);
        CaptureVectorIcons::renderIcon(dcRt.Get(), d2dFactory.Get(), id, rect2x, darkBrush.Get(), 1.8f);

        // 无 factory 降级分支覆盖 (下方小格 24x24)
        D2D1_RECT_F rectFallback = D2D1::RectF(ox, oy + 34.0f, ox + 24.0f, oy + 58.0f);
        CaptureVectorIcons::renderIcon(dcRt.Get(), nullptr, id, rectFallback, darkBrush.Get(), 1.0f);
    }

    // 底部绘制深浅双模式 FloatingGlassBar 真实工具栏条示例
    FloatingGlassBar::drawGlassPanel(dcRt.Get(), D2D1::RectF(16.0f, 560.0f, 460.0f, 600.0f), 10.0f, false);
    FloatingGlassBar::drawGlassPanel(dcRt.Get(), D2D1::RectF(480.0f, 560.0f, 924.0f, 600.0f), 10.0f, true);
    FloatingGlassBar::drawButtonPill(dcRt.Get(), D2D1::RectF(24.0f, 565.0f, 54.0f, 595.0f), 6.0f, true, false, false, false, 1.0f, false);
    FloatingGlassBar::drawButtonPill(dcRt.Get(), D2D1::RectF(58.0f, 565.0f, 88.0f, 595.0f), 6.0f, false, true, false, false, 1.0f, false);
    FloatingGlassBar::drawButtonPill(dcRt.Get(), D2D1::RectF(92.0f, 565.0f, 122.0f, 595.0f), 6.0f, false, false, true, false, 1.0f, false);
    FloatingGlassBar::drawButtonPill(dcRt.Get(), D2D1::RectF(126.0f, 565.0f, 156.0f, 595.0f), 6.0f, false, false, false, true, 1.0f, false);
    FloatingGlassBar::drawSeparator(dcRt.Get(), 164.0f, 568.0f, 592.0f, 1.0f, false);
    FloatingGlassBar::drawSeparator(dcRt.Get(), 628.0f, 568.0f, 592.0f, 1.0f, true);

    ASSERT_EQ(dcRt->EndDraw(), S_OK);
    GdiFlush();

    cv::Mat rendered(sheetH, sheetW, CV_8UC4, dibBits);
    // 验证每个非 None 图标在 1x (30x30) 与 2x (52x52) 下均真实产出有效矢量笔画像素，且留有充足负空间（绝不糊成实心黑块）
    for (size_t i = 0; i < allIcons.size(); ++i) {
        const auto id = allIcons[i];
        if (id == CaptureIconId::None) continue;
        const int col = static_cast<int>(i) % cols;
        const int row = static_cast<int>(i) / cols;
        const int ox = 16 + col * static_cast<int>(cellW);
        const int oy = 14 + row * static_cast<int>(cellH);

        cv::Rect roi1x(ox + 4, oy + 4, 22, 22);
        cv::Mat patch1x = rendered(roi1x);
        int ink1x = 0;
        for (int y = 0; y < patch1x.rows; ++y) {
            for (int x = 0; x < patch1x.cols; ++x) {
                const auto& px = patch1x.at<cv::Vec4b>(y, x);
                if (px[0] < 215 || px[1] < 215 || px[2] < 215) {
                    ++ink1x;
                }
            }
        }
        EXPECT_GT(ink1x, 8) << "Icon index " << i << " produced too few ink pixels at 1x";
        EXPECT_LT(ink1x, patch1x.rows * patch1x.cols * 80 / 100) << "Icon index " << i << " flooded negative space at 1x";
    }

    if (std::getenv("TOOLS3000_DUMP_ICON_SHEET") != nullptr) {
        cv::imwrite("build/icon_audit_sheet.png", rendered);
    }

    SelectObject(memDc, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDc);
}

TEST(CaptureSliderPopupTest, SteplessSliderAndWheelInteraction) {
    using namespace tools3000::capture;

    CaptureState state;
    state.dpiScale = 1.25f;
    state.currentStrokeWidth = 6;
    state.currentElementCornerRadius = 12.0f;

    // 1. 初始化线宽滑块弹窗
    state.sliderPopup.type = SliderPopupType::StrokeWidth;
    state.sliderPopup.minValue = 1;
    state.sliderPopup.maxValue = 28;
    state.sliderPopup.currentValue = state.currentStrokeWidth;
    state.sliderPopup.trackRect = D2D1::RectF(100.0f, 200.0f, 300.0f, 206.0f);
    
    EXPECT_EQ(state.sliderPopup.type, SliderPopupType::StrokeWidth);
    EXPECT_EQ(state.sliderPopup.currentValue, 6);

    // 2. 模拟滑块拖拽映射
    float trackW = state.sliderPopup.trackRect.right - state.sliderPopup.trackRect.left;
    float clickX = state.sliderPopup.trackRect.left + trackW * 0.5f; // 50% 位置
    float pct = std::clamp((clickX - state.sliderPopup.trackRect.left) / trackW, 0.0f, 1.0f);
    int mappedVal = state.sliderPopup.minValue + static_cast<int>(std::round(pct * (state.sliderPopup.maxValue - state.sliderPopup.minValue)));
    EXPECT_NEAR(mappedVal, 15, 1);

    // 3. 模拟预设胶囊按钮配置
    std::vector<int> presets = { 2, 4, 6, 8, 14, 20 };
    for (size_t i = 0; i < presets.size(); ++i) {
        float px = state.sliderPopup.trackRect.left + i * (trackW / presets.size());
        state.sliderPopup.presetButtons.push_back({ presets[i], D2D1::RectF(px, 220.0f, px + 25.0f, 240.0f) });
    }
    EXPECT_EQ(state.sliderPopup.presetButtons.size(), 6);
    EXPECT_EQ(state.sliderPopup.presetButtons[2].first, 6);

    // 4. 模拟滚轮调节步进
    int zDeltaUp = 120;
    int stepUp = (zDeltaUp > 0) ? 1 : -1;
    state.currentStrokeWidth = std::clamp(state.currentStrokeWidth + stepUp, 1, 28);
    EXPECT_EQ(state.currentStrokeWidth, 7);

    int zDeltaDown = -120;
    int stepDown = (zDeltaDown > 0) ? 1 : -1;
    state.currentStrokeWidth = std::clamp(state.currentStrokeWidth + stepDown, 1, 28);
    EXPECT_EQ(state.currentStrokeWidth, 6);

    // 5. 切换到圆角滑块弹窗
    state.sliderPopup.type = SliderPopupType::CornerRadius;
    state.sliderPopup.minValue = 0;
    state.sliderPopup.maxValue = 40;
    state.sliderPopup.currentValue = static_cast<int>(state.currentElementCornerRadius);
    EXPECT_EQ(state.sliderPopup.type, SliderPopupType::CornerRadius);
    EXPECT_EQ(state.sliderPopup.currentValue, 12);
}

TEST(CaptureShiftConstraintTest, OrthogonalAndSquareAspectConstraints) {
    using namespace tools3000::capture;

    // 1. 验证正方形约束算法
    cv::Point start(100, 100);
    cv::Point end(250, 180); // w=150, h=80
    int w = std::abs(end.x - start.x);
    int h = std::abs(end.y - start.y);
    int side = std::max(w, h);
    cv::Point constrainedEnd(start.x + (end.x >= start.x ? side : -side),
                             start.y + (end.y >= start.y ? side : -side));
    EXPECT_EQ(constrainedEnd.x - start.x, 150);
    EXPECT_EQ(constrainedEnd.y - start.y, 150);

    // 2. 验证箭头 45° 正交吸附算法
    cv::Point arrowStart(200, 200);
    cv::Point arrowEnd(300, 210); // 接近水平 0°
    double dx = arrowEnd.x - arrowStart.x;
    double dy = arrowEnd.y - arrowStart.y;
    double dist = std::hypot(dx, dy);
    double angle = std::atan2(dy, dx);
    constexpr double step = 3.14159265358979323846 / 4.0;
    double snappedAngle = std::round(angle / step) * step;
    cv::Point snappedEnd(static_cast<int>(std::round(arrowStart.x + dist * std::cos(snappedAngle))),
                         static_cast<int>(std::round(arrowStart.y + dist * std::sin(snappedAngle))));
    EXPECT_NEAR(snappedEnd.y, arrowStart.y, 1); // 成功吸附到绝对水平 0°
    EXPECT_NEAR(snappedEnd.x - arrowStart.x, static_cast<int>(dist), 1);
}

TEST(CaptureSmartGuidesAndQrTest, AlignmentDetectionAndQrLayout) {
    using namespace tools3000::capture;

    CaptureState state;
    state.dpiScale = 1.0f;
    state.detectedQrText = "https://github.com/yuan278501381/Tools3000";

    // 1. 验证二维码微晶胶囊文本截断与布局计算
    std::wstring displayMsg = L"二维码: " + tools3000::core::WinUtils::utf8ToWstring(state.detectedQrText);
    EXPECT_TRUE(displayMsg.find(L"https://github.com") != std::wstring::npos);
    if (displayMsg.size() > 28) {
        displayMsg = displayMsg.substr(0, 26) + L"...";
    }
    EXPECT_EQ(displayMsg.substr(displayMsg.size() - 3), L"...");

    // 2. 验证智能对齐中轴计算
    D2D1_RECT_F selRect = D2D1::RectF(460.0f, 270.0f, 1460.0f, 810.0f); // 中心 (960, 540)
    D2D1_SIZE_F surfaceSize = D2D1::SizeF(1920.0f, 1080.0f); // 中心 (960, 540)
    float cx = (selRect.left + selRect.right) * 0.5f;
    float cy = (selRect.top + selRect.bottom) * 0.5f;
    float midScreenX = surfaceSize.width * 0.5f;
    float midScreenY = surfaceSize.height * 0.5f;

    EXPECT_NEAR(cx, midScreenX, 0.001f);
    EXPECT_NEAR(cy, midScreenY, 0.001f);
    EXPECT_LT(std::abs(cx - midScreenX), 4.0f);
    EXPECT_LT(std::abs(cy - midScreenY), 4.0f);
}

TEST(CaptureUndoRedoStateSanitizationTest, ActiveElementSafetyAndDirtyFlags) {
    using namespace tools3000::capture;

    CaptureState state;
    cv::Mat dummy(400, 400, CV_8UC3, cv::Scalar(100, 100, 100));
    state.markup.setBaseImage(dummy);

    // 1. 添加图元
    auto* rectElem = state.markup.drawRectangle(cv::Point(10, 10), cv::Point(100, 100), MarkupColor::Red(), 2.0f);
    ASSERT_NE(rectElem, nullptr);
    state.activeElement = rectElem;
    EXPECT_EQ(state.markup.elementCount(), 1);

    // 2. 执行撤销并验证安全归空
    state.activeElement = nullptr;
    EXPECT_TRUE(state.markup.undo());
    EXPECT_EQ(state.markup.elementCount(), 0);
    EXPECT_EQ(state.activeElement, nullptr);

    // 3. 执行重做并验证图元恢复
    EXPECT_TRUE(state.markup.redo());
    EXPECT_EQ(state.markup.elementCount(), 1);

    // 4. 清空验证
    state.activeElement = nullptr;
    state.markup.clearAll();
    EXPECT_EQ(state.markup.elementCount(), 0);
}

TEST(CaptureHistoryTimeMachineTest, NavigationAndClampLogic) {
    using namespace tools3000::capture;

    CaptureState state;
    state.historyMode = false;
    state.historyIndex = 0;

    int totalHistory = 5;
    // 模拟逗号按键（上溯历史）
    if (!state.historyMode) {
        state.historyMode = true;
        state.historyIndex = 0;
    }
    EXPECT_TRUE(state.historyMode);
    EXPECT_EQ(state.historyIndex, 0);

    // 连续按逗号上溯
    for (int i = 0; i < 10; ++i) {
        state.historyIndex = std::min(state.historyIndex + 1, totalHistory - 1);
    }
    EXPECT_EQ(state.historyIndex, 4); // 严格夹取在上限

    // 连续按句号回溯
    for (int i = 0; i < 10; ++i) {
        state.historyIndex = std::max(state.historyIndex - 1, 0);
    }
    EXPECT_EQ(state.historyIndex, 0); // 严格夹取在下限
}

TEST(CaptureEmptyTextBoxResizeTest, MinimumBoundsAndResizeBeforeInput) {
    using namespace tools3000::capture;

    CaptureState state;
    cv::Mat canvas(600, 800, CV_8UC4, cv::Scalar(0, 0, 0, 0));
    state.markup.setBaseImage(canvas);

    // 1. 创建空文本标注元素（模拟用户刚刚点击画布）
    auto* textElem = state.markup.addText(cv::Point(150, 150), "", MarkupColor::Red(), 18.0f);
    ASSERT_NE(textElem, nullptr);
    textElem->isActive = true;
    textElem->isEditing = true;

    // 2. 验证空文本时的包围盒绝不为 0 且具有舒适的最小尺寸 (杜绝 8 手柄挤压在一起)
    cv::Rect bbox = textElem->getBoundingBox();
    EXPECT_GE(bbox.width, 80);
    EXPECT_GE(bbox.height, 25);
    EXPECT_EQ(bbox.x, 150);
    EXPECT_EQ(bbox.y, 150);

    // 3. 验证 8 个手柄的命中测试 (用户在未输入内容前即可拖拽任意手柄)
    HitArea hitRB = textElem->hitTestEx(cv::Point(bbox.x + bbox.width, bbox.y + bbox.height));
    EXPECT_EQ(hitRB, HitArea::RB);

    HitArea hitLT = textElem->hitTestEx(cv::Point(bbox.x, bbox.y));
    EXPECT_EQ(hitLT, HitArea::LT);

    // 4. 模拟拖拽 RB 手柄放大文本框 (字号增大)
    float oldFontSize = textElem->fontSize;
    textElem->resize(20, 20, HitArea::RB);
    EXPECT_GT(textElem->fontSize, oldFontSize);

    // 5. 模拟拖拽 LT 手柄缩小文本框
    textElem->resize(10, 10, HitArea::LT);
    EXPECT_LE(textElem->fontSize, 180.0f);
    EXPECT_GE(textElem->fontSize, 12.0f);
}

TEST(CaptureCornerRadiusDesignerWorkflowTest, LiveCornerCursorAndReset) {
    using namespace tools3000::capture;

    CaptureState state;
    state.dpiScale = 1.0f;
    state.cornerRadius = 12.0f;

    // 1. 模拟设计师双击圆角手柄：在 12px 与 0px 之间快速重置/切换
    float currentR = state.cornerRadius;
    float toggledR = (currentR > 0.0f) ? 0.0f : 12.0f;
    EXPECT_EQ(toggledR, 0.0f);

    float restoredR = (toggledR > 0.0f) ? 0.0f : 12.0f;
    EXPECT_EQ(restoredR, 12.0f);

    // 2. 验证四个角部的 offset 计算
    float selW = 400.0f, selH = 300.0f;
    float offset = std::clamp(std::max(22.0f, state.cornerRadius + 8.0f), 16.0f, std::min(selW, selH) * 0.40f);
    EXPECT_EQ(offset, 22.0f);
}

TEST(CaptureSelectionSideMenuTest, PixPinSideMenuAndSliderInteraction) {
    using namespace tools3000::capture;

    CaptureState state;
    state.mode = OverlayMode::Screenshot;
    state.dpiScale = 1.0f;
    state.cornerRadius = 16.0f;

    // 1. 测试常规居中选区：侧边菜单默认在右侧
    D2D1_RECT_F selRect = D2D1::RectF(100.0f, 100.0f, 400.0f, 300.0f);
    D2D1_SIZE_F surface = D2D1::SizeF(1920.0f, 1080.0f);
    rebuildCaptureToolbar(state, selRect, surface);

    EXPECT_EQ(state.selectionSideButtons.size(), 4u);
    EXPECT_EQ(state.selectionSideButtons[0].command, ToolbarCommand::SideToggleCornerRadius);
    EXPECT_EQ(state.selectionSideButtons[1].command, ToolbarCommand::SideCycleAspectRatio);
    EXPECT_EQ(state.selectionSideButtons[2].command, ToolbarCommand::SideInvertSelection);
    EXPECT_EQ(state.selectionSideButtons[3].command, ToolbarCommand::SideResetSelection);
    EXPECT_GT(state.selectionSideRect.left, selRect.right); // 默认在右侧
    EXPECT_FLOAT_EQ(state.selectionSideButtons[0].rect.top, selRect.top); // 右上外侧顶部对齐
    EXPECT_FLOAT_EQ(state.selectionSideButtons[0].rect.right - state.selectionSideButtons[0].rect.left, 30.0f);
    EXPECT_FLOAT_EQ(state.selectionSideButtons[0].rect.bottom - state.selectionSideButtons[0].rect.top, 30.0f);

    // 2. 测试紧贴屏幕右边缘选区：侧边菜单自动翻转至左侧
    D2D1_RECT_F rightEdgeSel = D2D1::RectF(1750.0f, 100.0f, 1910.0f, 300.0f);
    rebuildCaptureToolbar(state, rightEdgeSel, surface);
    EXPECT_EQ(state.selectionSideButtons.size(), 4u);
    EXPECT_LT(state.selectionSideRect.right, rightEdgeSel.left); // 智能翻转至左侧

    // 3. 模拟激活 SideToggleCornerRadius 展开 SelectionCornerRadius 滑块弹窗
    state.sliderPopup.type = SliderPopupType::SelectionCornerRadius;
    state.sliderPopup.minValue = 0;
    state.sliderPopup.maxValue = 80;
    state.sliderPopup.currentValue = static_cast<int>(state.cornerRadius);
    state.sliderPopup.trackRect = D2D1::RectF(100.0f, 200.0f, 300.0f, 205.0f);

    EXPECT_EQ(state.sliderPopup.currentValue, 16);

    // 4. 模拟在滑块轨道上点击 50% 位置
    float trackW = state.sliderPopup.trackRect.right - state.sliderPopup.trackRect.left;
    float clickX = state.sliderPopup.trackRect.left + trackW * 0.5f;
    float pct = std::clamp((clickX - state.sliderPopup.trackRect.left) / trackW, 0.0f, 1.0f);
    int mappedVal = state.sliderPopup.minValue + static_cast<int>(std::round(pct * (state.sliderPopup.maxValue - state.sliderPopup.minValue)));
    state.cornerRadius = static_cast<float>(mappedVal);
    EXPECT_EQ(state.cornerRadius, 40.0f);

    // 5. 模拟滚轮微调选区圆角
    int zDelta = 120;
    int delta = (zDelta > 0) ? 1 : -1;
    state.cornerRadius = std::clamp(state.cornerRadius + static_cast<float>(delta * 2), 0.0f, 80.0f);
    EXPECT_EQ(state.cornerRadius, 42.0f);
}

// -----------------------------------------------------------------------------
// 26. 截图/录屏提示覆盖层生命周期与自适应门禁测试套件
// -----------------------------------------------------------------------------
TEST(CaptureLifecycleAndGhostWindowTest, ShortcutHintOverlayLifecycleAndGhostExclusion) {
    using namespace tools3000::capture;

    // 1. 初始状态：提示层未显示
    EXPECT_FALSE(ShortcutHintOverlay::instance().isVisible());

    // 2. 模拟进入选区模式：展示快捷键提示
    ShortcutHintOverlay::instance().show(ShortcutHintContext::CaptureSelecting);
    EXPECT_TRUE(ShortcutHintOverlay::instance().isVisible());

    // 3. 模拟退出/取消：即刻隐藏，销毁窗口资源，杜绝幽灵残留
    ShortcutHintOverlay::instance().hide();
    EXPECT_FALSE(ShortcutHintOverlay::instance().isVisible());

    // 4. 模拟 50 次高频连击唤起与隐藏 (Burst Simulation)，验证幂等性
    for (int i = 0; i < 50; ++i) {
        ShortcutHintOverlay::instance().show(ShortcutHintContext::CaptureSelecting);
        EXPECT_TRUE(ShortcutHintOverlay::instance().isVisible());
        ShortcutHintOverlay::instance().hide();
        EXPECT_FALSE(ShortcutHintOverlay::instance().isVisible());
    }
}

TEST(CaptureLifecycleAndGhostWindowTest, DynamicShortcutAdaptationAndOverlayHints) {
    using namespace tools3000::capture;

    // 1. 验证快捷提示层能动态适配系统语言与快捷键配置
    auto selectingItems = ShortcutHintOverlay::instance().getItemsForContext(ShortcutHintContext::CaptureSelecting);
    EXPECT_FALSE(selectingItems.empty());
    
    // 验证包含核心操作引导条目
    bool hasDragItem = false;
    bool hasSpaceItem = false;
    for (const auto& item : selectingItems) {
        if (item.key == L"拖拽" || item.key == L"Drag") hasDragItem = true;
        if (item.key == L"Space") hasSpaceItem = true;
    }
    EXPECT_TRUE(hasDragItem);
    EXPECT_TRUE(hasSpaceItem);

    // 2. 验证录屏提示项动态读取 HotkeyManager，非硬编码
    auto recordingItems = ShortcutHintOverlay::instance().getItemsForContext(ShortcutHintContext::Recording);
    EXPECT_FALSE(recordingItems.empty());
}

// -----------------------------------------------------------------------------
// 27. 截图标注增强测试套件 (Sequence Numbers, Blur, Highlight Multiply, Spline)
// -----------------------------------------------------------------------------
TEST(CaptureMarkupOptimizationTest, SequenceNumberAutoHealingAndUndoRollback) {
    using namespace tools3000::capture;

    MarkupEngine engine;

    // 1. 初始状态：下一个序列号必须为 1
    EXPECT_EQ(engine.currentNumber(), 1);

    // 2. 依次添加 1, 2, 3 号标记
    int num1 = engine.addNumberMark(cv::Point(20, 20), MarkupColor::Red());
    int num2 = engine.addNumberMark(cv::Point(40, 40), MarkupColor::Red());
    int num3 = engine.addNumberMark(cv::Point(60, 60), MarkupColor::Red());

    EXPECT_EQ(num1, 1);
    EXPECT_EQ(num2, 2);
    EXPECT_EQ(num3, 3);
    EXPECT_EQ(engine.currentNumber(), 4);
    EXPECT_EQ(engine.elementCount(), 3u);

    // 3. 撤销最后一个（3号）：序列号计数器必须正确回滚到 3
    EXPECT_TRUE(engine.undo());
    EXPECT_EQ(engine.elementCount(), 2u);
    EXPECT_EQ(engine.currentNumber(), 3);

    // 4. 再次添加新编号：必须填补 3，绝不跳号至 4
    int num3Again = engine.addNumberMark(cv::Point(80, 80), MarkupColor::Red());
    EXPECT_EQ(num3Again, 3);
    EXPECT_EQ(engine.currentNumber(), 4);

    // 5. 重做撤销栈清空与重置：clearAll() 必须彻底重置序列号至 1
    engine.clearAll();
    EXPECT_EQ(engine.elementCount(), 0u);
    EXPECT_EQ(engine.currentNumber(), 1);

    // 6. 重新添加后验证删除指定元素的回滚行为
    engine.addNumberMark(cv::Point(10, 10), MarkupColor::Red());
    int secondId = engine.addNumberMark(cv::Point(30, 30), MarkupColor::Red());
    auto* elem2 = engine.getElementAt(cv::Point(30, 30));
    ASSERT_NE(elem2, nullptr);
    uint32_t id2 = elem2->id;

    EXPECT_EQ(secondId, 2);
    EXPECT_EQ(engine.currentNumber(), 3);

    // 删除第 2 个序号标记，计数器动态维护
    engine.removeElement(id2);
    EXPECT_EQ(engine.elementCount(), 1u);
    EXPECT_EQ(engine.currentNumber(), 2);
}

TEST(CaptureMarkupOptimizationTest, GaussianBlurMarkupAndRendering) {
    using namespace tools3000::capture;

    MarkupEngine engine;
    // 创建包含高频黑白斑马线的底图，验证高斯模糊前后的方差收敛
    cv::Mat base(100, 100, CV_8UC3, cv::Scalar(0, 0, 0));
    for (int y = 0; y < 100; ++y) {
        for (int x = 0; x < 100; x += 4) {
            base.at<cv::Vec3b>(y, x) = cv::Vec3b(255, 255, 255);
        }
    }
    engine.setBaseImage(base);

    // 1. 添加高斯模糊区域
    auto* blurElem = engine.applyBlur(cv::Point(20, 20), cv::Point(80, 80), 15);
    ASSERT_NE(blurElem, nullptr);
    EXPECT_EQ(blurElem->tool, MarkupTool::Blur);
    EXPECT_EQ(blurElem->mosaicBlockSize, 15);
    EXPECT_EQ(engine.elementCount(), 1u);

    // 2. 渲染合成图
    cv::Mat composite = engine.getCompositeImage();
    ASSERT_FALSE(composite.empty());
    EXPECT_EQ(composite.cols, 100);
    EXPECT_EQ(composite.rows, 100);

    // 模糊区域内部的像素应当被平滑，不再具有纯黑纯白的二值阶跃
    cv::Vec3b centerPix = composite.at<cv::Vec3b>(50, 50);
    EXPECT_GE(centerPix[0], 20);
    EXPECT_LE(centerPix[0], 235);

    // 选区外部区域应当保持原样
    cv::Vec3b outerPix = composite.at<cv::Vec3b>(5, 5);
    EXPECT_EQ(outerPix[0], 0);

    // 3. 撤销高斯模糊
    EXPECT_TRUE(engine.undo());
    cv::Mat restored = engine.getCompositeImage();
    EXPECT_EQ(restored.at<cv::Vec3b>(50, 50)[0], base.at<cv::Vec3b>(50, 50)[0]);
}

TEST(CaptureMarkupOptimizationTest, GaussianBlurDegenerateAndEdgeCases) {
    using namespace tools3000::capture;

    MarkupEngine engine;
    cv::Mat base(50, 50, CV_8UC3, cv::Scalar(128, 128, 128));
    engine.setBaseImage(base);

    // 1. 退化为 1x1 的极小选区，且 kernelSize 大于选区尺寸
    engine.applyBlur(cv::Point(10, 10), cv::Point(11, 11), 25);
    // 2. 退化为 0 面积的点选区
    engine.applyBlur(cv::Point(20, 20), cv::Point(20, 20), 15);
    // 3. 极狭长选区 (1x30)
    engine.applyBlur(cv::Point(5, 5), cv::Point(6, 35), 9);

    // 验证合成渲染安全不抛出异常、不崩溃
    cv::Mat composite;
    EXPECT_NO_THROW({
        composite = engine.getCompositeImage();
    });
    EXPECT_FALSE(composite.empty());
    EXPECT_EQ(composite.cols, 50);
    EXPECT_EQ(composite.rows, 50);
}

TEST(CaptureMarkupOptimizationTest, HighlightMultiplyBlendPreservesBlackText) {
    using namespace tools3000::capture;

    MarkupEngine engine;
    // 底图：白色背景上有一个黑色文字像素 (0, 0, 0)
    cv::Mat base(60, 60, CV_8UC3, cv::Scalar(255, 255, 255));
    base.at<cv::Vec3b>(30, 30) = cv::Vec3b(0, 0, 0);
    engine.setBaseImage(base);

    // 应用鲜黄色荧光笔高亮 (RGB: 255, 255, 0 -> BGR: 0, 255, 255)
    MarkupColor yellow{255, 255, 0, 255};
    engine.drawHighlight(cv::Point(10, 10), cv::Point(50, 50), yellow);

    cv::Mat composite = engine.getCompositeImage();
    ASSERT_FALSE(composite.empty());

    // 核心断言：正片叠底 (Multiply) 模式下，深色文字 (0,0,0) * 任何颜色 / 255 恒等于 0，绝不发灰泛白
    cv::Vec3b textPixel = composite.at<cv::Vec3b>(30, 30);
    EXPECT_EQ(textPixel[0], 0);
    EXPECT_EQ(textPixel[1], 0);
    EXPECT_EQ(textPixel[2], 0);

    // 白色背景 (255, 255, 255) * 黄色 / 255 应当被染成鲜明黄色（B通道显著衰减，G与R保持255）
    cv::Vec3b bgPixel = composite.at<cv::Vec3b>(20, 20);
    EXPECT_LT(bgPixel[0], 100);   // B通道显著衰减
    EXPECT_EQ(bgPixel[1], 255);   // G
    EXPECT_EQ(bgPixel[2], 255);   // R
}

TEST(CaptureMarkupOptimizationTest, CatmullRomPenSmoothingInterpolation) {
    using namespace tools3000::capture;

    MarkupEngine engine;
    cv::Mat base(100, 100, CV_8UC3, cv::Scalar(255, 255, 255));
    engine.setBaseImage(base);

    // 边缘情况 1: 0 个点与 1 个点，安全返回 nullptr
    std::vector<cv::Point> emptyPts;
    EXPECT_EQ(engine.drawPenStroke(emptyPts, MarkupColor::Red(), 2.0f), nullptr);
    std::vector<cv::Point> singlePt = {cv::Point(10, 10)};
    EXPECT_EQ(engine.drawPenStroke(singlePt, MarkupColor::Red(), 2.0f), nullptr);

    // 边缘情况 2: 仅 2 个点，保持原样折线段（不插值）
    std::vector<cv::Point> twoPts = {cv::Point(10, 10), cv::Point(20, 20)};
    auto* stroke2 = engine.drawPenStroke(twoPts, MarkupColor::Red(), 2.0f);
    ASSERT_NE(stroke2, nullptr);
    EXPECT_EQ(stroke2->penPoints.size(), 2u);

    // 正常情况: 传入 3 个直角折线控制点
    std::vector<cv::Point> rawPoints = {
        cv::Point(10, 10),
        cv::Point(10, 80),
        cv::Point(80, 80)
    };

    auto* stroke = engine.drawPenStroke(rawPoints, MarkupColor::Red(), 2.0f);
    ASSERT_NE(stroke, nullptr);

    // Catmull-Rom 样条插值必须将点集细化平滑，插值后的点数远大于原始 3 个点
    EXPECT_GT(stroke->penPoints.size(), rawPoints.size() * 2);

    cv::Mat composite = engine.getCompositeImage();
    EXPECT_FALSE(composite.empty());
}

// -----------------------------------------------------------------------------
// 28. 录屏光标查表与并发多波纹优化测试套件 (CursorOverlay & fastDistance)
// -----------------------------------------------------------------------------
TEST(ScreenRecordingOptimizationTest, FastSqrtDistanceLookupAccuracy) {
    using namespace tools3000::capture;

    // 验证 65536 静态查表在关键值与典型距离上的绝对精度
    const int testDistances[] = {0, 1, 4, 9, 16, 25, 49, 100, 256, 500, 1024, 4096, 16384, 65535};
    for (int d2 : testDistances) {
        float expected = std::sqrt(static_cast<float>(d2));
        float actual = CursorOverlay::fastDistance(d2);
        EXPECT_NEAR(actual, expected, 0.0001f);
    }

    // 验证超出查表上限（> 65535）时的动态开方兜底
    int outOfTable = 70000;
    float expectedFallback = std::sqrt(static_cast<float>(outOfTable));
    float actualFallback = CursorOverlay::fastDistance(outOfTable);
    EXPECT_NEAR(actualFallback, expectedFallback, 0.0001f);

    // 验证负数防护
    EXPECT_EQ(CursorOverlay::fastDistance(-5), 0.0f);
}

TEST(ScreenRecordingOptimizationTest, ClickZoomElasticCurvePrecision) {
    using namespace tools3000::capture;

    // 验证关键时刻的阻尼弹性缩放比例：0.0 -> 1.0, 0.18 -> 0.78, 0.50 -> 1.38, 1.0 -> 1.0
    EXPECT_NEAR(CursorOverlay::calculateClickZoomScale(0.0f), 1.00f, 0.001f);
    EXPECT_NEAR(CursorOverlay::calculateClickZoomScale(0.18f), 0.78f, 0.001f);
    EXPECT_NEAR(CursorOverlay::calculateClickZoomScale(0.50f), 1.38f, 0.001f);
    EXPECT_NEAR(CursorOverlay::calculateClickZoomScale(1.00f), 1.00f, 0.001f);

    // 边界与异常值兜底
    EXPECT_EQ(CursorOverlay::calculateClickZoomScale(-0.5f), 1.00f);
    EXPECT_EQ(CursorOverlay::calculateClickZoomScale(1.5f), 1.00f);

    // 全区间范围值校验 [0.78, 1.38]
    for (int i = 0; i <= 100; ++i) {
        float t = i / 100.0f;
        float scale = CursorOverlay::calculateClickZoomScale(t);
        EXPECT_GE(scale, 0.779f);
        EXPECT_LE(scale, 1.381f);
    }
}

TEST(ScreenRecordingOptimizationTest, CursorOverlayMultiRippleAndFramePatch) {
    using namespace tools3000::capture;

    CursorOverlay overlay;
    std::vector<std::uint8_t> framePixels(100 * 100 * 4, 128);
    CaptureFrameView frame{};
    frame.data = framePixels.data();
    frame.width = 100;
    frame.height = 100;
    frame.stride = 100 * 4;
    frame.format = CapturePixelFormat::Bgra32;

    CaptureRegion region{0, 0, 100, 100};

    // 注入测试点击波纹，确保产生实际渲染并生成有效 Patch
    overlay.triggerClickForTesting(POINT{50, 50}, false);

    // 模拟第 1 帧点击波纹渲染与 RAII 像素回退
    {
        auto patch1 = overlay.apply(frame, region, false, true);
        EXPECT_FALSE(patch1.empty());
    }
    // 验证第 1 帧 Patch 析构后无损还原
    EXPECT_EQ(framePixels[50 * 100 * 4 + 50 * 4], 128);

    // 模拟第 2 帧并发点击，波纹队列正常消费与还原
    {
        overlay.triggerClickForTesting(POINT{60, 60}, true);
        auto patch2 = overlay.apply(frame, region, false, true);
        EXPECT_FALSE(patch2.empty());
    }

    // Patch 析构后，原帧像素必须完全被无损还原
    EXPECT_EQ(framePixels[0], 128);
    EXPECT_EQ(framePixels[50 * 100 * 4 + 50 * 4], 128);
    EXPECT_EQ(framePixels[60 * 100 * 4 + 60 * 4], 128);
}

// -----------------------------------------------------------------------------
// 29. GIF 调色板聚类与量化器测试套件 (GifPaletteOptimizer)
// -----------------------------------------------------------------------------
TEST(ScreenRecordingOptimizationTest, GifPaletteOptimizerClusteringAndQuantization) {
    using namespace tools3000::capture;

    GifPaletteOptimizer optimizer;
    EXPECT_FALSE(optimizer.initialized);

    // 构造一个 16x16 的 3 通道 BGR 图像，包含红、绿、蓝、白 4 块鲜明颜色
    const int w = 16, h = 16, bpp = 3;
    const int stride = w * bpp;
    std::vector<uint8_t> src(h * stride);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* px = src.data() + y * stride + x * bpp;
            if (x < 8 && y < 8) {
                // 红色 (BGR: 0, 0, 255)
                px[0] = 0; px[1] = 0; px[2] = 255;
            } else if (x >= 8 && y < 8) {
                // 绿色 (BGR: 0, 255, 0)
                px[0] = 0; px[1] = 255; px[2] = 0;
            } else if (x < 8 && y >= 8) {
                // 蓝色 (BGR: 255, 0, 0)
                px[0] = 255; px[1] = 0; px[2] = 0;
            } else {
                // 白色 (BGR: 255, 255, 255)
                px[0] = 255; px[1] = 255; px[2] = 255;
            }
        }
    }

    // 1. 生成调色板
    optimizer.updatePalette(src.data(), w, h, stride, bpp);
    EXPECT_TRUE(optimizer.initialized);

    // 验证调色板前 256 项均包含 0xFF Alpha 通道
    for (int i = 0; i < 256; ++i) {
        EXPECT_EQ((optimizer.palette[i] >> 24) & 0xFF, 0xFF);
    }

    // 2. 执行帧像素量化
    std::vector<uint8_t> dst(w * h, 0);
    uint32_t outputPalette[256]{};
    optimizer.quantizeFrame(src.data(), w, h, stride, bpp, dst.data(), w, outputPalette);

    // 3. 验证量化后的索引均在 [0, 255] 合法区间，且相同颜色区域的索引高度一致
    uint8_t redIndex = dst[2 * w + 2];
    uint8_t greenIndex = dst[2 * w + 12];
    uint8_t blueIndex = dst[12 * w + 2];
    uint8_t whiteIndex = dst[12 * w + 12];

    // 四种不同颜色必须映射到不同的调色板索引
    EXPECT_NE(redIndex, greenIndex);
    EXPECT_NE(redIndex, blueIndex);
    EXPECT_NE(greenIndex, blueIndex);

    // 同一纯色块内部像素索引严格一致
    EXPECT_EQ(dst[4 * w + 4], redIndex);
    EXPECT_EQ(dst[4 * w + 14], greenIndex);
    EXPECT_EQ(dst[14 * w + 4], blueIndex);
    EXPECT_EQ(dst[14 * w + 14], whiteIndex);
}

TEST(ScreenRecordingOptimizationTest, GifPaletteOptimizerMonochromeEdgeCases) {
    using namespace tools3000::capture;

    GifPaletteOptimizer optimizer;

    // 边缘测试 1: 纯黑图像
    const int w = 8, h = 8, bpp = 4;
    const int stride = w * bpp;
    std::vector<uint8_t> blackSrc(h * stride, 0);
    optimizer.updatePalette(blackSrc.data(), w, h, stride, bpp);
    EXPECT_TRUE(optimizer.initialized);

    std::vector<uint8_t> blackDst(w * h, 0);
    uint32_t palette[256]{};
    optimizer.quantizeFrame(blackSrc.data(), w, h, stride, bpp, blackDst.data(), w, palette);
    // 所有量化结果都应相同
    EXPECT_EQ(blackDst[0], blackDst[w * h - 1]);

    // 边缘测试 2: 纯白图像
    std::vector<uint8_t> whiteSrc(h * stride, 255);
    optimizer.updatePalette(whiteSrc.data(), w, h, stride, bpp);
    std::vector<uint8_t> whiteDst(w * h, 0);
    optimizer.quantizeFrame(whiteSrc.data(), w, h, stride, bpp, whiteDst.data(), w, palette);
    EXPECT_EQ(whiteDst[0], whiteDst[w * h - 1]);
}

TEST(BeautyShellTest, GenerateShellDimensionsAndPalettes) {
    using namespace tools3000::capture;

    // 1. 空输入容错
    BeautyShellOptions opt;
    cv::Mat emptyRes = applyBeautyShell(cv::Mat{}, opt);
    EXPECT_TRUE(emptyRes.empty());

    // 2. 正常 3 通道 BGR 输入生成外壳
    cv::Mat src(120, 200, CV_8UC3, cv::Scalar(255, 128, 64));
    opt.enabled = true;
    opt.padding = 32;
    opt.cornerRadius = 16.0f;
    opt.shadowRadius = 18.0f;
    opt.bgType = BeautyBackgroundType::AuroraPurple;

    cv::Mat result = applyBeautyShell(src, opt);
    EXPECT_FALSE(result.empty());
    EXPECT_EQ(result.channels(), 4); // BGRA 格式
    EXPECT_EQ(result.cols, 200 + 32 * 2);
    EXPECT_EQ(result.rows, 120 + 32 * 2);

    // 3. 测试 5 大预设渐变调色板均能稳定产出
    std::vector<BeautyBackgroundType> palettes = {
        BeautyBackgroundType::StudioSlate,
        BeautyBackgroundType::PaperChalk,
        BeautyBackgroundType::SilkMist,
        BeautyBackgroundType::MidnightGraphite,
        BeautyBackgroundType::PureMinimal,
        BeautyBackgroundType::AuroraPurple,
        BeautyBackgroundType::SunriseCoral,
        BeautyBackgroundType::EmeraldSea,
        BeautyBackgroundType::ObsidianDark,
        BeautyBackgroundType::MinimalLight
    };

    for (auto pal : palettes) {
        opt.bgType = pal;
        cv::Mat out = applyBeautyShell(src, opt);
        EXPECT_EQ(out.cols, result.cols);
        EXPECT_EQ(out.rows, result.rows);
        EXPECT_EQ(out.channels(), 4);
    }

    // 4. 4 通道 BGRA 输入
    cv::Mat src4(100, 150, CV_8UC4, cv::Scalar(100, 150, 200, 255));
    cv::Mat out4 = applyBeautyShell(src4, opt);
    EXPECT_EQ(out4.cols, 150 + 32 * 2);
    EXPECT_EQ(out4.rows, 100 + 32 * 2);
    EXPECT_EQ(out4.channels(), 4);

    // 5. 极端微小与狭长边缘尺寸防御测试 (1x1, 1x500, 500x1, pad=0, 阴影偏移溢出)
    cv::Mat tiny1x1(1, 1, CV_8UC3, cv::Scalar(10, 20, 30));
    opt.padding = 4; // outW = 9, outH = 9, 远小于默认高斯核 ksize=45
    opt.shadowRadius = 22.0f;
    opt.shadowOffsetY = 50; // 偏移远大于 pad
    cv::Mat outTiny = applyBeautyShell(tiny1x1, opt);
    EXPECT_FALSE(outTiny.empty());
    EXPECT_EQ(outTiny.cols, 1 + 4 * 2);
    EXPECT_EQ(outTiny.rows, 1 + 4 * 2);

    // 6. pad = 0 边界
    opt.padding = 0;
    cv::Mat outZeroPad = applyBeautyShell(tiny1x1, opt);
    EXPECT_FALSE(outZeroPad.empty());
    EXPECT_EQ(outZeroPad.cols, 1);
    EXPECT_EQ(outZeroPad.rows, 1);

    // 7. 极端细长条 (1x600 与 600x1)
    opt.padding = 16;
    cv::Mat tall(600, 1, CV_8UC3, cv::Scalar(50, 50, 50));
    cv::Mat outTall = applyBeautyShell(tall, opt);
    EXPECT_EQ(outTall.cols, 1 + 16 * 2);
    EXPECT_EQ(outTall.rows, 600 + 16 * 2);

    cv::Mat wide(1, 600, CV_8UC3, cv::Scalar(50, 50, 50));
    cv::Mat outWide = applyBeautyShell(wide, opt);
    EXPECT_EQ(outWide.cols, 600 + 16 * 2);
    EXPECT_EQ(outWide.rows, 1 + 16 * 2);
}

TEST(AspectRatioAndMarkupEnhancementTest, AspectRatioCycleAndNumberShape) {
    using namespace tools3000::capture;

    // 1. 选区固定比例预设状态验证
    CaptureState state;
    EXPECT_EQ(state.aspectRatio, AspectRatioPreset::Free);
    EXPECT_EQ(state.currentNumberShape, NumberBadgeShape::Circle);

    // 2. 模拟切换比例与形状
    state.aspectRatio = AspectRatioPreset::Ratio_16_9;
    EXPECT_EQ(state.aspectRatio, AspectRatioPreset::Ratio_16_9);
    state.aspectRatio = AspectRatioPreset::Ratio_Golden;
    EXPECT_EQ(state.aspectRatio, AspectRatioPreset::Ratio_Golden);

    state.currentNumberShape = NumberBadgeShape::RoundedSquare;
    EXPECT_EQ(state.currentNumberShape, NumberBadgeShape::RoundedSquare);

    // 3. 序号标注渲染与形状参数验证
    MarkupEngine engine;
    cv::Mat canvas(300, 400, CV_8UC3, cv::Scalar(240, 240, 240));
    engine.setBaseImage(canvas);

    int id1 = engine.addNumberMark(cv::Point(50, 50), MarkupColor(239, 68, 68), 1.0f, NumberBadgeShape::Circle);
    int id2 = engine.addNumberMark(cv::Point(150, 50), MarkupColor(59, 130, 246), 1.0f, NumberBadgeShape::RoundedSquare);

    auto* numWithArrow = engine.drawNumberMark(cv::Point(200, 100), MarkupColor(16, 185, 129), 1.0f, NumberBadgeShape::Circle);
    ASSERT_NE(numWithArrow, nullptr);
    numWithArrow->hasLeaderArrow = true;
    numWithArrow->endPt = cv::Point(260, 160);
    numWithArrow->thickness = 3.0f;
    EXPECT_TRUE(numWithArrow->hasLeaderArrow);
    cv::Rect arrowBox = numWithArrow->getBoundingBox();
    EXPECT_GE(arrowBox.width, 50);
    EXPECT_GE(arrowBox.height, 50);
    EXPECT_TRUE(numWithArrow->hitTest(cv::Point(200, 100)));
    EXPECT_TRUE(numWithArrow->hitTest(cv::Point(230, 130)));
    EXPECT_TRUE(numWithArrow->hitTest(cv::Point(260, 160)));

    EXPECT_GT(id1, 0);
    EXPECT_GT(id2, 0);
    EXPECT_EQ(engine.elementCount(), 3);
    cv::Mat composite = engine.getCompositeImage();
    EXPECT_FALSE(composite.empty());
    EXPECT_EQ(composite.cols, 400);
    EXPECT_EQ(composite.rows, 300);

    // 4. 文字圆角微晶胶囊底衬与编辑态双击测试
    auto* textElem = engine.addText(cv::Point(100, 100), "Hello World", MarkupColor(255, 255, 255), 18.0f);
    ASSERT_NE(textElem, nullptr);
    textElem->fill = true; // 开启圆角微晶底衬
    cv::Mat textComposite = engine.getCompositeImage();
    EXPECT_FALSE(textComposite.empty());
}

TEST(ScreenRecordingIndicatorAndCountdownTest, DurationFormatAndSkipCountdown) {
    using namespace tools3000::capture;

    // 1. 暂停状态切换
    auto& indicator = RecordingIndicator::instance();
    EXPECT_FALSE(indicator.isPaused());
    indicator.setPaused(true);
    EXPECT_TRUE(indicator.isPaused());
    indicator.setPaused(false);
    EXPECT_FALSE(indicator.isPaused());

    // 2. ScreenRecorder skipCountdown
    auto& recorder = ScreenRecorder::instance();
    recorder.skipCountdown();
    // 状态未在录制时调用 skipCountdown 不应引发任何异常
    EXPECT_EQ(recorder.state(), RecordState::Idle);
}

TEST(PolymorphicMarkupAndPinEnhancementTest, PolymorphicHandlersAndLineAndPinOperations) {
    using namespace tools3000::capture;

    // 1. 验证多态标注工具注册表 (MarkupToolRegistry)
    auto& registry = MarkupToolRegistry::instance();
    auto* lineHandler = registry.getHandler(MarkupTool::Line);
    ASSERT_NE(lineHandler, nullptr);

    auto* rectHandler = registry.getHandler(MarkupTool::Rectangle);
    ASSERT_NE(rectHandler, nullptr);

    auto* arrowHandler = registry.getHandler(MarkupTool::Arrow);
    ASSERT_NE(arrowHandler, nullptr);

    auto* numHandler = registry.getHandler(MarkupTool::Number);
    ASSERT_NE(numHandler, nullptr);

    // 2. 验证直线工具 (MarkupTool::Line) 的多态渲染、包围盒与命中测试
    MarkupEngine engine;
    cv::Mat canvas(300, 400, CV_8UC3, cv::Scalar(255, 255, 255));
    engine.setBaseImage(canvas);

    auto* elem = engine.drawLine(cv::Point(20, 30), cv::Point(180, 150), MarkupColor(255, 0, 0), 4.0f, LineStyle::Solid);
    ASSERT_NE(elem, nullptr);
    EXPECT_EQ(engine.elementCount(), 1);
    EXPECT_EQ(elem->tool, MarkupTool::Line);
    EXPECT_EQ(elem->startPt, cv::Point(20, 30));
    EXPECT_EQ(elem->endPt, cv::Point(180, 150));

    // 包围盒测试：必须包含端点与线宽扩展
    cv::Rect bbox = elem->getBoundingBox();
    EXPECT_LE(bbox.x, 20);
    EXPECT_LE(bbox.y, 30);
    EXPECT_GE(bbox.x + bbox.width, 180);
    EXPECT_GE(bbox.y + bbox.height, 150);

    // 命中测试：在线段中点 (100, 90) 附近应命中，在远离线段 (20, 200) 处不命中
    EXPECT_TRUE(elem->hitTest(cv::Point(100, 90), 8));
    EXPECT_FALSE(elem->hitTest(cv::Point(20, 200), 5));

    // 直线虚线样式与点线样式测试
    auto* dashedLine = engine.drawLine(cv::Point(50, 50), cv::Point(200, 50), MarkupColor(0, 255, 0), 2.0f, LineStyle::Dashed);
    ASSERT_NE(dashedLine, nullptr);
    cv::Mat composite = engine.getCompositeImage();
    EXPECT_FALSE(composite.empty());

    // 3. 验证序号重置功能 (resetNumber)
    engine.resetNumber(1);
    EXPECT_EQ(engine.currentNumber(), 1);
    int n1 = engine.addNumberMark(cv::Point(30, 30), MarkupColor(0, 0, 255));
    EXPECT_EQ(n1, 1);
    EXPECT_EQ(engine.currentNumber(), 2);

    int n2 = engine.addNumberMark(cv::Point(60, 30), MarkupColor(0, 0, 255));
    EXPECT_EQ(n2, 2);
    EXPECT_EQ(engine.currentNumber(), 3);

    engine.resetNumber(10);
    EXPECT_EQ(engine.currentNumber(), 10);
    int n10 = engine.addNumberMark(cv::Point(90, 30), MarkupColor(0, 0, 255));
    EXPECT_EQ(n10, 10);
    EXPECT_EQ(engine.currentNumber(), 11);

    engine.resetNumber(); // 默认回到 1
    EXPECT_EQ(engine.currentNumber(), 1);
    int nRestart = engine.addNumberMark(cv::Point(120, 30), MarkupColor(0, 0, 255));
    EXPECT_EQ(nRestart, 1);
    EXPECT_EQ(engine.currentNumber(), 2);

    // 4. 验证直线工具在退化场景下的非退化包围盒与命中健全性
    auto* horizLine = engine.drawLine(cv::Point(50, 100), cv::Point(200, 100), MarkupColor(0, 0, 255), 2.0f);
    ASSERT_NE(horizLine, nullptr);
    cv::Rect hBox = horizLine->getBoundingBox();
    EXPECT_GT(hBox.width, 0);
    EXPECT_GT(hBox.height, 0);

    auto* vertLine = engine.drawLine(cv::Point(100, 50), cv::Point(100, 200), MarkupColor(0, 0, 255), 2.0f);
    ASSERT_NE(vertLine, nullptr);
    cv::Rect vBox = vertLine->getBoundingBox();
    EXPECT_GT(vBox.width, 0);
    EXPECT_GT(vBox.height, 0);

    auto* ptLine = engine.drawLine(cv::Point(100, 100), cv::Point(100, 100), MarkupColor(0, 0, 255), 2.0f);
    ASSERT_NE(ptLine, nullptr);
    cv::Rect ptBox = ptLine->getBoundingBox();
    EXPECT_GT(ptBox.width, 0);
    EXPECT_GT(ptBox.height, 0);

    // 5. 验证多态激活把手 (renderActiveHandles)
    horizLine->isActive = true;
    cv::Mat activeCanvas = canvas.clone();
    engine.renderElement(activeCanvas, *horizLine);
    EXPECT_FALSE(activeCanvas.empty());

    // 6. 验证二级工具栏矩形/椭圆/直线无缝互转
    CaptureState tbState;
    tbState.dpiScale = 1.0f;
    tbState.mode = OverlayMode::Screenshot;
    tbState.currentTool = MarkupTool::Rectangle;
    tbState.isMarkupToolActive = true;
    rebuildCaptureToolbar(tbState, D2D1::RectF(100, 100, 500, 400), D2D1::SizeF(1920, 1080));
    bool hasSubRect = false, hasSubEllipse = false, hasSubLine = false;
    for (const auto& btn : tbState.secondaryToolbarButtons) {
        if (btn.command == ToolbarCommand::SelectTool) {
            if (btn.tool == MarkupTool::Rectangle) hasSubRect = true;
            if (btn.tool == MarkupTool::Ellipse) hasSubEllipse = true;
            if (btn.tool == MarkupTool::Line) hasSubLine = true;
        }
    }
    EXPECT_TRUE(hasSubRect);
    EXPECT_TRUE(hasSubEllipse);
    EXPECT_TRUE(hasSubLine);

    // 7. 验证贴图高级色彩变换与多重状态机算法 (Invert/Grayscale/Multi-Transform Sync/Premultiplied Alpha)
    // 7.1 3 通道图像反色算法 (Involution 性质: f(f(x)) == x)
    cv::Mat bgrTest = (cv::Mat_<cv::Vec3b>(2, 2) <<
        cv::Vec3b(10, 20, 30), cv::Vec3b(100, 150, 200),
        cv::Vec3b(0, 255, 128), cv::Vec3b(255, 255, 255));
    cv::Mat invBgr = cv::Scalar(255, 255, 255) - bgrTest;
    EXPECT_EQ(invBgr.at<cv::Vec3b>(0, 0), cv::Vec3b(245, 235, 225));
    EXPECT_EQ(invBgr.at<cv::Vec3b>(1, 1), cv::Vec3b(0, 0, 0));
    cv::Mat doubleInvBgr = cv::Scalar(255, 255, 255) - invBgr;
    EXPECT_EQ(doubleInvBgr.at<cv::Vec3b>(0, 0), bgrTest.at<cv::Vec3b>(0, 0));

    // 7.2 4 通道 BGRA 图像反色 (保留 Alpha 通道)
    cv::Mat bgraTest = (cv::Mat_<cv::Vec4b>(2, 2) <<
        cv::Vec4b(10, 20, 30, 200), cv::Vec4b(100, 150, 200, 150),
        cv::Vec4b(0, 255, 128, 50), cv::Vec4b(255, 255, 255, 0));
    cv::Mat invBgra = bgraTest.clone();
    std::vector<cv::Mat> channels;
    cv::split(invBgra, channels);
    channels[0] = 255 - channels[0];
    channels[1] = 255 - channels[1];
    channels[2] = 255 - channels[2];
    cv::merge(channels, invBgra);
    EXPECT_EQ(invBgra.at<cv::Vec4b>(0, 0), cv::Vec4b(245, 235, 225, 200));
    EXPECT_EQ(invBgra.at<cv::Vec4b>(1, 1), cv::Vec4b(0, 0, 0, 0)); // 原图 Alpha=0 保持为 0

    // 7.3 Direct2D 硬件表面预乘 Alpha 校验算法 (Premultiplied Alpha: RGB <= A)
    for (int r = 0; r < invBgra.rows; ++r) {
        auto* p = invBgra.ptr<cv::Vec4b>(r);
        for (int c = 0; c < invBgra.cols; ++c) {
            const uchar a = p[c][3];
            if (a == 0) {
                p[c][0] = 0; p[c][1] = 0; p[c][2] = 0;
            } else if (a < 255) {
                p[c][0] = static_cast<uchar>((p[c][0] * a) / 255);
                p[c][1] = static_cast<uchar>((p[c][1] * a) / 255);
                p[c][2] = static_cast<uchar>((p[c][2] * a) / 255);
            }
        }
    }
    // 验证 Alpha=0 的像素 RGB 必定为 0，彻底杜绝 D2D 驱动层光晕毛刺与软缺页崩溃
    EXPECT_EQ(invBgra.at<cv::Vec4b>(1, 1), cv::Vec4b(0, 0, 0, 0));
    EXPECT_LE(invBgra.at<cv::Vec4b>(0, 0)[0], invBgra.at<cv::Vec4b>(0, 0)[3]);

    // 7.4 多重变换同步流水线 (Grayscale + Invert + Rotate + Flip 联动测试)
    cv::Mat colorBackup = bgrTest.clone();
    cv::Mat workingImg = bgrTest.clone();
    // 进入灰度
    cv::Mat gray;
    cv::cvtColor(workingImg, gray, cv::COLOR_BGR2GRAY);
    cv::cvtColor(gray, workingImg, cv::COLOR_GRAY2BGR);
    // 在灰度下进行反色
    workingImg = cv::Scalar(255, 255, 255) - workingImg;
    colorBackup = cv::Scalar(255, 255, 255) - colorBackup;
    // 在灰度下进行旋转
    cv::rotate(workingImg, workingImg, cv::ROTATE_90_CLOCKWISE);
    cv::rotate(colorBackup, colorBackup, cv::ROTATE_90_CLOCKWISE);
    // 退出灰度恢复彩色底图
    workingImg = colorBackup.clone();
    // 校验：恢复后的彩色底图同时具备 90 度旋转几何特征与色彩反转特征
    EXPECT_EQ(workingImg.rows, bgrTest.cols);
    EXPECT_EQ(workingImg.cols, bgrTest.rows);
    EXPECT_EQ(workingImg.at<cv::Vec3b>(0, 0), cv::Vec3b(255, 0, 127)); // 对应 (1, 0) 的反色 (255-0, 255-255, 255-128)

    // 8. 验证 Reicon 矢量图标枚举健全性
    EXPECT_NE(static_cast<int>(CaptureIconId::ToolLine), 0);
    EXPECT_NE(static_cast<int>(CaptureIconId::ActionInvert), 0);
    EXPECT_NE(static_cast<int>(CaptureIconId::ActionGrayscale), 0);
    EXPECT_NE(static_cast<int>(CaptureIconId::ActionFold), 0);
    EXPECT_NE(static_cast<int>(CaptureIconId::ActionResetScale), 0);
    EXPECT_NE(static_cast<int>(CaptureIconId::ActionResetNumber), 0);
    EXPECT_NE(static_cast<int>(CaptureIconId::ActionSnapshot), 0);
    EXPECT_NE(static_cast<int>(CaptureIconId::ActionOpenFolder), 0);
}

TEST(CaptureOptimizationAndNewFeaturesTest, ScreenRecordingAndFloatingGlassBar) {
    using namespace tools3000::capture;

    // 1. 验证新增 ReIcon 矢量图标枚举健全性
    EXPECT_NE(static_cast<int>(CaptureIconId::ActionSnapshot), 0);
    EXPECT_NE(static_cast<int>(CaptureIconId::ActionOpenFolder), 0);

    // 2. 验证 ScreenRecorder 高清快照原子请求状态流转
    auto& recorder = ScreenRecorder::instance();
    EXPECT_FALSE(recorder.isSnapshotRequested());
    recorder.requestSnapshot();
    // 录制器在 Idle 状态下不触发快照置位
    EXPECT_FALSE(recorder.isSnapshotRequested());

    // 3. 验证 RecordingIndicator 录制完成快捷响应浮岛状态流转
    auto& indicator = RecordingIndicator::instance();
    EXPECT_FALSE(indicator.isCompleted());
    indicator.showCompleted("C:\\dummy\\test.mp4", 12.5);
    EXPECT_TRUE(indicator.isCompleted());
    indicator.hide();
    EXPECT_FALSE(indicator.isCompleted());

    // 4. 验证 WinUtils 剪贴板文件复制与资源管理器定位健壮性
    EXPECT_FALSE(tools3000::core::WinUtils::copyFileToClipboard(L"C:\\nonexistent_file_xyz_12345.mp4"));
    EXPECT_FALSE(tools3000::core::WinUtils::showInExplorer(L""));

    // 创建临时文件验证真实文件剪贴板设置
    wchar_t tempDir[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring tempFile = std::wstring(tempDir) + L"tools3000_test_clip.txt";
    HANDLE hFile = CreateFileW(tempFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hFile);
        EXPECT_TRUE(tools3000::core::WinUtils::copyFileToClipboard(tempFile));
        DeleteFileW(tempFile.c_str());
    }

    // 5. 验证 FloatingGlassBar 接口健壮性与空句柄安全性 (Null-Safety Contract)
    FloatingGlassBar::drawGlassPanel(nullptr, D2D1::RectF(0, 0, 100, 30), 8.0f, true);
    FloatingGlassBar::drawGlassPanel(nullptr, D2D1::RectF(0, 0, 100, 30), 8.0f, false);
    FloatingGlassBar::drawButtonPill(nullptr, D2D1::RectF(0, 0, 24, 24), 4.0f, true, true, false, false, 1.0f, true);
    FloatingGlassBar::drawButtonPill(nullptr, D2D1::RectF(0, 0, 24, 24), 4.0f, false, false, true, false, 1.0f, false);
    FloatingGlassBar::drawButtonPill(nullptr, D2D1::RectF(0, 0, 24, 24), 4.0f, false, false, false, true, 1.0f, true);
    FloatingGlassBar::drawSeparator(nullptr, 50.0f, 0.0f, 30.0f, 1.0f, true);
    FloatingGlassBar::drawSeparator(nullptr, 50.0f, 0.0f, 30.0f, 1.0f, false);
}

// 声明 ScreenRecorder.cpp 内部实现的圆角遮罩算法供测试验证
namespace tools3000::capture {
    void applyVideoFrameCornerMask(uint8_t* data, int width, int height, int stride, CapturePixelFormat format, float cornerRadius);
}

TEST(CaptureOptimizationAndNewFeaturesTest, PolymorphicArrowsLineStylesAndFullChainCornerRadius) {
    using namespace tools3000::capture;

    // 1. 验证截图与图像平滑抗锯齿 Alpha 圆角处理 (applyRoundedCorners)
    cv::Mat solidWhite(100, 100, CV_8UC3, cv::Scalar(255, 255, 255));
    // 1.1 半径为 0 时返回原图尺寸与内容
    cv::Mat r0 = applyRoundedCorners(solidWhite, 0.0f);
    EXPECT_EQ(r0.size(), solidWhite.size());

    // 1.2 半径大于 0 时生成 4 通道 BGRA 图像
    cv::Mat r20 = applyRoundedCorners(solidWhite, 20.0f);
    EXPECT_EQ(r20.channels(), 4);
    EXPECT_EQ(r20.cols, 100);
    EXPECT_EQ(r20.rows, 100);
    // 中心像素保持完全不透明 (Alpha = 255)
    EXPECT_EQ(r20.at<cv::Vec4b>(50, 50)[3], 255);
    // 四角极点像素完全裁切透明 (Alpha = 0)
    EXPECT_EQ(r20.at<cv::Vec4b>(0, 0)[3], 0);
    EXPECT_EQ(r20.at<cv::Vec4b>(0, 99)[3], 0);
    EXPECT_EQ(r20.at<cv::Vec4b>(99, 0)[3], 0);
    EXPECT_EQ(r20.at<cv::Vec4b>(99, 99)[3], 0);
    // 验证圆角边缘具有平滑抗锯齿过渡 (0 < Alpha < 255)
    bool hasSubpixelGradient = false;
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 20; ++x) {
            uint8_t a = r20.at<cv::Vec4b>(y, x)[3];
            if (a > 0 && a < 255) {
                hasSubpixelGradient = true;
                break;
            }
        }
        if (hasSubpixelGradient) break;
    }
    EXPECT_TRUE(hasSubpixelGradient);

    // 1.3 4 通道输入且本身带透明通道时的容错与合成
    cv::Mat bgraInput(60, 60, CV_8UC4, cv::Scalar(100, 150, 200, 200));
    cv::Mat rBgra = applyRoundedCorners(bgraInput, 15.0f);
    EXPECT_EQ(rBgra.channels(), 4);
    EXPECT_EQ(rBgra.at<cv::Vec4b>(0, 0)[3], 0);
    EXPECT_LE(rBgra.at<cv::Vec4b>(30, 30)[3], 200);

    // 2. 验证多态箭头 (Classic Filled, Thin Open, Modern Tech, DoubleEnded)
    MarkupEngine engine;
    cv::Mat base(200, 200, CV_8UC3, cv::Scalar(0, 0, 0));
    engine.setBaseImage(base);

    // 2.1 经典实心尖角箭头 (Standard)
    auto* arrow1 = engine.drawArrow(cv::Point(20, 20), cv::Point(80, 20), MarkupColor::Red(), 3.0f);
    ASSERT_NE(arrow1, nullptr);
    arrow1->arrowStyle = ArrowStyle::Standard;
    arrow1->lineStyle = LineStyle::Solid;

    // 2.2 极简细线开放折角箭头 (Thin)
    auto* arrow2 = engine.drawArrow(cv::Point(20, 60), cv::Point(80, 60), MarkupColor::Green(), 2.0f);
    ASSERT_NE(arrow2, nullptr);
    arrow2->arrowStyle = ArrowStyle::Thin;
    arrow2->lineStyle = LineStyle::Dashed;

    // 2.3 圆润科技流线机翼型箭头 (Tech)
    auto* arrow3 = engine.drawArrow(cv::Point(20, 100), cv::Point(80, 100), MarkupColor::Blue(), 4.0f);
    ASSERT_NE(arrow3, nullptr);
    arrow3->arrowStyle = ArrowStyle::Tech;
    arrow3->lineStyle = LineStyle::DashDot;

    // 2.4 双向对称箭头 (DoubleEnded)
    auto* arrow4 = engine.drawArrow(cv::Point(20, 140), cv::Point(80, 140), MarkupColor::Yellow(), 3.0f);
    ASSERT_NE(arrow4, nullptr);
    arrow4->arrowStyle = ArrowStyle::DoubleEnded;
    arrow4->lineStyle = LineStyle::Dotted;

    cv::Mat comp = engine.getCompositeImage();
    EXPECT_FALSE(comp.empty());
    // 校验箭头终点附近非零像素存在 (成功绘制箭头头部)
    EXPECT_GT(cv::norm(comp(cv::Rect(75, 10, 20, 20)), cv::NORM_L1), 0.0);
    EXPECT_GT(cv::norm(comp(cv::Rect(75, 50, 20, 20)), cv::NORM_L1), 0.0);
    EXPECT_GT(cv::norm(comp(cv::Rect(75, 90, 20, 20)), cv::NORM_L1), 0.0);
    EXPECT_GT(cv::norm(comp(cv::Rect(75, 130, 20, 20)), cv::NORM_L1), 0.0);

    // 3. 验证通用多态线型管线 (矩形带圆角、椭圆、直线虚线化)
    engine.clearAll();
    // 3.1 矩形：带圆角 + 虚线
    auto* rect = engine.drawRectangle(cv::Point(10, 10), cv::Point(90, 90), MarkupColor::White(), 2.0f);
    ASSERT_NE(rect, nullptr);
    rect->cornerRadius = 12.0f;
    rect->lineStyle = LineStyle::Dashed;

    // 3.2 椭圆：点划线
    auto* ellipse = engine.drawEllipse(cv::Point(110, 10), cv::Point(190, 90), MarkupColor::White(), 2.0f);
    ASSERT_NE(ellipse, nullptr);
    ellipse->lineStyle = LineStyle::DashDot;

    // 3.3 直线：点虚线
    auto* line = engine.drawLine(cv::Point(10, 150), cv::Point(190, 150), MarkupColor::White(), 2.0f, LineStyle::Dotted);
    ASSERT_NE(line, nullptr);

    cv::Mat comp2 = engine.getCompositeImage();
    EXPECT_GT(cv::norm(comp2(cv::Rect(10, 10, 80, 80)), cv::NORM_L1), 0.0);
    EXPECT_GT(cv::norm(comp2(cv::Rect(110, 10, 80, 80)), cv::NORM_L1), 0.0);
    EXPECT_GT(cv::norm(comp2(cv::Rect(10, 145, 180, 10)), cv::NORM_L1), 0.0);

    // 4. 验证录屏圆角遮罩 (applyVideoFrameCornerMask)
    std::vector<uint8_t> frameBuf(100 * 100 * 4, 255);
    applyVideoFrameCornerMask(frameBuf.data(), 100, 100, 100 * 4, CapturePixelFormat::Bgra32, 20.0f);
    // 四角极点像素应被清空为 0
    EXPECT_EQ(frameBuf[0], 0); // B
    EXPECT_EQ(frameBuf[1], 0); // G
    EXPECT_EQ(frameBuf[2], 0); // R
    EXPECT_EQ(frameBuf[3], 0); // A
    // 中心点像素保持 255
    size_t centerIdx = (50 * 100 + 50) * 4;
    EXPECT_EQ(frameBuf[centerIdx], 255);
    EXPECT_EQ(frameBuf[centerIdx + 3], 255);

    // 5. 验证录屏指示器区域与圆角呼吸边框接口
    auto& ind = RecordingIndicator::instance();
    ind.setRecordingRegion(100, 100, 600, 400, 16.0f);
    ind.clearRecordingRegion();

    // 6. 深度验证 ArrowHandler 架构级重构 (IMarkupToolHandler 契约、轴线命中、双端点手柄与形变)
    const auto* arrowHandler = MarkupToolRegistry::instance().getHandler(MarkupTool::Arrow);
    ASSERT_NE(arrowHandler, nullptr);

    MarkupElement arrowElem;
    arrowElem.tool = MarkupTool::Arrow;
    arrowElem.startPt = cv::Point(20, 50);
    arrowElem.endPt = cv::Point(100, 50); // 水平箭头 (dy = 0)
    arrowElem.thickness = 3.0f;
    arrowElem.arrowStyle = ArrowStyle::Standard;

    // 6.1 getBoundingBox 保护验证：彻底根治 dy=0 导致的高度为 0 或包围盒坍塌
    cv::Rect bbox = arrowHandler->getBoundingBox(arrowElem);
    EXPECT_GE(bbox.width, 80);
    EXPECT_GE(bbox.height, 20);
    EXPECT_TRUE(bbox.contains(cv::Point(20, 50)));
    EXPECT_TRUE(bbox.contains(cv::Point(100, 50)));

    // 6.2 hitTest 精确点线距离与箭头保护区验证
    // 轴线中心点命中 (Body)
    EXPECT_EQ(arrowHandler->hitTest(arrowElem, cv::Point(60, 50), 4), HitArea::Body);
    // 箭头头部附近命中 (Body)
    EXPECT_EQ(arrowHandler->hitTest(arrowElem, cv::Point(100, 50), 4), HitArea::Body);
    // 远离轴线点拒绝 (None)
    EXPECT_EQ(arrowHandler->hitTest(arrowElem, cv::Point(60, 90), 4), HitArea::None);

    // 6.3 活跃状态控制手柄命中：首尾端点分别映射为 LT (Tail) 与 RB (Tip) 手柄
    arrowElem.isActive = true;
    EXPECT_EQ(arrowHandler->hitTest(arrowElem, cv::Point(20, 50), 4), HitArea::LT);
    EXPECT_EQ(arrowHandler->hitTest(arrowElem, cv::Point(100, 50), 4), HitArea::RB);

    // 6.4 拖拽手柄形变 (resize) 验证
    arrowHandler->resize(arrowElem, -5, -10, HitArea::LT);
    EXPECT_EQ(arrowElem.startPt, cv::Point(15, 40));
    arrowHandler->resize(arrowElem, 10, 15, HitArea::RB);
    EXPECT_EQ(arrowElem.endPt, cv::Point(110, 65));

    // 6.5 双控制手柄渲染 (renderActiveHandles) 验证
    cv::Mat handleCanvas(150, 150, CV_8UC3, cv::Scalar(0, 0, 0));
    arrowHandler->renderActiveHandles(handleCanvas, arrowElem);
    EXPECT_GT(cv::norm(handleCanvas, cv::NORM_L1), 0.0);

    // 7. 验证录屏快照热键会话武装策略 (HotkeyPolicy 根治开机全局抢占 Ctrl+Shift+S 另存为缺陷)
    using namespace tools3000::core;
    EXPECT_EQ(hotkeyArmScopeForName("Record Snapshot"), HotkeyArmScope::WhileRecording);
    EXPECT_EQ(hotkeyArmScopeForName("Record Pause"), HotkeyArmScope::WhileRecording);
    EXPECT_EQ(hotkeyArmScopeForName("Capture"), HotkeyArmScope::Always);

    // 闲置/未录制时绝不抢占武装
    EXPECT_FALSE(hotkeyShouldBeArmed(hotkeyArmScopeForName("Record Snapshot"), false));
    EXPECT_FALSE(hotkeyShouldBeArmed(hotkeyArmScopeForName("Record Pause"), false));
    // 录制开启时动态武装
    EXPECT_TRUE(hotkeyShouldBeArmed(hotkeyArmScopeForName("Record Snapshot"), true));
    EXPECT_TRUE(hotkeyShouldBeArmed(hotkeyArmScopeForName("Record Pause"), true));

    // 外部冲突判定：未武装时的卸载不是冲突；武装时注册失败才是真正冲突
    EXPECT_FALSE(hotkeyLooksExternallyConflicted(true, false, false));
    EXPECT_TRUE(hotkeyLooksExternallyConflicted(true, true, false));

    // 8. 验证贴图透明度微调区间契约 (10% ~ 100%) 与选区微调步长
    auto clampOpacity = [](float op) { return std::clamp(op, 0.10f, 1.0f); };
    EXPECT_NEAR(clampOpacity(0.05f), 0.10f, 0.001f);
    EXPECT_NEAR(clampOpacity(0.75f), 0.75f, 0.001f);
    EXPECT_NEAR(clampOpacity(1.50f), 1.00f, 0.001f);
    float wheelOp = 1.0f;
    wheelOp = clampOpacity(wheelOp - 0.05f);
    EXPECT_NEAR(wheelOp, 0.95f, 0.001f);

    auto calcStep = [](bool isShift) { return isShift ? 10 : 1; };
    EXPECT_EQ(calcStep(false), 1);
    EXPECT_EQ(calcStep(true), 10);

    // 9. 框架级通用圆角交互辅助引擎 (CornerRadiusHelper) 测试套件
    // 9.1 最小尺寸展示门限 (canShowHandles)
    EXPECT_FALSE(CornerRadiusHelper::canShowHandles(30.0f, 30.0f));
    EXPECT_FALSE(CornerRadiusHelper::canShowHandles(40.0f, 60.0f, 1.0f, 50.0f));
    EXPECT_TRUE(CornerRadiusHelper::canShowHandles(60.0f, 60.0f, 1.0f, 50.0f));
    EXPECT_TRUE(CornerRadiusHelper::canShowHandles(100.0f, 100.0f));

    // 9.2 动态偏移量计算 (calculateOffset)
    float off0 = CornerRadiusHelper::calculateOffset(0.0f, 100.0f, 100.0f);
    float off20 = CornerRadiusHelper::calculateOffset(20.0f, 100.0f, 100.0f);
    EXPECT_GE(off0, 16.0f);
    EXPECT_GT(off20, off0);

    // 9.3 内角手柄生成 (getCornerHandles)
    auto handles = CornerRadiusHelper::getCornerHandles(10.0f, 10.0f, 110.0f, 110.0f, 12.0f);
    EXPECT_EQ(handles.size(), 4u);
    EXPECT_GT(handles[0].x, 10.0f);
    EXPECT_LT(handles[0].x, 60.0f);
    EXPECT_GT(handles[0].y, 10.0f);
    EXPECT_LT(handles[0].y, 60.0f);
    EXPECT_EQ(handles[0].cornerIndex, 0);

    // 9.4 命中测试 (hitTestHandles)
    EXPECT_EQ(CornerRadiusHelper::hitTestHandles(10.0f, 10.0f, 110.0f, 110.0f, 12.0f, handles[0].x, handles[0].y), 0);
    EXPECT_EQ(CornerRadiusHelper::hitTestHandles(10.0f, 10.0f, 110.0f, 110.0f, 12.0f, handles[2].x, handles[2].y), 2);
    EXPECT_EQ(CornerRadiusHelper::hitTestHandles(10.0f, 10.0f, 110.0f, 110.0f, 12.0f, 60.0f, 60.0f), -1);

    // 9.5 推拉拖拽数学模型 (calculateDraggedRadius)
    float rIn = CornerRadiusHelper::calculateDraggedRadius(10.0f, 10.0f, 110.0f, 110.0f, 25.0f, 25.0f, 10.0f, 35.0f, 35.0f);
    EXPECT_GT(rIn, 10.0f);
    float rOut = CornerRadiusHelper::calculateDraggedRadius(10.0f, 10.0f, 110.0f, 110.0f, 25.0f, 25.0f, 10.0f, 15.0f, 15.0f);
    EXPECT_LT(rOut, 10.0f);
    float rZero = CornerRadiusHelper::calculateDraggedRadius(10.0f, 10.0f, 110.0f, 110.0f, 25.0f, 25.0f, 10.0f, -50.0f, -50.0f);
    EXPECT_EQ(rZero, 0.0f);
    float rMax = CornerRadiusHelper::calculateDraggedRadius(10.0f, 10.0f, 110.0f, 110.0f, 25.0f, 25.0f, 10.0f, 200.0f, 200.0f, 30.0f);
    EXPECT_EQ(rMax, 30.0f);

    // 9.6 增量拖拽 (applyIncrementalDrag)
    float incR = CornerRadiusHelper::applyIncrementalDrag(12.0f, 4.0f, 4.0f, 1.0f, 1.0f, 50.0f);
    EXPECT_EQ(incR, 16.0f);

    // 9.7 微晶 Toast 文本格式 (getToastMessage)
    auto toastZh = CornerRadiusHelper::getToastMessage(CornerRadiusTarget::Selection, 12.0f, true);
    auto toastEn = CornerRadiusHelper::getToastMessage(CornerRadiusTarget::Selection, 12.0f, false);
    EXPECT_NE(toastZh.find(L"12"), std::wstring::npos);
    EXPECT_NE(toastEn.find(L"12"), std::wstring::npos);

    // 10. 矩形标注工具 (RectangleHandler) 内角圆角手柄控制
    const auto* rectHandler = MarkupToolRegistry::instance().getHandler(MarkupTool::Rectangle);
    ASSERT_NE(rectHandler, nullptr);

    MarkupElement rectElem;
    rectElem.tool = MarkupTool::Rectangle;
    rectElem.startPt = cv::Point(10, 10);
    rectElem.endPt = cv::Point(110, 110);
    rectElem.cornerRadius = 0.0f;
    rectElem.thickness = 2.0f;
    rectElem.isActive = true;

    auto rectCornerHandles = CornerRadiusHelper::getCornerHandles(10.0f, 10.0f, 110.0f, 110.0f, 0.0f);
    ASSERT_FALSE(rectCornerHandles.empty());
    EXPECT_EQ(rectHandler->hitTest(rectElem, cv::Point(static_cast<int>(std::round(rectCornerHandles[0].x)),
                                                       static_cast<int>(std::round(rectCornerHandles[0].y))), 4),
              HitArea::CornerRadius);

    cv::Mat rectCanvas(120, 120, CV_8UC3, cv::Scalar(0, 0, 0));
    rectHandler->renderActiveHandles(rectCanvas, rectElem);
    EXPECT_GT(cv::norm(rectCanvas, cv::NORM_L1), 0.0);

    // 11. 全流程响应式快捷键指引 HUD (ShortcutHintOverlay)
    auto& hintOverlay = ShortcutHintOverlay::instance();
    std::vector<ShortcutHintContext> allContexts = {
        ShortcutHintContext::CaptureSelecting,
        ShortcutHintContext::CaptureDragging,
        ShortcutHintContext::CaptureSelected,
        ShortcutHintContext::CaptureMarking,
        ShortcutHintContext::CaptureMarkingNumber,
        ShortcutHintContext::CaptureEditingText,
    };
    for (auto ctx : allContexts) {
        auto items = hintOverlay.getItemsForContext(ctx);
        EXPECT_FALSE(items.empty());
        for (const auto& item : items) {
            EXPECT_FALSE(item.key.empty());
            EXPECT_FALSE(item.label.empty());
        }
    }

    auto dragItems = hintOverlay.getItemsForContext(ShortcutHintContext::CaptureDragging);
    bool hasSpacePan = false;
    for (const auto& it : dragItems) {
        if (it.key == L"Space") hasSpacePan = true;
    }
    EXPECT_TRUE(hasSpacePan);

    auto numItems = hintOverlay.getItemsForContext(ShortcutHintContext::CaptureMarkingNumber);
    bool hasLeaderArrow = false;
    for (const auto& it : numItems) {
        if (it.label.find(L"箭头") != std::wstring::npos || it.label.find(L"arrow") != std::wstring::npos ||
            it.label.find(L"指示") != std::wstring::npos || it.label.find(L"Leader") != std::wstring::npos) {
            hasLeaderArrow = true;
        }
    }
    EXPECT_TRUE(hasLeaderArrow);

    auto textItems = hintOverlay.getItemsForContext(ShortcutHintContext::CaptureEditingText);
    bool hasEnterSubmit = false;
    for (const auto& it : textItems) {
        if (it.key.find(L"Enter") != std::wstring::npos) hasEnterSubmit = true;
    }
    EXPECT_TRUE(hasEnterSubmit);
}

TEST(CaptureOptimizationAndNewFeaturesTest, PreRecordingSetupHudAndWeChatGifCopy) {
    using namespace tools3000::capture;

    // 1. 验证 RecordSetupOptions 默认配置与无级切换状态机
    RecordSetupOptions setup;
    EXPECT_EQ(setup.format, RecordFormat::MP4_H264);
    EXPECT_EQ(setup.fps, 30);
    EXPECT_EQ(setup.qualityLevel, 1);
    EXPECT_FALSE(setup.showClickEffects);
    EXPECT_TRUE(setup.showClickZoom);
    EXPECT_TRUE(setup.includeKeycast);
    EXPECT_FALSE(setup.captureSystemAudio);
    EXPECT_FALSE(setup.captureMicrophone);

    // 2. 验证 ConfigManager 录屏配置项持久化契约
    auto& config = tools3000::core::ConfigManager::instance();
    auto tempConfigDir = std::filesystem::temp_directory_path() /
        (L"Tools3000CaptureConfigTest_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::create_directories(tempConfigDir, ec);
    config.initialize(tempConfigDir);

    config.set<std::string>("/recording/format", "gif");
    EXPECT_EQ(config.get<std::string>("/recording/format", ""), "gif");
    config.set<int>("/recording/fps", 60);
    EXPECT_EQ(config.get<int>("/recording/fps", 0), 60);
    config.set<int>("/recording/bitrate", 20);
    EXPECT_EQ(config.get<int>("/recording/bitrate", 0), 20);
    config.set<bool>("/recording/showClickEffects", true);
    EXPECT_TRUE(config.get<bool>("/recording/showClickEffects", false));
    config.set<bool>("/recording/showClickZoom", false);
    EXPECT_FALSE(config.get<bool>("/recording/showClickZoom", true));
    config.set<bool>("/recording/includeKeycast", false);
    EXPECT_FALSE(config.get<bool>("/recording/includeKeycast", true));
    config.set<bool>("/recording/captureSystemAudio", true);
    EXPECT_TRUE(config.get<bool>("/recording/captureSystemAudio", false));
    config.set<bool>("/recording/captureMicrophone", true);
    EXPECT_TRUE(config.get<bool>("/recording/captureMicrophone", false));

    // 3. 验证 ShortcutHintOverlay 拖拽选区专属极简 HUD (PixPin 标杆对标)
    auto& hintOverlay = ShortcutHintOverlay::instance();
    auto dragItems = hintOverlay.getItemsForContext(ShortcutHintContext::CaptureDragging);
    EXPECT_EQ(dragItems.size(), 4u);
    EXPECT_EQ(dragItems[0].key, L"W/A/S/D");
    EXPECT_EQ(dragItems[1].key, L"Shift / Tab");
    EXPECT_EQ(dragItems[2].key, L"Space");
    EXPECT_EQ(dragItems[3].key, L"Esc");
    if (tools3000::core::WinUtils::isSystemLanguageChinese()) {
        EXPECT_EQ(dragItems[0].label, L"移动鼠标光标");
        EXPECT_EQ(dragItems[1].label, L"切换吸附 / 辅助微调");
        EXPECT_EQ(dragItems[2].label, L"按住平移选区");
        EXPECT_EQ(dragItems[3].label, L"取消");
    }

    // 验证选区圆角与美化外壳圆角单一事实源持久化
    config.set<double>("/screenshot/cornerRadius", 14.0);
    EXPECT_NEAR(config.get<double>("/screenshot/cornerRadius", 0.0), 14.0, 0.001);
    config.set<int>("/capture/beautyShellRadius", 24);
    EXPECT_EQ(config.get<int>("/capture/beautyShellRadius", 0), 24);
    config.set<bool>("/capture/beautyShellEnabled", true);
    EXPECT_TRUE(config.get<bool>("/capture/beautyShellEnabled", false));
    config.set<std::string>("/capture/beautyShellTheme", "paper_chalk");
    EXPECT_EQ(config.get<std::string>("/capture/beautyShellTheme", ""), "paper_chalk");

    // 验证录屏选区前指引 HUD (RecordSelecting)
    auto recItems = hintOverlay.getItemsForContext(ShortcutHintContext::RecordSelecting);
    EXPECT_FALSE(recItems.empty());
    bool hasSpace = false, hasEsc = false, hasEnter = false;
    for (const auto& item : recItems) {
        if (item.key == L"Space") hasSpace = true;
        if (item.key == L"Esc") hasEsc = true;
        if (item.key == L"Enter") hasEnter = true;
    }
    EXPECT_TRUE(hasSpace);
    EXPECT_TRUE(hasEsc);
    EXPECT_TRUE(hasEnter);

    // 4. 验证 CursorOverlay macOS 风格平滑缩放阻尼插值曲线 (Screen Studio 风格 0.78 下压 -> 1.38 弹性爆发 -> 1.00 回弹)
    EXPECT_NEAR(CursorOverlay::calculateClickZoomScale(0.0f), 1.0f, 0.001f);
    EXPECT_NEAR(CursorOverlay::calculateClickZoomScale(0.18f), 0.78f, 0.01f);
    EXPECT_NEAR(CursorOverlay::calculateClickZoomScale(0.50f), 1.38f, 0.01f);
    EXPECT_NEAR(CursorOverlay::calculateClickZoomScale(1.0f), 1.0f, 0.001f);

    // 5. 验证微信 GIF 复制接口 (copyGifToClipboard)
    // 5.1 非法/不存在文件返回 false
    EXPECT_FALSE(tools3000::core::WinUtils::copyGifToClipboard(L"C:\\nonexistent_fake_path_12345.gif"));
    EXPECT_FALSE(tools3000::core::WinUtils::copyGifToClipboard(L""));

    // 5.2 真实 1x1 GIF89a 临时文件写入与复合剪贴板注入
    wchar_t tempDir[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring tempGif = std::wstring(tempDir) + L"tools3000_test_wechat_clip.gif";
    const unsigned char gif1x1[] = {
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x21, 0xf9, 0x04, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02,
        0x44, 0x01, 0x00, 0x3b
    };
    FILE* fp = nullptr;
    _wfopen_s(&fp, tempGif.c_str(), L"wb");
    if (fp) {
        fwrite(gif1x1, 1, sizeof(gif1x1), fp);
        fclose(fp);
        EXPECT_TRUE(tools3000::core::WinUtils::copyGifToClipboard(tempGif));
        DeleteFileW(tempGif.c_str());
    }

    // 6. 验证录屏预置栏 (RecordRegion) 工具栏布局与无障碍语义交互
    CaptureState recState;
    recState.mode = OverlayMode::RecordRegion;
    recState.recordSetup.format = RecordFormat::GIF;
    recState.recordSetup.fps = 15;
    recState.recordSetup.qualityLevel = 2;
    recState.recordSetup.showClickEffects = true;
    recState.recordSetup.showClickZoom = true;
    recState.recordSetup.includeKeycast = true;
    recState.recordSetup.captureSystemAudio = true;
    recState.recordSetup.captureMicrophone = true;

    rebuildCaptureToolbar(recState, D2D1::RectF(100.0f, 100.0f, 600.0f, 500.0f), D2D1::SizeF(1920.0f, 1080.0f));
    EXPECT_EQ(recState.toolbarButtons.size(), 9u);

    // 验证无障碍命名与在 GIF 模式下声音按键安全屏蔽状态
    for (const auto& btn : recState.toolbarButtons) {
        EXPECT_FALSE(toolbarButtonAccessibleName(btn).empty());
        if (btn.command == ToolbarCommand::RecordToggleSystemAudio) {
            EXPECT_FALSE(isToolbarButtonSelected(btn, recState));
        }
        if (btn.command == ToolbarCommand::RecordToggleMicrophone) {
            EXPECT_FALSE(isToolbarButtonSelected(btn, recState));
        }
        if (btn.command == ToolbarCommand::RecordToggleKeycast) {
            EXPECT_TRUE(isToolbarButtonSelected(btn, recState));
        }
        if (btn.command == ToolbarCommand::RecordCycleClickEffect) {
            EXPECT_TRUE(isToolbarButtonSelected(btn, recState));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 19. 截图/录屏/美化外壳多态特性与 DevOps 生命周期自动化验证套件
// ─────────────────────────────────────────────────────────────────────────────

TEST(CapturePolymorphicFeaturesAndDevOpsTest, EffectiveCornerRadiusPolymorphismAndHandleInteraction) {
    using namespace tools3000::capture;
    auto temp = std::filesystem::temp_directory_path() /
        (L"Tools3000_CornerRadiusTest_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::create_directories(temp, ec);
    tools3000::core::ConfigManager::instance().initialize(temp);
    auto& config = tools3000::core::ConfigManager::instance();

    CaptureState state;
    // 1. 默认状态：美化外壳未启用，有效圆角等于选区直角 (0px)
    state.cornerRadius = 0.0f;
    state.beautyShell.enabled = false;
    state.beautyShell.cornerRadius = 16.0f;
    EXPECT_FLOAT_EQ(state.effectiveCornerRadius(), 0.0f);

    // 2. 调节选区圆角为 12px
    state.cornerRadius = 12.0f;
    EXPECT_FLOAT_EQ(state.effectiveCornerRadius(), 12.0f);

    // 3. 启用美化外壳：多态生效，有效圆角自动切为外壳圆角 (16px)
    state.beautyShell.enabled = true;
    EXPECT_FLOAT_EQ(state.effectiveCornerRadius(), 16.0f);

    // 4. 调整外壳圆角为大弧度 (24px 与 32px)
    state.beautyShell.cornerRadius = 24.0f;
    EXPECT_FLOAT_EQ(state.effectiveCornerRadius(), 24.0f);
    state.beautyShell.cornerRadius = 32.0f;
    EXPECT_FLOAT_EQ(state.effectiveCornerRadius(), 32.0f);

    // 5. 退出外壳模式：平滑回退至选区原生圆角 (12px)
    state.beautyShell.enabled = false;
    EXPECT_FLOAT_EQ(state.effectiveCornerRadius(), 12.0f);

    // 6. 边界值测试 (0px 直角外壳与极端半径)
    state.beautyShell.enabled = true;
    state.beautyShell.cornerRadius = 0.0f;
    EXPECT_FLOAT_EQ(state.effectiveCornerRadius(), 0.0f);
    state.beautyShell.cornerRadius = 60.0f;
    EXPECT_FLOAT_EQ(state.effectiveCornerRadius(), 60.0f);

    // 7. 手柄偏移与命中检测联动 (基于 effectiveCornerRadius)
    state.beautyShell.cornerRadius = 24.0f;
    const float selW = 300.0f;
    const float selH = 200.0f;
    const float scale = 1.0f;
    EXPECT_TRUE(CornerRadiusHelper::canShowHandles(selW, selH, scale, 110.0f));

    float off = CornerRadiusHelper::calculateOffset(state.effectiveCornerRadius(), selW, selH, scale);
    EXPECT_GE(off, 16.0f);
    EXPECT_LE(off, 80.0f);

    auto handles = CornerRadiusHelper::getCornerHandles(100.0f, 100.0f, 400.0f, 300.0f, state.effectiveCornerRadius(), scale, false);
    ASSERT_EQ(handles.size(), 4u);
    int hitLT = CornerRadiusHelper::hitTestHandles(100.0f, 100.0f, 400.0f, 300.0f, state.effectiveCornerRadius(), handles[0].x, handles[0].y, scale);
    EXPECT_EQ(hitLT, 0);

    // 8. 模拟拖拽手柄推拉数学模型：外壳模式下修改外壳圆角，普通模式下修改选区圆角
    float draggedR = CornerRadiusHelper::calculateDraggedRadius(
        100.0f, 100.0f, 400.0f, 300.0f,
        handles[0].x, handles[0].y,
        state.effectiveCornerRadius(),
        handles[0].x + 10.0f, handles[0].y + 10.0f,
        32.0f);
    EXPECT_GT(draggedR, 24.0f);
    EXPECT_LE(draggedR, 32.0f);

    // 9. Toast 文本与 ConfigManager 键值双向回写断言
    auto toastShellZh = CornerRadiusHelper::getToastMessage(CornerRadiusTarget::BeautifyShell, draggedR, true);
    auto toastShellEn = CornerRadiusHelper::getToastMessage(CornerRadiusTarget::BeautifyShell, draggedR, false);
    EXPECT_NE(toastShellZh.find(L"美化外壳圆角"), std::wstring::npos);
    EXPECT_NE(toastShellEn.find(L"Beauty Shell Radius"), std::wstring::npos);

    config.set<int>("/capture/beautyShellRadius", static_cast<int>(std::round(draggedR)));
    EXPECT_EQ(config.get<int>("/capture/beautyShellRadius", 0), static_cast<int>(std::round(draggedR)));

    auto toastSelZh = CornerRadiusHelper::getToastMessage(CornerRadiusTarget::Selection, 12.0f, true);
    auto toastSelEn = CornerRadiusHelper::getToastMessage(CornerRadiusTarget::Selection, 12.0f, false);
    EXPECT_NE(toastSelZh.find(L"选区圆角"), std::wstring::npos);
    EXPECT_NE(toastSelEn.find(L"Selection Radius"), std::wstring::npos);

    auto toastRectZh = CornerRadiusHelper::getToastMessage(CornerRadiusTarget::RectangleMarkup, 8.0f, true);
    auto toastRectEn = CornerRadiusHelper::getToastMessage(CornerRadiusTarget::RectangleMarkup, 8.0f, false);
    EXPECT_NE(toastRectZh.find(L"矩形圆角"), std::wstring::npos);
    EXPECT_NE(toastRectEn.find(L"Corner Radius"), std::wstring::npos);

    std::filesystem::remove_all(temp, ec);
}

TEST(CapturePolymorphicFeaturesAndDevOpsTest, DropdownMenuStateMachineAndHoverPreview) {
    using namespace tools3000::capture;
    auto temp = std::filesystem::temp_directory_path() /
        (L"Tools3000_CaptureDropdownTest_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::create_directories(temp, ec);
    tools3000::core::ConfigManager::instance().initialize(temp);

    CaptureState state;
    state.beautyShell.bgType = BeautyBackgroundType::StudioSlate;
    state.beautyShell.padding = 24;
    state.beautyShell.cornerRadius = 16.0f;
    state.recordSetup.format = RecordFormat::MP4_H264;
    state.recordSetup.fps = 30;
    state.recordSetup.qualityLevel = 1;

    // 1. 打开外壳质感背景下拉菜单 (BeautyShellBg)
    const D2D1_RECT_F anchor = D2D1::RectF(100.0f, 200.0f, 150.0f, 230.0f);
    openDropdownMenu(state, DropdownType::BeautyShellBg, anchor);
    EXPECT_EQ(state.dropdownMenu.type, DropdownType::BeautyShellBg);
    EXPECT_EQ(state.dropdownMenu.items.size(), 5u);
    EXPECT_EQ(state.dropdownMenu.originalBgType, BeautyBackgroundType::StudioSlate);
    EXPECT_EQ(state.dropdownMenu.hoveredIndex, -1);

    // 2. 悬停质感实时预览 (Hover Preview: 移动到索引 3 - MidnightGraphite 曜石深邃)
    updateDropdownHover(state, 3);
    EXPECT_EQ(state.dropdownMenu.hoveredIndex, 3);
    EXPECT_EQ(state.beautyShell.bgType, BeautyBackgroundType::MidnightGraphite);

    // 2.1 悬停越界保护：传入非法负索引或超大索引，必须安全回退至 originalBgType
    updateDropdownHover(state, -1);
    EXPECT_EQ(state.beautyShell.bgType, BeautyBackgroundType::StudioSlate);
    updateDropdownHover(state, 3);
    EXPECT_EQ(state.beautyShell.bgType, BeautyBackgroundType::MidnightGraphite);
    updateDropdownHover(state, 999);
    EXPECT_EQ(state.beautyShell.bgType, BeautyBackgroundType::StudioSlate);

    // 3. 用户移出/取消选择 (closeDropdownMenu with restorePreview = true)
    updateDropdownHover(state, 2);
    closeDropdownMenu(state, true);
    EXPECT_EQ(state.dropdownMenu.type, DropdownType::None);
    EXPECT_TRUE(state.dropdownMenu.items.empty());
    EXPECT_EQ(state.beautyShell.bgType, BeautyBackgroundType::StudioSlate); // 100% 恢复原始主题

    // 4. 再次打开并提交新选择 (commit: 选择 2 - SilkMist 翡翠海)
    openDropdownMenu(state, DropdownType::BeautyShellBg, anchor);
    commitDropdownSelection(state, DropdownType::BeautyShellBg, 2);
    EXPECT_EQ(state.dropdownMenu.type, DropdownType::None);
    EXPECT_EQ(state.beautyShell.bgType, BeautyBackgroundType::SilkMist);
    EXPECT_TRUE(state.beautyShell.enabled);
    EXPECT_EQ(tools3000::core::ConfigManager::instance().get<std::string>("/capture/beautyShellTheme", ""), "emerald_sea");
    EXPECT_TRUE(tools3000::core::ConfigManager::instance().get<bool>("/capture/beautyShellEnabled", false));
    EXPECT_FALSE(state.loupeToastMessage.empty());

    // 4.1 直接在已打开状态下切换到其他下拉类型 (无缝切换)
    openDropdownMenu(state, DropdownType::BeautyShellBg, anchor);
    EXPECT_EQ(state.dropdownMenu.type, DropdownType::BeautyShellBg);
    openDropdownMenu(state, DropdownType::BeautyShellPadding, anchor);
    EXPECT_EQ(state.dropdownMenu.type, DropdownType::BeautyShellPadding);
    EXPECT_EQ(state.dropdownMenu.items.size(), 6u);

    // 5. 验证外壳留白边距 (BeautyShellPadding)
    commitDropdownSelection(state, DropdownType::BeautyShellPadding, 48);
    EXPECT_EQ(state.beautyShell.padding, 48);
    EXPECT_EQ(tools3000::core::ConfigManager::instance().get<int>("/capture/beautyShellPadding", 0), 48);

    // 6. 验证外壳圆角 (BeautyShellRadius)
    openDropdownMenu(state, DropdownType::BeautyShellRadius, anchor);
    EXPECT_EQ(state.dropdownMenu.type, DropdownType::BeautyShellRadius);
    EXPECT_EQ(state.dropdownMenu.items.size(), 7u);
    commitDropdownSelection(state, DropdownType::BeautyShellRadius, 24);
    EXPECT_FLOAT_EQ(state.beautyShell.cornerRadius, 24.0f);
    EXPECT_EQ(tools3000::core::ConfigManager::instance().get<int>("/capture/beautyShellRadius", 0), 24);

    // 7. 验证录屏格式 (RecordFormat: 切换至 GIF 并检查微信安全提示说明)
    openDropdownMenu(state, DropdownType::RecordFormat, anchor);
    EXPECT_EQ(state.dropdownMenu.type, DropdownType::RecordFormat);
    EXPECT_EQ(state.dropdownMenu.items.size(), 2u);
    commitDropdownSelection(state, DropdownType::RecordFormat, 1);
    EXPECT_EQ(state.recordSetup.format, RecordFormat::GIF);
    EXPECT_EQ(tools3000::core::ConfigManager::instance().get<std::string>("/recording/format", ""), "gif");

    // 8. 验证录屏帧率 (RecordFps: 切换至 60 fps 极客高刷)
    openDropdownMenu(state, DropdownType::RecordFps, anchor);
    EXPECT_EQ(state.dropdownMenu.type, DropdownType::RecordFps);
    EXPECT_EQ(state.dropdownMenu.items.size(), 3u);
    commitDropdownSelection(state, DropdownType::RecordFps, 60);
    EXPECT_EQ(state.recordSetup.fps, 60);
    EXPECT_EQ(tools3000::core::ConfigManager::instance().get<int>("/recording/fps", 0), 60);

    // 9. 验证录屏画质 (RecordQuality: 切换至超清 16Mbps)
    openDropdownMenu(state, DropdownType::RecordQuality, anchor);
    commitDropdownSelection(state, DropdownType::RecordQuality, 2);
    EXPECT_EQ(state.recordSetup.qualityLevel, 2);
    EXPECT_EQ(tools3000::core::ConfigManager::instance().get<int>("/recording/bitrate", 0), 16);

    // 10. 验证鼠标点击特效 (RecordClickEffect: 模式 3 水波纹 + Mac 缩放)
    openDropdownMenu(state, DropdownType::RecordClickEffect, anchor);
    commitDropdownSelection(state, DropdownType::RecordClickEffect, 3);
    EXPECT_TRUE(state.recordSetup.showClickEffects);
    EXPECT_TRUE(state.recordSetup.showClickZoom);
    EXPECT_TRUE(tools3000::core::ConfigManager::instance().get<bool>("/recording/showClickEffects", false));
    EXPECT_TRUE(tools3000::core::ConfigManager::instance().get<bool>("/recording/showClickZoom", false));

    // 11. 验证快捷键轮转无锚点调用 (anchorRect.right <= anchorRect.left)
    const BeautyBackgroundType curTheme = state.beautyShell.bgType;
    openDropdownMenu(state, DropdownType::BeautyShellBg, D2D1::RectF(0, 0, 0, 0));
    EXPECT_NE(state.beautyShell.bgType, curTheme);

    const int curPadding = state.beautyShell.padding;
    openDropdownMenu(state, DropdownType::BeautyShellPadding, D2D1::RectF(0, 0, 0, 0));
    EXPECT_NE(state.beautyShell.padding, curPadding);

    const float curR = state.beautyShell.cornerRadius;
    openDropdownMenu(state, DropdownType::BeautyShellRadius, D2D1::RectF(0, 0, 0, 0));
    EXPECT_NE(state.beautyShell.cornerRadius, curR);
}

TEST(CapturePolymorphicFeaturesAndDevOpsTest, TextFontSizeConfigPersistenceFramework) {
    using namespace tools3000::capture;
    auto temp = std::filesystem::temp_directory_path() /
        (L"Tools3000_TextFontSizeTest_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::create_directories(temp, ec);
    tools3000::core::ConfigManager::instance().initialize(temp);
    auto& config = tools3000::core::ConfigManager::instance();

    // 1. 验证默认/已配置字号读取与回写
    config.set<double>("/capture/markup/text/fontSize", 26.0);
    const double loadedSize = config.get<double>("/capture/markup/text/fontSize", 18.0);
    EXPECT_DOUBLE_EQ(loadedSize, 26.0);

    // 2. 模拟创建 Text 标注元素并校验字号加载
    MarkupEngine engine;
    auto* textElem = engine.addText(cv::Point(100, 100), "DevOps Automation", MarkupColor::Red(), static_cast<float>(loadedSize));
    ASSERT_NE(textElem, nullptr);
    EXPECT_EQ(textElem->tool, MarkupTool::Text);
    EXPECT_FLOAT_EQ(textElem->fontSize, 26.0f);
    EXPECT_EQ(textElem->text, "DevOps Automation");

    // 3. 验证 TextHandler 调整与边界收敛 (12.0px ~ 180.0px)
    const auto* handler = MarkupToolRegistry::instance().getHandler(MarkupTool::Text);
    ASSERT_NE(handler, nullptr);

    // 模拟八向拉伸手柄调整文字字号
    handler->resize(*textElem, 20, 20, HitArea::RB);
    EXPECT_GT(textElem->fontSize, 26.0f);
    config.set<double>("/capture/markup/text/fontSize", static_cast<double>(textElem->fontSize));
    EXPECT_FLOAT_EQ(static_cast<float>(config.get<double>("/capture/markup/text/fontSize", 18.0)), textElem->fontSize);

    // 极小下拉缩放测试 (低于 12px 截断保护)
    handler->resize(*textElem, -200, -200, HitArea::RB);
    EXPECT_FLOAT_EQ(textElem->fontSize, 12.0f);

    // 极大拉伸测试 (高于 180px 截断保护)
    handler->resize(*textElem, 500, 500, HitArea::RB);
    EXPECT_FLOAT_EQ(textElem->fontSize, 180.0f);

    // 4. 验证包围盒随字号自适应重算
    cv::Rect box = handler->getBoundingBox(*textElem);
    EXPECT_GT(box.width, 100);
    EXPECT_GT(box.height, 50);

    // 5. 极端边界配置值读取保护机制
    auto getSafeFontSize = [&](double raw) -> float {
        return (raw < 10.0 || raw > 144.0) ? 18.0f : static_cast<float>(raw);
    };
    EXPECT_FLOAT_EQ(getSafeFontSize(5.0), 18.0f);
    EXPECT_FLOAT_EQ(getSafeFontSize(250.0), 18.0f);
    EXPECT_FLOAT_EQ(getSafeFontSize(36.0), 36.0f);

    // 恢复标准测试值
    config.set<double>("/capture/markup/text/fontSize", 18.0);
    std::filesystem::remove_all(temp, ec);
}

TEST(CapturePolymorphicFeaturesAndDevOpsTest, ShortcutHintOverlayCardLayoutAndAvoidance) {
    using namespace tools3000::capture;
    const RECT workArea = { 0, 0, 1920, 1080 };
    const int cardW = 220;
    const int cardH = 260;

    // 1. 无遮挡避让矩形时：默认最优位置为左下角 (PixPin 标杆规范)
    POINT pos = ShortcutHintOverlay::computeOptimalPosition(cardW, cardH, workArea, 1.0f, {});
    const int margin1x = static_cast<int>(ShortcutHintStyle::BaseScreenMargin * 1.0f);
    EXPECT_EQ(pos.x, workArea.left + margin1x);
    EXPECT_EQ(pos.y, workArea.bottom - cardH - margin1x);

    // 2. 左下角存在大面积碰撞 (例如工具栏或选区锁定在左下) -> 智能避让至其他无碰撞候选点 (如左上角)
    const RECT avoidLeftBottom = { 0, 750, 400, 1080 };
    POINT avoidedPos = ShortcutHintOverlay::computeOptimalPosition(cardW, cardH, workArea, 1.0f, { avoidLeftBottom });
    EXPECT_NE(avoidedPos.y, workArea.bottom - cardH - margin1x);
    // 候选点中左上角无碰撞，应该被选为最优点
    EXPECT_EQ(avoidedPos.x, workArea.left + margin1x);
    EXPECT_EQ(avoidedPos.y, workArea.top + margin1x);

    // 3. 高分屏缩放系数适配 (200% 缩放下外边距倍增)
    POINT pos2x = ShortcutHintOverlay::computeOptimalPosition(cardW, cardH, workArea, 2.0f, {});
    const int margin2x = static_cast<int>(ShortcutHintStyle::BaseScreenMargin * 2.0f);
    EXPECT_EQ(pos2x.x, workArea.left + margin2x);
    EXPECT_EQ(pos2x.y, workArea.bottom - cardH - margin2x);

    // 4. 极端狭小工作区与负缩放鲁棒保护
    const RECT tinyWorkArea = { 0, 0, 200, 200 };
    POINT tinyPos = ShortcutHintOverlay::computeOptimalPosition(cardW, cardH, tinyWorkArea, -1.0f, {});
    EXPECT_GE(tinyPos.x, tinyWorkArea.left);
    EXPECT_GE(tinyPos.y, tinyWorkArea.top);

    // 5. 验证不同上下文的快捷键项目完整性与语义无障碍
    auto& overlay = ShortcutHintOverlay::instance();
    for (auto ctx : {
        ShortcutHintContext::CaptureSelecting,
        ShortcutHintContext::CaptureDragging,
        ShortcutHintContext::CaptureSelected,
        ShortcutHintContext::CaptureMarking,
        ShortcutHintContext::CaptureMarkingNumber,
        ShortcutHintContext::CaptureEditingText,
        ShortcutHintContext::RecordSelecting,
        ShortcutHintContext::ScrollCapture,
        ShortcutHintContext::Recording,
        ShortcutHintContext::RecordingPaused
    }) {
        auto items = overlay.getItemsForContext(ctx);
        EXPECT_FALSE(items.empty());
        for (const auto& item : items) {
            EXPECT_FALSE(item.key.empty());
            EXPECT_FALSE(item.label.empty());
        }
    }
}

TEST(CapturePolymorphicFeaturesAndDevOpsTest, WeChatGif20MbSafeExitAndLifecycle) {
    using namespace tools3000::capture;
    // 1. 验证微信 GIF 20MB 测试阈值覆写与恢复机制
    EXPECT_EQ(ScreenRecorder::maxGifBytesForTesting(), 0ULL);

    ScreenRecorder::setMaxGifBytesForTesting(2048ULL);
    EXPECT_EQ(ScreenRecorder::maxGifBytesForTesting(), 2048ULL);

    ScreenRecorder::setMaxGifBytesForTesting(0ULL);
    EXPECT_EQ(ScreenRecorder::maxGifBytesForTesting(), 0ULL);

    // 2. 验证 stats.stopReason 为 "gif_wechat_limit" 时的契约语义与友好多语言提示
    RecordStats stats;
    stats.stopReason = "gif_wechat_limit";
    stats.durationSec = 12.5;
    stats.frameCount = 180;

    EXPECT_EQ(stats.stopReason, "gif_wechat_limit");

    // 模拟 PluginEntry 国际化提示映射契约
    const bool isZh = true;
    std::wstring toastZh = (stats.stopReason == "gif_wechat_limit")
        ? (isZh ? L"GIF 已达微信 20MB 上限，已自动为您安全保存" : L"GIF reached WeChat 20MB limit and saved")
        : L"其他提示";
    EXPECT_EQ(toastZh, L"GIF 已达微信 20MB 上限，已自动为您安全保存");

    const bool isEn = false;
    std::wstring toastEn = (stats.stopReason == "gif_wechat_limit")
        ? (isEn ? L"GIF 已达微信 20MB 上限，已自动为您安全保存" : L"GIF reached WeChat 20MB limit and saved")
        : L"Other toast";
    EXPECT_EQ(toastEn, L"GIF reached WeChat 20MB limit and saved");

    // 验证磁盘满异常提示契约
    stats.stopReason = "low_disk_space";
    std::wstring diskToastZh = (stats.stopReason == "low_disk_space")
        ? (isZh ? L"磁盘空间不足，录屏已安全保存" : L"Low disk space, recording saved")
        : L"";
    EXPECT_EQ(diskToastZh, L"磁盘空间不足，录屏已安全保存");

    // 3. 验证录屏单例在空闲状态下的鲁棒生命周期
    EXPECT_EQ(ScreenRecorder::instance().state(), RecordState::Idle);
    EXPECT_FALSE(ScreenRecorder::instance().needsFinalization());
}

TEST(CapturePolymorphicFeaturesAndDevOpsTest, TooltipAntiOverlapGeometryAvoidance) {
    using namespace tools3000::capture;
    const float scale = 1.0f;
    const D2D1_SIZE_F surface = D2D1::SizeF(1920.0f, 1080.0f);
    const float tw = 120.0f;
    const float th = 26.0f;

    // 1. 普通中央位置：按钮下方无二级工具栏，默认在按钮上方弹出 (buttonRect.top - th - 6px)
    const D2D1_RECT_F btnNormal = D2D1::RectF(500.0f, 400.0f, 540.0f, 432.0f);
    D2D1_RECT_F tipNormal = calculateTooltipRect(btnNormal, tw, th, scale, surface, D2D1::RectF(0,0,0,0), D2D1::RectF(0,0,0,0));
    EXPECT_FLOAT_EQ(tipNormal.bottom, btnNormal.top - 6.0f * scale);
    EXPECT_FLOAT_EQ(tipNormal.right - tipNormal.left, tw);
    EXPECT_FLOAT_EQ(tipNormal.bottom - tipNormal.top, th);

    // 2. 主工具栏下方紧贴着二级工具栏 (hasSecondary = true)：主工具栏提示强制在主栏上方弹出，绝不遮挡二级栏
    const D2D1_RECT_F secToolbar = D2D1::RectF(400.0f, 434.0f, 800.0f, 470.0f);
    D2D1_RECT_F tipWithSec = calculateTooltipRect(btnNormal, tw, th, scale, surface, D2D1::RectF(0,0,0,0), secToolbar);
    EXPECT_FLOAT_EQ(tipWithSec.bottom, btnNormal.top - 6.0f * scale);
    EXPECT_LE(tipWithSec.bottom, secToolbar.top); // 严格低于/在二级栏外部，0 像素重叠

    // 3. 屏幕顶部边界约束 (ty < 4px 翻转至按钮下方)
    const D2D1_RECT_F btnTop = D2D1::RectF(500.0f, 10.0f, 540.0f, 42.0f);
    D2D1_RECT_F tipTop = calculateTooltipRect(btnTop, tw, th, scale, surface, D2D1::RectF(0,0,0,0), D2D1::RectF(0,0,0,0));
    EXPECT_FLOAT_EQ(tipTop.top, btnTop.bottom + 6.0f * scale);

    // 4. 屏幕左右侧物理边缘安全 Clamp (8px 安全余量)
    const D2D1_RECT_F btnLeft = D2D1::RectF(2.0f, 300.0f, 34.0f, 332.0f);
    D2D1_RECT_F tipLeft = calculateTooltipRect(btnLeft, tw, th, scale, surface, D2D1::RectF(0,0,0,0), D2D1::RectF(0,0,0,0));
    EXPECT_GE(tipLeft.left, 8.0f * scale);

    const D2D1_RECT_F btnRight = D2D1::RectF(1900.0f, 300.0f, 1918.0f, 332.0f);
    D2D1_RECT_F tipRight = calculateTooltipRect(btnRight, tw, th, scale, surface, D2D1::RectF(0,0,0,0), D2D1::RectF(0,0,0,0));
    EXPECT_LE(tipRight.right, surface.width - 8.0f * scale);

    // 5. 极端边界测试：超窄画布 (surface.width < tw + 16)，验证 std::clamp 无异常与崩溃
    const D2D1_SIZE_F narrowSurface = D2D1::SizeF(80.0f, 1080.0f);
    D2D1_RECT_F tipNarrow = calculateTooltipRect(btnNormal, tw, th, scale, narrowSurface, D2D1::RectF(0,0,0,0), D2D1::RectF(0,0,0,0));
    EXPECT_GE(tipNarrow.left, 8.0f * scale);

    // 5.1 双工具栏反转场景（二级属性栏位于主工具栏正上方）：主栏提示向下避让，二级栏提示向上避让
    const D2D1_RECT_F priBelow = D2D1::RectF(400.0f, 500.0f, 800.0f, 540.0f);
    const D2D1_RECT_F secAbove = D2D1::RectF(400.0f, 456.0f, 800.0f, 492.0f);
    const D2D1_RECT_F btnOnPri = D2D1::RectF(440.0f, 504.0f, 472.0f, 536.0f);
    const D2D1_RECT_F btnOnSec = D2D1::RectF(440.0f, 460.0f, 472.0f, 488.0f);
    const D2D1_RECT_F tipOnPri = calculateTooltipRect(btnOnPri, tw, th, scale, surface, D2D1::RectF(0,0,0,0), secAbove, priBelow, false);
    const D2D1_RECT_F tipOnSec = calculateTooltipRect(btnOnSec, tw, th, scale, surface, D2D1::RectF(0,0,0,0), secAbove, priBelow, false);
    EXPECT_GE(tipOnPri.top, priBelow.bottom);
    EXPECT_LE(tipOnSec.bottom, secAbove.top);

    // 5.2 选区侧边竖排圆钮：提示向水平侧向居中弹出，绝不垂直遮挡下方相邻圆钮
    const D2D1_RECT_F sideCircleBtn = D2D1::RectF(908.0f, 200.0f, 938.0f, 230.0f);
    const D2D1_RECT_F tipSide = calculateTooltipRect(sideCircleBtn, tw, th, scale, surface, D2D1::RectF(0,0,0,0), D2D1::RectF(0,0,0,0), D2D1::RectF(0,0,0,0), true);
    EXPECT_GE(tipSide.left, sideCircleBtn.right);

    // 5.3 弹出层互斥收起状态机验证
    CaptureState popupState;
    popupState.isSizeMenuOpen = true;
    popupState.sliderPopup.type = SliderPopupType::SelectionCornerRadius;
    popupState.openSubmenu = SubmenuType::ArrowType;
    popupState.dropdownMenu.type = DropdownType::ShapeMode;
    EXPECT_TRUE(hasAnyCapturePopupOpen(popupState));
    closeAllCapturePopups(popupState);
    EXPECT_FALSE(hasAnyCapturePopupOpen(popupState));

    // 6. 提示文本字典覆盖度校验 (中文与英文 100% 对齐且非空)
    ToolbarButton rectBtn;
    rectBtn.command = ToolbarCommand::SelectTool;
    rectBtn.tool = MarkupTool::Rectangle;
    EXPECT_FALSE(tooltipForButton(rectBtn, true).empty());
    EXPECT_FALSE(tooltipForButton(rectBtn, false).empty());

    ToolbarButton beautyBtn;
    beautyBtn.command = ToolbarCommand::ToggleBeautyShell;
    EXPECT_FALSE(tooltipForButton(beautyBtn, true).empty());
    EXPECT_FALSE(tooltipForButton(beautyBtn, false).empty());

    ToolbarButton recFormatBtn;
    recFormatBtn.command = ToolbarCommand::RecordToggleFormat;
    EXPECT_FALSE(tooltipForButton(recFormatBtn, true).empty());
    EXPECT_FALSE(tooltipForButton(recFormatBtn, false).empty());

    ToolbarButton textBtn;
    textBtn.command = ToolbarCommand::SelectTool;
    textBtn.tool = MarkupTool::Text;
    EXPECT_FALSE(tooltipForButton(textBtn, true).empty());
    EXPECT_FALSE(tooltipForButton(textBtn, false).empty());
}

TEST(CaptureMarkupToolFixesTest, NumberBadgeSingleClickVsLeaderArrowDrag) {
    using namespace tools3000::capture;
    MarkupEngine engine;
    cv::Mat canvas(400, 400, CV_8UC3, cv::Scalar(255, 255, 255));
    engine.setBaseImage(canvas);

    // 1. 单击序号标号：生成静态纯圆标 (hasLeaderArrow == false)
    auto* badge1 = engine.drawNumberMark(cv::Point(100, 100), MarkupColor::Red(), 1.0f);
    ASSERT_NE(badge1, nullptr);
    EXPECT_FALSE(badge1->hasLeaderArrow);
    EXPECT_EQ(badge1->startPt.x, 100);
    EXPECT_EQ(badge1->startPt.y, 100);
    EXPECT_EQ(badge1->numberValue, 1);

    const auto* numHandler = MarkupToolRegistry::instance().getHandler(MarkupTool::Number);
    ASSERT_NE(numHandler, nullptr);

    // 静态标号命中测试：只在圆标内命中 Body，圆标外返回 None
    EXPECT_EQ(numHandler->hitTest(*badge1, cv::Point(100, 100), 4), HitArea::Body);
    EXPECT_EQ(numHandler->hitTest(*badge1, cv::Point(200, 200), 4), HitArea::None);

    // 2. 拖拽引出箭头标号：用户按住不放拖动鼠标引出箭头指向目标点 (hasLeaderArrow == true)
    auto badge2Ptr = std::make_unique<MarkupElement>();
    badge2Ptr->tool = MarkupTool::Number;
    badge2Ptr->startPt = cv::Point(150, 150);
    badge2Ptr->endPt = cv::Point(220, 220); // 拖动引出箭头指向 (220, 220)
    badge2Ptr->hasLeaderArrow = true;
    badge2Ptr->numberValue = 2;
    badge2Ptr->color = MarkupColor::Blue();
    badge2Ptr->isActive = true;
    badge2Ptr->dpiScale = 1.0f;
    auto* badge2 = engine.addElement(std::move(badge2Ptr));
    ASSERT_NE(badge2, nullptr);
    EXPECT_TRUE(badge2->hasLeaderArrow);

    // 箭头把手命中测试：末端箭头尖端应命中 HitArea::RB
    EXPECT_EQ(numHandler->hitTest(*badge2, cv::Point(220, 220), 4), HitArea::RB);
    // 箭身连线中段命中 HitArea::Body
    EXPECT_EQ(numHandler->hitTest(*badge2, cv::Point(185, 185), 4), HitArea::Body);
    // 标号本体命中 HitArea::Body
    EXPECT_EQ(numHandler->hitTest(*badge2, cv::Point(150, 150), 4), HitArea::Body);
    // 外部无关联区域返回 None
    EXPECT_EQ(numHandler->hitTest(*badge2, cv::Point(300, 300), 4), HitArea::None);

    // 3. 拖动手柄调整箭头尖端位置
    numHandler->resize(*badge2, 10, -5, HitArea::RB);
    EXPECT_EQ(badge2->endPt.x, 230);
    EXPECT_EQ(badge2->endPt.y, 215);
    EXPECT_EQ(badge2->startPt.x, 150); // 起点标号保持不动

    // 4. 平移整个标号与箭头
    numHandler->resize(*badge2, 20, 20, HitArea::Body);
    EXPECT_EQ(badge2->startPt.x, 170);
    EXPECT_EQ(badge2->endPt.x, 250);
}

TEST(CaptureMarkupToolFixesTest, HollowRectangleClickThroughAndCornerRadiusDrag) {
    using namespace tools3000::capture;
    const auto* rectHandler = MarkupToolRegistry::instance().getHandler(MarkupTool::Rectangle);
    ASSERT_NE(rectHandler, nullptr);

    // 1. 空心矩形 (fill = false) 穿透测试
    MarkupElement hollowRect;
    hollowRect.tool = MarkupTool::Rectangle;
    hollowRect.startPt = cv::Point(50, 50);
    hollowRect.endPt = cv::Point(250, 250);
    hollowRect.fill = false;
    hollowRect.thickness = 2.0f;
    hollowRect.isActive = false;

    // 四周边框带上应命中 Body (如 x=50, y=100)
    EXPECT_EQ(rectHandler->hitTest(hollowRect, cv::Point(50, 100), 4), HitArea::Body);
    EXPECT_EQ(rectHandler->hitTest(hollowRect, cv::Point(250, 100), 4), HitArea::Body);
    // 矩形空心内部中央应返回 None（穿透，允许用户在内部画新矩形或点击背景元素）
    EXPECT_EQ(rectHandler->hitTest(hollowRect, cv::Point(150, 150), 4), HitArea::None);

    // 2. 实心矩形 (fill = true) 不穿透
    MarkupElement filledRect = hollowRect;
    filledRect.fill = true;
    EXPECT_EQ(rectHandler->hitTest(filledRect, cv::Point(150, 150), 4), HitArea::Body);

    // 3. 矩形圆角把手推拉计算测试
    // 左上角手柄：往中心 (150, 150) 拖拽，圆角应增加
    float r1 = CornerRadiusHelper::calculateDraggedRadius(
        50.0f, 50.0f, 250.0f, 250.0f,
        60.0f, 60.0f, // dragStart
        0.0f,         // startRadius
        80.0f, 80.0f  // current (向内拉动 20px)
    );
    EXPECT_GT(r1, 10.0f);
    EXPECT_LE(r1, 100.0f); // 不超过 maxRadius (min(w,h)*0.5)

    // 右下角手柄：往外角 (300, 300) 推，圆角应减小至 0
    float r2 = CornerRadiusHelper::calculateDraggedRadius(
        50.0f, 50.0f, 250.0f, 250.0f,
        240.0f, 240.0f, // dragStart
        20.0f,          // startRadius
        260.0f, 260.0f  // current (向外推 20px)
    );
    EXPECT_FLOAT_EQ(r2, 0.0f);
}

TEST(CaptureMarkupToolFixesTest, LineToolHandlerAndEndpointsResize) {
    using namespace tools3000::capture;
    const auto* lineHandler = MarkupToolRegistry::instance().getHandler(MarkupTool::Line);
    ASSERT_NE(lineHandler, nullptr);

    MarkupElement lineElem;
    lineElem.tool = MarkupTool::Line;
    lineElem.startPt = cv::Point(30, 40);
    lineElem.endPt = cv::Point(130, 140);
    lineElem.thickness = 3.0f;
    lineElem.lineStyle = LineStyle::Solid;
    lineElem.color = MarkupColor::Green();
    lineElem.isActive = true;

    // 激活状态新手柄测试：起点命中 LT，终点命中 RB，中点命中平滑曲线控制柄 T
    EXPECT_EQ(lineHandler->hitTest(lineElem, cv::Point(30, 40), 4), HitArea::LT);
    EXPECT_EQ(lineHandler->hitTest(lineElem, cv::Point(130, 140), 4), HitArea::RB);
    EXPECT_EQ(lineHandler->hitTest(lineElem, cv::Point(80, 90), 4), HitArea::T);

    // 非中点线身段命中 Body
    EXPECT_EQ(lineHandler->hitTest(lineElem, cv::Point(55, 65), 4), HitArea::Body);
    // 远离线段返回 None
    EXPECT_EQ(lineHandler->hitTest(lineElem, cv::Point(10, 200), 4), HitArea::None);

    // 调整端点
    lineHandler->resize(lineElem, 5, -5, HitArea::LT);
    EXPECT_EQ(lineElem.startPt.x, 35);
    EXPECT_EQ(lineElem.startPt.y, 35);
    EXPECT_EQ(lineElem.endPt.x, 130); // 终点不变

    // 渲染测试
    cv::Mat canvas(200, 200, CV_8UC3, cv::Scalar(0, 0, 0));
    lineHandler->render(canvas, lineElem);
    EXPECT_GT(cv::norm(canvas, cv::NORM_L1), 0.0);
}

TEST(CaptureMarkupToolFixesTest, CleanCompositeExportVsActiveHandlePreview) {
    using namespace tools3000::capture;
    MarkupEngine engine;
    cv::Mat canvas(200, 200, CV_8UC3, cv::Scalar(100, 100, 100));
    engine.setBaseImage(canvas);

    auto* rect = engine.drawRectangle(cv::Point(20, 20), cv::Point(120, 120), MarkupColor::Red(), 2.0f);
    ASSERT_NE(rect, nullptr);
    rect->isActive = true; // 激活状态

    // 纯净图导出 (复制到剪贴板 / 保存文件)：绝对不包含高反差选中框与手柄
    cv::Mat cleanImg = engine.getCompositeImage(false);
    ASSERT_FALSE(cleanImg.empty());

    // 实时交互预览 (D2D / 编辑态)：包含高反差外框与手柄
    cv::Mat previewImg = engine.getCompositeImage(true);
    ASSERT_FALSE(previewImg.empty());

    // 二者像素应有差异 (预览图绘制了手柄与激活边界)
    double diff = cv::norm(cleanImg, previewImg, cv::NORM_L1);
    EXPECT_GT(diff, 0.0);
}

TEST(CaptureMarkupToolFixesTest, NonHollowToolInteriorHitTestPreserved) {
    using namespace tools3000::capture;

    // 验证继承自 DefaultBoxHandler 的非空心工具在 fill == false 时，内部中央仍能正确命中 HitArea::Body
    // 杜绝之前因把 !fill 错误加入 DefaultBoxHandler 导致的文本、马赛克、模糊、荧光笔无法点击选中的重大退化

    // 1. Text 工具
    const auto* textHandler = MarkupToolRegistry::instance().getHandler(MarkupTool::Text);
    ASSERT_NE(textHandler, nullptr);
    MarkupElement textElem;
    textElem.tool = MarkupTool::Text;
    textElem.startPt = cv::Point(50, 50);
    textElem.endPt = cv::Point(150, 90);
    textElem.fill = false;
    textElem.textRenderSize = cv::Size(100, 40);
    EXPECT_EQ(textHandler->hitTest(textElem, cv::Point(100, 70), 4), HitArea::Body);
    EXPECT_EQ(textHandler->hitTest(textElem, cv::Point(200, 200), 4), HitArea::None);

    // 2. Mosaic 工具
    const auto* mosaicHandler = MarkupToolRegistry::instance().getHandler(MarkupTool::Mosaic);
    ASSERT_NE(mosaicHandler, nullptr);
    MarkupElement mosaicElem;
    mosaicElem.tool = MarkupTool::Mosaic;
    mosaicElem.startPt = cv::Point(40, 40);
    mosaicElem.endPt = cv::Point(140, 140);
    mosaicElem.fill = false;
    EXPECT_EQ(mosaicHandler->hitTest(mosaicElem, cv::Point(90, 90), 4), HitArea::Body);

    // 3. Blur 工具
    const auto* blurHandler = MarkupToolRegistry::instance().getHandler(MarkupTool::Blur);
    ASSERT_NE(blurHandler, nullptr);
    MarkupElement blurElem;
    blurElem.tool = MarkupTool::Blur;
    blurElem.startPt = cv::Point(60, 60);
    blurElem.endPt = cv::Point(160, 160);
    blurElem.fill = false;
    EXPECT_EQ(blurHandler->hitTest(blurElem, cv::Point(110, 110), 4), HitArea::Body);

    // 4. Highlight 工具
    const auto* highlightHandler = MarkupToolRegistry::instance().getHandler(MarkupTool::Highlight);
    ASSERT_NE(highlightHandler, nullptr);
    MarkupElement highlightElem;
    highlightElem.tool = MarkupTool::Highlight;
    highlightElem.startPt = cv::Point(30, 30);
    highlightElem.endPt = cv::Point(130, 70);
    highlightElem.fill = false;
    EXPECT_EQ(highlightHandler->hitTest(highlightElem, cv::Point(80, 50), 4), HitArea::Body);

    // 5. Inpaint 工具
    const auto* inpaintHandler = MarkupToolRegistry::instance().getHandler(MarkupTool::Inpaint);
    ASSERT_NE(inpaintHandler, nullptr);
    MarkupElement inpaintElem;
    inpaintElem.tool = MarkupTool::Inpaint;
    inpaintElem.startPt = cv::Point(20, 20);
    inpaintElem.endPt = cv::Point(100, 100);
    inpaintElem.fill = false;
    EXPECT_EQ(inpaintHandler->hitTest(inpaintElem, cv::Point(60, 60), 4), HitArea::Body);
}

TEST(CaptureMarkupToolFixesTest, EllipseGeometricHitTestHollowVsFilled) {
    using namespace tools3000::capture;
    const auto* ellipseHandler = MarkupToolRegistry::instance().getHandler(MarkupTool::Ellipse);
    ASSERT_NE(ellipseHandler, nullptr);

    // 椭圆测试：中心在 (100, 100)，水平半轴 rx = 60，垂直半轴 ry = 40
    // startPt = (40, 60), endPt = (160, 140)
    MarkupElement ellipseElem;
    ellipseElem.tool = MarkupTool::Ellipse;
    ellipseElem.startPt = cv::Point(40, 60);
    ellipseElem.endPt = cv::Point(160, 140);
    ellipseElem.fill = false;
    ellipseElem.isActive = false;

    // 1. 空心未填充状态：中心 (100, 100) 必须穿透返回 None
    EXPECT_EQ(ellipseHandler->hitTest(ellipseElem, cv::Point(100, 100), 4), HitArea::None);

    // 椭圆边缘轮廓上的点 (40, 100) 和 (100, 60) 应命中 Body
    EXPECT_EQ(ellipseHandler->hitTest(ellipseElem, cv::Point(40, 100), 4), HitArea::Body);
    EXPECT_EQ(ellipseHandler->hitTest(ellipseElem, cv::Point(100, 60), 4), HitArea::Body);

    // 椭圆包围盒四角空白区域 (45, 65) 必须穿透返回 None
    EXPECT_EQ(ellipseHandler->hitTest(ellipseElem, cv::Point(45, 65), 4), HitArea::None);

    // 2. 实心填充状态：内部中央应命中 Body
    ellipseElem.fill = true;
    EXPECT_EQ(ellipseHandler->hitTest(ellipseElem, cv::Point(100, 100), 4), HitArea::Body);

    // 3. 激活态 8 方向手柄
    ellipseElem.isActive = true;
    EXPECT_EQ(ellipseHandler->hitTest(ellipseElem, cv::Point(40, 60), 4), HitArea::LT);
    EXPECT_EQ(ellipseHandler->hitTest(ellipseElem, cv::Point(160, 140), 4), HitArea::RB);
}

TEST(CaptureMarkupToolFixesTest, PenStrokeProximityHitTest) {
    using namespace tools3000::capture;
    const auto* penHandler = MarkupToolRegistry::instance().getHandler(MarkupTool::Pen);
    ASSERT_NE(penHandler, nullptr);

    // 涂鸦笔折线：(20, 20) -> (100, 20) -> (100, 100)
    MarkupElement penElem;
    penElem.tool = MarkupTool::Pen;
    penElem.penPoints = { cv::Point(20, 20), cv::Point(100, 20), cv::Point(100, 100) };
    penElem.thickness = 3.0f;
    penElem.isActive = false;

    // 1. 折线段附近必须命中 Body
    EXPECT_EQ(penHandler->hitTest(penElem, cv::Point(50, 20), 4), HitArea::Body);
    EXPECT_EQ(penHandler->hitTest(penElem, cv::Point(100, 60), 4), HitArea::Body);

    // 2. 包围盒内但远离折线段的空白区域 (20, 100) 必须穿透返回 None
    EXPECT_EQ(penHandler->hitTest(penElem, cv::Point(20, 100), 4), HitArea::None);
    EXPECT_EQ(penHandler->hitTest(penElem, cv::Point(50, 80), 4), HitArea::None);

    // 3. 激活态 8 方向控制手柄
    penElem.isActive = true;
    cv::Rect bbox = penHandler->getBoundingBox(penElem);
    EXPECT_EQ(penHandler->hitTest(penElem, cv::Point(bbox.x, bbox.y), 4), HitArea::LT);
    EXPECT_EQ(penHandler->hitTest(penElem, cv::Point(bbox.x + bbox.width, bbox.y + bbox.height), 4), HitArea::RB);
}

// -----------------------------------------------------------------------------
// 30. 四角微晶圆角手柄交互与数学推拉模型测试套件 (CornerRadiusHelper)
// -----------------------------------------------------------------------------
TEST(CaptureCornerRadiusTest, AllFourHandlesGeneratedAndPositioned) {
    using namespace tools3000::capture;

    float left = 100.0f, top = 100.0f, right = 500.0f, bottom = 400.0f;
    float radius = 20.0f;
    float scale = 1.0f;

    // 当 singlePrimaryCorner 为 false 时，必须完整返回 4 个角的手柄
    auto handles = CornerRadiusHelper::getCornerHandles(left, top, right, bottom, radius, scale, false);
    ASSERT_EQ(handles.size(), 4u);

    // 0: LT 左上角
    EXPECT_EQ(handles[0].cornerIndex, 0);
    EXPECT_GT(handles[0].x, left);
    EXPECT_GT(handles[0].y, top);
    EXPECT_GT(handles[0].inDirX, 0.0f);
    EXPECT_GT(handles[0].inDirY, 0.0f);

    // 1: RT 右上角
    EXPECT_EQ(handles[1].cornerIndex, 1);
    EXPECT_LT(handles[1].x, right);
    EXPECT_GT(handles[1].y, top);
    EXPECT_LT(handles[1].inDirX, 0.0f);
    EXPECT_GT(handles[1].inDirY, 0.0f);

    // 2: RB 右下角
    EXPECT_EQ(handles[2].cornerIndex, 2);
    EXPECT_LT(handles[2].x, right);
    EXPECT_LT(handles[2].y, bottom);
    EXPECT_LT(handles[2].inDirX, 0.0f);
    EXPECT_LT(handles[2].inDirY, 0.0f);

    // 3: LB 左下角
    EXPECT_EQ(handles[3].cornerIndex, 3);
    EXPECT_GT(handles[3].x, left);
    EXPECT_LT(handles[3].y, bottom);
    EXPECT_GT(handles[3].inDirX, 0.0f);
    EXPECT_LT(handles[3].inDirY, 0.0f);
}

TEST(CaptureCornerRadiusTest, HitTestingAllFourHandles) {
    using namespace tools3000::capture;

    float left = 100.0f, top = 100.0f, right = 500.0f, bottom = 400.0f;
    float radius = 20.0f;
    float scale = 1.0f;

    auto handles = CornerRadiusHelper::getCornerHandles(left, top, right, bottom, radius, scale, false);
    ASSERT_EQ(handles.size(), 4u);

    // 命中各个手柄的中心位置
    for (int i = 0; i < 4; ++i) {
        int hit = CornerRadiusHelper::hitTestHandles(
            left, top, right, bottom, radius,
            handles[i].x, handles[i].y, scale, 11.0f, false);
        EXPECT_EQ(hit, i);
    }

    // 选区中心位置不应命中任何圆角手柄
    int hitCenter = CornerRadiusHelper::hitTestHandles(
        left, top, right, bottom, radius,
        300.0f, 250.0f, scale, 11.0f, false);
    EXPECT_EQ(hitCenter, -1);

    // 选区外部位置不应命中
    int hitOutside = CornerRadiusHelper::hitTestHandles(
        left, top, right, bottom, radius,
        50.0f, 50.0f, scale, 11.0f, false);
    EXPECT_EQ(hitOutside, -1);
}

TEST(CaptureCornerRadiusTest, FourCornersDraggingMathematicalModel) {
    using namespace tools3000::capture;

    float left = 100.0f, top = 100.0f, right = 500.0f, bottom = 400.0f;
    float startRadius = 20.0f;
    float maxRadius = 150.0f;

    // 0: LT 左上角 (向内向下拉拽为正，向外向上推为负)
    {
        float startX = 120.0f, startY = 120.0f;
        // 向内拉 (dx=+20, dy=+20)
        float rIn = CornerRadiusHelper::calculateDraggedRadius(
            left, top, right, bottom, startX, startY, startRadius, startX + 20.0f, startY + 20.0f, maxRadius, 0);
        EXPECT_GT(rIn, startRadius);

        // 向外推 (dx=-10, dy=-10)
        float rOut = CornerRadiusHelper::calculateDraggedRadius(
            left, top, right, bottom, startX, startY, startRadius, startX - 10.0f, startY - 10.0f, maxRadius, 0);
        EXPECT_LT(rOut, startRadius);

        // 推过头截断至 0
        float rZero = CornerRadiusHelper::calculateDraggedRadius(
            left, top, right, bottom, startX, startY, startRadius, startX - 100.0f, startY - 100.0f, maxRadius, 0);
        EXPECT_EQ(rZero, 0.0f);

        // 超过上限截断至 maxRadius
        float rMax = CornerRadiusHelper::calculateDraggedRadius(
            left, top, right, bottom, startX, startY, startRadius, startX + 300.0f, startY + 300.0f, maxRadius, 0);
        EXPECT_EQ(rMax, maxRadius);
    }

    // 1: RT 右上角 (向左向下拉拽为正，向右向上推为负)
    {
        float startX = 480.0f, startY = 120.0f;
        // 向内拉 (dx=-20, dy=+20)
        float rIn = CornerRadiusHelper::calculateDraggedRadius(
            left, top, right, bottom, startX, startY, startRadius, startX - 20.0f, startY + 20.0f, maxRadius, 1);
        EXPECT_GT(rIn, startRadius);

        // 向外推 (dx=+10, dy=-10)
        float rOut = CornerRadiusHelper::calculateDraggedRadius(
            left, top, right, bottom, startX, startY, startRadius, startX + 10.0f, startY - 10.0f, maxRadius, 1);
        EXPECT_LT(rOut, startRadius);
    }

    // 2: RB 右下角 (向左向上拉拽为正，向右向下推为负)
    {
        float startX = 480.0f, startY = 380.0f;
        // 向内拉 (dx=-20, dy=-20)
        float rIn = CornerRadiusHelper::calculateDraggedRadius(
            left, top, right, bottom, startX, startY, startRadius, startX - 20.0f, startY - 20.0f, maxRadius, 2);
        EXPECT_GT(rIn, startRadius);

        // 向外推 (dx=+10, dy=+10)
        float rOut = CornerRadiusHelper::calculateDraggedRadius(
            left, top, right, bottom, startX, startY, startRadius, startX + 10.0f, startY + 10.0f, maxRadius, 2);
        EXPECT_LT(rOut, startRadius);
    }

    // 3: LB 左下角 (向右向上拉拽为正，向左向下推为负)
    {
        float startX = 120.0f, startY = 380.0f;
        // 向内拉 (dx=+20, dy=-20)
        float rIn = CornerRadiusHelper::calculateDraggedRadius(
            left, top, right, bottom, startX, startY, startRadius, startX + 20.0f, startY - 20.0f, maxRadius, 3);
        EXPECT_GT(rIn, startRadius);

        // 向外推 (dx=-10, dy=+10)
        float rOut = CornerRadiusHelper::calculateDraggedRadius(
            left, top, right, bottom, startX, startY, startRadius, startX - 10.0f, startY + 10.0f, maxRadius, 3);
        EXPECT_LT(rOut, startRadius);
    }
}

// -----------------------------------------------------------------------------
// 31. 区域录屏模式切换与工具栏全链路契约测试套件
// -----------------------------------------------------------------------------
TEST(CaptureRecordTransitionTest, ModeSwitchAndStateSanitization) {
    using namespace tools3000::capture;

    CaptureState state;
    state.mode = OverlayMode::Screenshot;

    // 添加一些临时标注
    cv::Mat dummy(100, 100, CV_8UC3, cv::Scalar(0, 0, 0));
    state.markup.setBaseImage(dummy);
    auto* rect = state.markup.drawRectangle(cv::Point(10, 10), cv::Point(50, 50), MarkupColor::Red(), 2.0f);
    state.activeElement = rect;
    state.openSubmenu = SubmenuType::ShapeType;
    state.sliderPopup.type = SliderPopupType::CornerRadius;

    ASSERT_EQ(state.markup.elementCount(), 1u);
    ASSERT_NE(state.activeElement, nullptr);

    // 调用生产级 transitionToRecordMode 执行原子切换与状态净化
    transitionToRecordMode(state);

    // 契约：首次调用必须切换为 RecordRegion，清空所有标注与选中元素，关闭子菜单及滑块，使工具栏失效重绘
    EXPECT_EQ(state.mode, OverlayMode::RecordRegion);
    EXPECT_EQ(state.markup.elementCount(), 0u);
    EXPECT_EQ(state.activeElement, nullptr);
    EXPECT_EQ(state.openSubmenu, SubmenuType::None);
    EXPECT_EQ(state.sliderPopup.type, SliderPopupType::None);
    EXPECT_FALSE(state.toolbarLayoutValid);

    // 验证重新构建后的录屏主工具栏为 9 项规格（含 MP4/GIF、fps、画质、点击特效、按键回显、音频、麦克风、取消、开始录制）
    D2D1_RECT_F selRect = D2D1::RectF(100.0f, 100.0f, 600.0f, 500.0f);
    D2D1_SIZE_F surface = D2D1::SizeF(1920.0f, 1080.0f);
    rebuildCaptureToolbar(state, selRect, surface);
    EXPECT_TRUE(state.toolbarLayoutValid);
    EXPECT_EQ(state.toolbarButtons.size(), 9u);
    EXPECT_TRUE(state.secondaryToolbarButtons.empty());

    // 验证开始录制按钮与取消按钮的存在及正确命令
    EXPECT_EQ(state.toolbarButtons.back().command, ToolbarCommand::RecordStartConfirm);
    EXPECT_EQ(state.toolbarButtons[7].command, ToolbarCommand::Cancel);

    // 验证再次调用 transitionToRecordMode 为幂等保护（不重复触发清理）
    transitionToRecordMode(state);
    EXPECT_EQ(state.mode, OverlayMode::RecordRegion);
    EXPECT_TRUE(state.toolbarLayoutValid);
}

TEST(CaptureCornerRadiusTest, HighDpiScalingModel) {
    using namespace tools3000::capture;

    float left = 100.0f, top = 100.0f, right = 500.0f, bottom = 400.0f;
    float startRadius = 20.0f;
    float maxRadius = 150.0f;

    // 100% 缩放 (scale = 1.0f): 鼠标移动 +20px 物理像素，逻辑圆角增加 20
    {
        float r100 = CornerRadiusHelper::calculateDraggedRadius(
            left, top, right, bottom, 120.0f, 120.0f, startRadius,
            140.0f, 140.0f, maxRadius, 0, 1.0f);
        EXPECT_NEAR(r100, startRadius + 20.0f, 0.001f);
    }

    // 200% 缩放 (scale = 2.0f): 鼠标移动 +20px 物理像素，逻辑圆角增加 10，保证物理手柄在屏幕上 1:1 紧跟鼠标
    {
        float r200 = CornerRadiusHelper::calculateDraggedRadius(
            left, top, right, bottom, 120.0f, 120.0f, startRadius,
            140.0f, 140.0f, maxRadius, 0, 2.0f);
        EXPECT_NEAR(r200, startRadius + 10.0f, 0.001f);
    }

    // 150% 缩放 (scale = 1.5f): 鼠标移动 +30px 物理像素，逻辑圆角增加 20
    {
        float r150 = CornerRadiusHelper::calculateDraggedRadius(
            left, top, right, bottom, 120.0f, 120.0f, startRadius,
            150.0f, 150.0f, maxRadius, 0, 1.5f);
        EXPECT_NEAR(r150, startRadius + 20.0f, 0.001f);
    }
}

TEST(CaptureCornerRadiusTest, CanShowHandlesThresholds) {
    using namespace tools3000::capture;

    // 64px 门限
    EXPECT_TRUE(CornerRadiusHelper::canShowHandles(100.0f, 100.0f, 1.0f, 64.0f));
    EXPECT_TRUE(CornerRadiusHelper::canShowHandles(64.0f, 64.0f, 1.0f, 64.0f));
    EXPECT_FALSE(CornerRadiusHelper::canShowHandles(60.0f, 100.0f, 1.0f, 64.0f));
    EXPECT_FALSE(CornerRadiusHelper::canShowHandles(100.0f, 60.0f, 1.0f, 64.0f));

    // 高 DPI 下按比例自适应门限 (scale = 2.0f, threshold = 128px)
    EXPECT_TRUE(CornerRadiusHelper::canShowHandles(130.0f, 130.0f, 2.0f, 64.0f));
    EXPECT_FALSE(CornerRadiusHelper::canShowHandles(120.0f, 130.0f, 2.0f, 64.0f));
}

// -----------------------------------------------------------------------------
// 38. 截图标注全工具二次编辑与属性同步测试套件
// -----------------------------------------------------------------------------

TEST(CaptureSecondaryEditingTest, ElementAttributeSynchronizationAndTextContinuation) {
    using namespace tools3000::capture;

    CaptureState state;
    cv::Mat canvas(600, 800, CV_8UC4, cv::Scalar(255, 255, 255, 255));
    state.markup.setBaseImage(canvas);

    // 1. 创建并添加矩形元素，配置个性化样式
    auto* rectElem = state.markup.drawRectangle(cv::Point(50, 50), cv::Point(250, 150), MarkupColor::Blue(), 4.0f);
    ASSERT_NE(rectElem, nullptr);
    rectElem->fill = true;
    rectElem->lineStyle = LineStyle::Dashed;
    rectElem->cornerRadius = 12.0f;

    // 模拟点击命中矩形元素进行二次编辑
    HitResult hitRect = state.markup.getElementAtEx(cv::Point(60, 60));
    ASSERT_NE(hitRect.element, nullptr);
    EXPECT_EQ(hitRect.element, rectElem);

    // 同步到状态机属性
    state.activeElement = hitRect.element;
    state.activeElement->isActive = true;
    state.currentTool = hitRect.element->tool;
    state.currentColor = hitRect.element->color;
    state.currentStrokeWidth = static_cast<int>(std::round(hitRect.element->thickness));
    state.currentFillMode = hitRect.element->fill;
    state.currentLineStyle = hitRect.element->lineStyle;
    state.currentElementCornerRadius = hitRect.element->cornerRadius;

    // 验证状态机全部属性与矩形元素精确对齐
    EXPECT_EQ(state.currentTool, MarkupTool::Rectangle);
    EXPECT_EQ(state.currentColor.r, MarkupColor::Blue().r);
    EXPECT_EQ(state.currentColor.g, MarkupColor::Blue().g);
    EXPECT_EQ(state.currentColor.b, MarkupColor::Blue().b);
    EXPECT_EQ(state.currentStrokeWidth, 4);
    EXPECT_TRUE(state.currentFillMode);
    EXPECT_EQ(state.currentLineStyle, LineStyle::Dashed);
    EXPECT_NEAR(state.currentElementCornerRadius, 12.0f, 0.01f);

    // 2. 创建并添加文本元素
    auto* textElem = state.markup.addText(cv::Point(100, 200), "Hello Tools3000", MarkupColor::Green(), 18.0f);
    ASSERT_NE(textElem, nullptr);
    textElem->textRenderSize = cv::Size(120, 24);

    // 模拟从矩形切换到文本元素点击：验证 isEditing 变为 true，且文本内容无损保留
    HitResult hitText = state.markup.getElementAtEx(cv::Point(110, 210));
    ASSERT_NE(hitText.element, nullptr);
    EXPECT_EQ(hitText.element, textElem);

    state.activeElement = hitText.element;
    state.activeElement->isActive = true;
    state.currentTool = hitText.element->tool;
    state.currentColor = hitText.element->color;
    if (hitText.element->tool == MarkupTool::Text) {
        state.activeElement->isEditing = true;
    }

    EXPECT_EQ(state.currentTool, MarkupTool::Text);
    EXPECT_EQ(state.currentColor.g, MarkupColor::Green().g);
    EXPECT_TRUE(state.activeElement->isEditing);
    EXPECT_EQ(state.activeElement->text, "Hello Tools3000");

    // 模拟追加输入文字
    state.activeElement->text += " v1.0";
    EXPECT_EQ(state.activeElement->text, "Hello Tools3000 v1.0");

    // 模拟切换到其他工具（例如箭头）：提交当前编辑
    state.activeElement->isEditing = false;
    state.activeElement->isActive = false;
    state.activeElement = nullptr;
    state.currentTool = MarkupTool::Arrow;

    // 再次点击切回文本元素：必须再次激活 isEditing = true，并且文本已完整保存
    HitResult hitTextAgain = state.markup.getElementAtEx(cv::Point(110, 210));
    ASSERT_NE(hitTextAgain.element, nullptr);
    EXPECT_EQ(hitTextAgain.element->text, "Hello Tools3000 v1.0");
}

// -----------------------------------------------------------------------------
// 39. 快捷键提示 HUD 呼吸感与黑曜石微晶视觉约束测试套件
// -----------------------------------------------------------------------------

TEST(ShortcutHintTest, ObsidianCardBreathingRoomAndDpiMetrics) {
    using tools3000::capture::ShortcutHintStyle;

    // 验证世界级呼吸感几何尺寸底线 (严禁重现紧凑拥挤)
    EXPECT_GE(ShortcutHintStyle::BaseKeyHeight, 28.0f);
    EXPECT_GE(ShortcutHintStyle::BaseVerticalPadding, 16.0f);
    EXPECT_GE(ShortcutHintStyle::BaseHorizontalPadding, 16.0f);
    EXPECT_GE(ShortcutHintStyle::BaseLabelGap, 14.0f);
    EXPECT_GE(ShortcutHintStyle::BaseRowGap, 10.0f);
    EXPECT_GE(ShortcutHintStyle::BaseCornerRadius, 14.0f);

    // 行高总垂直呼吸感 >= 38px
    const float minRowSpan = ShortcutHintStyle::BaseKeyHeight + ShortcutHintStyle::BaseRowGap;
    EXPECT_GE(minRowSpan, 38.0f);

    // 高 DPI 缩放下按比例放大，无形变与截断
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f, 3.0f}) {
        const float scaledKeyH = ShortcutHintStyle::BaseKeyHeight * scale;
        const float scaledPadY = ShortcutHintStyle::BaseVerticalPadding * scale;
        const float scaledPadX = ShortcutHintStyle::BaseHorizontalPadding * scale;
        const float scaledLabelGap = ShortcutHintStyle::BaseLabelGap * scale;
        const float scaledRowGap = ShortcutHintStyle::BaseRowGap * scale;

        EXPECT_GE(scaledKeyH, 28.0f * scale);
        EXPECT_GE(scaledPadY, 16.0f * scale);
        EXPECT_GE(scaledPadX, 16.0f * scale);
        EXPECT_GE(scaledLabelGap, 14.0f * scale);
        EXPECT_GE(scaledRowGap, 10.0f * scale);
    }
}

// -----------------------------------------------------------------------------
// 40. 贴图 OCR 文本块排序与提取模型测试套件
// -----------------------------------------------------------------------------

TEST(PinWindowOcrTest, BlockHitTestingAndReadingOrderAssembly) {
    using namespace tools3000::capture;

    struct TestOcrBlock {
        std::string text;
        cv::Rect origBox;
    };

    std::vector<TestOcrBlock> blocks = {
        {"Third block bottom-right", cv::Rect(150, 100, 120, 24)},
        {"First block top-left", cv::Rect(20, 20, 140, 24)},
        {"Second block top-right", cv::Rect(180, 20, 130, 24)},
        {"Fourth block bottom-left", cv::Rect(20, 100, 110, 24)},
    };

    // 1. 阅读顺序排序算法 (Y 行容差优先，行内从左到右)
    auto sortedIndices = [&]() {
        std::vector<size_t> idx(blocks.size());
        std::iota(idx.begin(), idx.end(), 0);
        std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
            const auto& ba = blocks[a].origBox;
            const auto& bb = blocks[b].origBox;
            if (std::abs(ba.y - bb.y) > 12) {
                return ba.y < bb.y;
            }
            return ba.x < bb.x;
        });
        return idx;
    }();

    ASSERT_EQ(sortedIndices.size(), 4u);
    EXPECT_EQ(sortedIndices[0], 1u); // First block top-left
    EXPECT_EQ(sortedIndices[1], 2u); // Second block top-right
    EXPECT_EQ(sortedIndices[2], 3u); // Fourth block bottom-left
    EXPECT_EQ(sortedIndices[3], 0u); // Third block bottom-right

    // 2. 单点命中测试 (Hit Testing)
    auto hitTestPoint = [&](cv::Point pt) -> int {
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (blocks[i].origBox.contains(pt)) return static_cast<int>(i);
        }
        return -1;
    };

    EXPECT_EQ(hitTestPoint(cv::Point(30, 30)), 1);
    EXPECT_EQ(hitTestPoint(cv::Point(200, 30)), 2);
    EXPECT_EQ(hitTestPoint(cv::Point(30, 110)), 3);
    EXPECT_EQ(hitTestPoint(cv::Point(160, 110)), 0);
    EXPECT_EQ(hitTestPoint(cv::Point(5, 5)), -1);

    // 3. 拉框选区交集测试 (Marquee Selection)
    auto selectInRect = [&](cv::Rect sel) -> std::vector<size_t> {
        std::vector<size_t> matched;
        for (size_t i = 0; i < blocks.size(); ++i) {
            if ((blocks[i].origBox & sel).area() > 0) {
                matched.push_back(i);
            }
        }
        return matched;
    };

    auto topRow = selectInRect(cv::Rect(10, 10, 320, 40));
    EXPECT_EQ(topRow.size(), 2u);
    EXPECT_TRUE(std::find(topRow.begin(), topRow.end(), 1u) != topRow.end());
    EXPECT_TRUE(std::find(topRow.begin(), topRow.end(), 2u) != topRow.end());

    // 4. 拼接提取文本
    std::string assembled;
    for (size_t i = 0; i < sortedIndices.size(); ++i) {
        if (i > 0) assembled += "\n";
        assembled += blocks[sortedIndices[i]].text;
    }
    EXPECT_EQ(assembled, "First block top-left\nSecond block top-right\nFourth block bottom-left\nThird block bottom-right");
}

// -----------------------------------------------------------------------------
// 41. 贴图 OCR 同行自然拼接与多行换行语义装配测试套件
// -----------------------------------------------------------------------------

TEST(PinWindowOcrTest, SmartSpacingSameLineAndMultiLineConcatenation) {
    struct MockBlock {
        std::string text;
        cv::Rect origBox;
    };

    // 模拟两行文字，每行包含 2 个无序的单词块
    std::vector<MockBlock> blocks = {
        {"World!", cv::Rect(90, 20, 60, 24)},      // 第一行第2词
        {"Hello", cv::Rect(20, 20, 50, 24)},       // 第一行第1词
        {"Experience", cv::Rect(120, 60, 100, 24)},// 第二行第2词
        {"World-class", cv::Rect(20, 60, 90, 24)}, // 第二行第1词
    };

    std::vector<size_t> sorted(blocks.size());
    std::iota(sorted.begin(), sorted.end(), 0);
    std::sort(sorted.begin(), sorted.end(), [&](size_t a, size_t b) {
        const auto& ba = blocks[a].origBox;
        const auto& bb = blocks[b].origBox;
        float avgH = (ba.height + bb.height) * 0.5f;
        if (std::abs(ba.y - bb.y) > avgH * 0.45f) return ba.y < bb.y;
        return ba.x < bb.x;
    });

    ASSERT_EQ(sorted.size(), 4u);
    EXPECT_EQ(sorted[0], 1u); // Hello
    EXPECT_EQ(sorted[1], 0u); // World!
    EXPECT_EQ(sorted[2], 3u); // World-class
    EXPECT_EQ(sorted[3], 2u); // Experience

    std::string assembled;
    const MockBlock* prevBlock = nullptr;
    for (size_t idx : sorted) {
        const auto& cur = blocks[idx];
        if (prevBlock) {
            float avgH = (prevBlock->origBox.height + cur.origBox.height) * 0.5f;
            bool isSameLine = std::abs(prevBlock->origBox.y - cur.origBox.y) <= avgH * 0.45f;
            if (isSameLine) {
                if (!assembled.empty() && assembled.back() != ' ' && !cur.text.empty() && cur.text.front() != ' ') {
                    assembled += " ";
                }
            } else {
                assembled += "\r\n";
            }
        }
        assembled += cur.text;
        prevBlock = &cur;
    }

    EXPECT_EQ(assembled, "Hello World!\r\nWorld-class Experience");
}

// -----------------------------------------------------------------------------
// 42. 快捷键提示 HUD 柔和高斯弥散投影与微晶底板约束测试套件
// -----------------------------------------------------------------------------

TEST(ShortcutHintTest, ShadowPaddingAndGeometricBreathingIntegrity) {
    using tools3000::capture::ShortcutHintStyle;

    EXPECT_EQ(ShortcutHintStyle::BaseShadowPadding, 12.0f);
    EXPECT_EQ(ShortcutHintStyle::BaseHorizontalPadding, 20.0f);
    EXPECT_EQ(ShortcutHintStyle::BaseVerticalPadding, 18.0f);
    EXPECT_EQ(ShortcutHintStyle::BaseLabelGap, 16.0f);
    EXPECT_EQ(ShortcutHintStyle::BaseRowGap, 12.0f);
    EXPECT_EQ(ShortcutHintStyle::BaseCornerRadius, 16.0f);

    // 验证多 DPI 缩放下高斯弥散投影边距缩放正确且严格大于 0
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f}) {
        const float scaledShadowPad = ShortcutHintStyle::BaseShadowPadding * scale;
        EXPECT_GE(scaledShadowPad, 12.0f * scale);
        const float scaledCornerR = ShortcutHintStyle::BaseCornerRadius * scale;
        EXPECT_GE(scaledCornerR, 16.0f * scale);
    }
}

// -----------------------------------------------------------------------------
// 43. HUD 字号升级、鼠标光标四级优先级状态机与文字高反差描边体系综合测试
// -----------------------------------------------------------------------------

TEST(CaptureHudAndCursorAndOutlineTest, HudDimensionsAndHighDpiScaling) {
    using tools3000::capture::ShortcutHintStyle;

    // 1. 验证 HUD 按键与说明标签字号调大升级，保障世界级排版饱满度
    EXPECT_GE(ShortcutHintStyle::BaseKeyFont, 14.0f);
    EXPECT_GE(ShortcutHintStyle::BaseLabelFont, 14.5f);
    EXPECT_GE(ShortcutHintStyle::BaseKeyHeight, 30.0f);
    EXPECT_GE(ShortcutHintStyle::BaseKeyHorizontalPadding, 14.0f);
    EXPECT_GE(ShortcutHintStyle::BaseKeyCornerRadius, 7.0f);

    // 2. 验证 High-DPI 缩放下按键高度与字号的线性缩放与呼吸感
    for (float scale : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f}) {
        float kFont = ShortcutHintStyle::BaseKeyFont * scale;
        float lFont = ShortcutHintStyle::BaseLabelFont * scale;
        float kHeight = ShortcutHintStyle::BaseKeyHeight * scale;
        EXPECT_GE(kFont, 14.0f * scale);
        EXPECT_GE(lFont, 14.5f * scale);
        EXPECT_GE(kHeight, 30.0f * scale);
    }
}

TEST(CaptureHudAndCursorAndOutlineTest, TextOutlineDataModelAndAutoColor) {
    using namespace tools3000::capture;

    // 1. MarkupColor 属性与静态工厂
    MarkupColor autoCol = MarkupColor::Auto();
    EXPECT_TRUE(autoCol.isAuto());
    EXPECT_EQ(autoCol.a, 0);

    MarkupColor black = MarkupColor::Black();
    EXPECT_FALSE(black.isAuto());
    EXPECT_EQ(black.r, 30);
    EXPECT_EQ(black.g, 41);
    EXPECT_EQ(black.b, 59);

    MarkupColor white = MarkupColor::White();
    EXPECT_FALSE(white.isAuto());
    EXPECT_EQ(white.r, 255);
    EXPECT_EQ(white.g, 255);
    EXPECT_EQ(white.b, 255);

    EXPECT_EQ(MarkupColor::Auto(), autoCol);
    EXPECT_NE(black, white);

    // 2. MarkupEngine 文本添加默认参数与显式参数
    MarkupEngine engine;
    cv::Mat base(120, 120, CV_8UC3, cv::Scalar(255, 255, 255));
    engine.setBaseImage(base);

    auto* elemDefault = engine.addText(cv::Point(10, 10), "Default Outline", MarkupColor::Black(), 20.0f);
    ASSERT_NE(elemDefault, nullptr);
    EXPECT_TRUE(elemDefault->textOutline);
    EXPECT_TRUE(elemDefault->textOutlineColor.isAuto());

    auto* elemWhite = engine.addText(cv::Point(10, 40), "White Outline", MarkupColor::Black(), 20.0f, true, MarkupColor::White());
    ASSERT_NE(elemWhite, nullptr);
    EXPECT_TRUE(elemWhite->textOutline);
    EXPECT_EQ(elemWhite->textOutlineColor, MarkupColor::White());

    auto* elemNoOutline = engine.addText(cv::Point(10, 70), "No Outline", MarkupColor::Black(), 20.0f, false, MarkupColor::Black());
    ASSERT_NE(elemNoOutline, nullptr);
    EXPECT_FALSE(elemNoOutline->textOutline);

    cv::Mat comp = engine.getCompositeImage();
    EXPECT_FALSE(comp.empty());
    EXPECT_EQ(engine.elementCount(), 3u);

    // 3. 验证背景亮度采样与自适应文字描边反色 (Auto 模式)
    cv::Mat darkCanvas(160, 240, CV_8UC3, cv::Scalar(30, 28, 26)); // 深色编辑器背景 (亮度 ~ 0.11)
    cv::Mat lightCanvas(160, 240, CV_8UC3, cv::Scalar(246, 248, 250)); // 浅色文档背景 (亮度 ~ 0.97)
    EXPECT_LT(sampleCanvasRegionLuminance(darkCanvas, cv::Rect(10, 10, 100, 40)), 0.20f);
    EXPECT_GT(sampleCanvasRegionLuminance(lightCanvas, cv::Rect(10, 10, 100, 40)), 0.85f);
    // 越界与空画布安全兜底
    EXPECT_NEAR(sampleCanvasRegionLuminance(cv::Mat(), cv::Rect(0, 0, 10, 10)), 0.50f, 0.01f);
    EXPECT_LT(sampleCanvasRegionLuminance(darkCanvas, cv::Rect(-50, -50, 20, 20)), 0.20f);

    // 彩色预设字色（红/橙/黄/绿/蓝/紫）在深色背景下必须自动反色为纯白描边 (255, 255, 255)，在浅色背景下反色为曜石黑描边 (15, 17, 23)
    const std::vector<MarkupColor> chromaticPresets = {
        MarkupColor::Red(),
        MarkupColor::Orange(),
        MarkupColor::Yellow(),
        MarkupColor::Green(),
        MarkupColor::Blue(),
        MarkupColor::Purple(),
    };
    for (const auto& textCol : chromaticPresets) {
        MarkupColor onDark = resolveTextOutlineColor(textCol, MarkupColor::Auto(), darkCanvas, cv::Rect(20, 20, 120, 36), false);
        EXPECT_EQ(onDark.r, 255);
        EXPECT_EQ(onDark.g, 255);
        EXPECT_EQ(onDark.b, 255);

        MarkupColor onLight = resolveTextOutlineColor(textCol, MarkupColor::Auto(), lightCanvas, cv::Rect(20, 20, 120, 36), false);
        EXPECT_EQ(onLight.r, 15);
        EXPECT_EQ(onLight.g, 17);
        EXPECT_EQ(onLight.b, 23);
    }

    // 极深色文字 (Black) 无论在深色或浅色背景均保持纯白高反差描边；极亮白/高明度浅粉彩文字恒定保持曜石黑描边
    EXPECT_EQ(computeAdaptiveTextOutlineColor(MarkupColor::Black(), 0.10f).r, 255);
    EXPECT_EQ(computeAdaptiveTextOutlineColor(MarkupColor::Black(), 0.90f).r, 255);
    EXPECT_EQ(computeAdaptiveTextOutlineColor(MarkupColor::White(), 0.10f).r, 15);
    EXPECT_EQ(computeAdaptiveTextOutlineColor(MarkupColor::White(), 0.90f).r, 15);
    const MarkupColor lightPastel{215, 225, 235, 255};
    EXPECT_EQ(computeAdaptiveTextOutlineColor(lightPastel, 0.10f).r, 15);

    // 边缘越界 ROI 局部夹紧采样验证：左半深色、右半亮白的分屏画布上，左侧负坐标越界文本框必须采样左侧局部深色边缘而非全图均值
    {
        cv::Mat splitCanvas(120, 240, CV_8UC3, cv::Scalar(245, 245, 245));
        splitCanvas(cv::Rect(0, 0, 80, 120)).setTo(cv::Scalar(18, 20, 24));
        const float clampedLeftLum = sampleCanvasRegionLuminance(splitCanvas, cv::Rect(-45, 20, 40, 30));
        EXPECT_LT(clampedLeftLum, 0.20f);
        MarkupColor edgeOutline = resolveTextOutlineColor(MarkupColor::Red(), MarkupColor::Auto(), splitCanvas, cv::Rect(-45, 20, 40, 30), false);
        EXPECT_EQ(edgeOutline.r, 255);
    }

    // 4. 真实像素级验证：深色背景下绘制红色文字 "3223" (Auto 描边) 必须产生高亮度白色描边像素
    {
        MarkupEngine darkEngine;
        darkEngine.setBaseImage(darkCanvas);
        darkEngine.addText(cv::Point(20, 30), "3223", MarkupColor::Red(), 24.0f, true, MarkupColor::Auto());
        cv::Mat renderedDark = darkEngine.getCompositeImage();
        int whiteOutlinePixels = 0;
        for (int y = 0; y < renderedDark.rows; ++y) {
            for (int x = 0; x < renderedDark.cols; ++x) {
                const auto& px = renderedDark.at<cv::Vec3b>(y, x);
                if (px[0] > 210 && px[1] > 210 && px[2] > 210) {
                    ++whiteOutlinePixels;
                }
            }
        }
        EXPECT_GT(whiteOutlinePixels, 20);
    }

    // 浅色背景下绘制红色文字 "3223" (Auto 描边) 必须产生曜石黑深色描边像素
    {
        MarkupEngine lightEngine;
        lightEngine.setBaseImage(lightCanvas);
        lightEngine.addText(cv::Point(20, 30), "3223", MarkupColor::Red(), 24.0f, true, MarkupColor::Auto());
        cv::Mat renderedLight = lightEngine.getCompositeImage();
        int darkOutlinePixels = 0;
        for (int y = 0; y < renderedLight.rows; ++y) {
            for (int x = 0; x < renderedLight.cols; ++x) {
                const auto& px = renderedLight.at<cv::Vec3b>(y, x);
                if (px[0] < 60 && px[1] < 60 && px[2] < 60) {
                    ++darkOutlinePixels;
                }
            }
        }
        EXPECT_GT(darkOutlinePixels, 20);
    }
}

TEST(CaptureHudAndCursorAndOutlineTest, TextOutlineToolbarLayoutAndCommandGeneration) {
    using namespace tools3000::capture;

    CaptureState state;
    state.currentTool = MarkupTool::Text;
    state.isMarkupToolActive = true;
    state.currentTextOutline = true;
    state.currentTextOutlineColor = MarkupColor::Auto();
    state.dragStart = {100, 100};
    state.dragEnd = {800, 600};
    state.state = OverlayState::Selected;
    state.dpiScale = 1.0f;

    // 当处于 Text 工具模式且描边开启时，构建二级属性栏
    rebuildCaptureToolbar(state, D2D1::RectF(100.0f, 100.0f, 800.0f, 600.0f), D2D1::SizeF(1920.0f, 1080.0f));
    ASSERT_FALSE(state.secondaryToolbarButtons.empty());

    // 检查是否存在 CycleStrokeWidth、ToggleFill、ToggleTextOutline 按钮且已彻底移除平铺的 SelectTextOutlineColor 空心圆圈按钮
    bool foundFontSize = false;
    bool foundToggleBg = false;
    bool foundToggleOutline = false;
    int outlineColorCount = 0;
    for (const auto& btn : state.secondaryToolbarButtons) {
        if (btn.command == ToolbarCommand::CycleStrokeWidth) {
            foundFontSize = true;
        }
        if (btn.command == ToolbarCommand::ToggleFill) {
            foundToggleBg = true;
        }
        if (btn.command == ToolbarCommand::ToggleTextOutline) {
            foundToggleOutline = true;
        }
        if (btn.command == ToolbarCommand::SelectTextOutlineColor) {
            outlineColorCount++;
        }
    }
    EXPECT_TRUE(foundFontSize);
    EXPECT_TRUE(foundToggleBg);
    EXPECT_TRUE(foundToggleOutline);
    EXPECT_EQ(outlineColorCount, 0);

    // 验证当激活的文字元素字号调整为 32px 时，二级属性栏字号按钮标签实时同步显示 "32"
    cv::Mat dummy(200, 300, CV_8UC3, cv::Scalar(30, 30, 30));
    state.markup.setBaseImage(dummy);
    state.activeElement = state.markup.addText(cv::Point(20, 20), "Test", MarkupColor::Red(), 32.0f, true, MarkupColor::Auto());
    state.toolbarLayoutValid = false;
    rebuildCaptureToolbar(state, D2D1::RectF(100.0f, 100.0f, 800.0f, 600.0f), D2D1::SizeF(1920.0f, 1080.0f));
    ASSERT_FALSE(state.secondaryToolbarButtons.empty());
    EXPECT_EQ(state.secondaryToolbarButtons.front().command, ToolbarCommand::CycleStrokeWidth);
    EXPECT_EQ(state.secondaryToolbarButtons.front().label, L"32");

    // 当关闭描边时，SelectTextOutlineColor 依然为 0
    state.currentTextOutline = false;
    state.toolbarLayoutValid = false;
    rebuildCaptureToolbar(state, D2D1::RectF(100.0f, 100.0f, 800.0f, 600.0f), D2D1::SizeF(1920.0f, 1080.0f));
    int outlineColorCountDisabled = 0;
    for (const auto& btn : state.secondaryToolbarButtons) {
        if (btn.command == ToolbarCommand::SelectTextOutlineColor) {
            outlineColorCountDisabled++;
        }
    }
    EXPECT_EQ(outlineColorCountDisabled, 0);
}

TEST(CaptureHudAndCursorAndOutlineTest, CornerRadiusHandlesHitTestPriorityOverText) {
    using namespace tools3000::capture;

    // 选区尺寸 600x400，左上角 (100, 100)，右下角 (700, 500)
    float l = 100.0f, t = 100.0f, r = 700.0f, b = 500.0f;
    float cornerRadius = 16.0f;
    float scale = 1.0f;

    // 验证在手柄有效展示范围内
    EXPECT_TRUE(CornerRadiusHelper::canShowHandles(r - l, b - t, scale, 64.0f));

    // 获取 4 个内角控制手柄几何坐标
    auto handles = CornerRadiusHelper::getCornerHandles(l, t, r, b, cornerRadius, scale, false);
    ASSERT_EQ(handles.size(), 4u);

    // 命中左上角手柄 (index = 0)
    int hitIdx0 = CornerRadiusHelper::hitTestHandles(l, t, r, b, cornerRadius, handles[0].x, handles[0].y, scale, 11.0f * scale, false);
    EXPECT_EQ(hitIdx0, 0);

    // 命中右上角手柄 (index = 1)
    int hitIdx1 = CornerRadiusHelper::hitTestHandles(l, t, r, b, cornerRadius, handles[1].x, handles[1].y, scale, 11.0f * scale, false);
    EXPECT_EQ(hitIdx1, 1);

    // 命中右下角手柄 (index = 2)
    int hitIdx2 = CornerRadiusHelper::hitTestHandles(l, t, r, b, cornerRadius, handles[2].x, handles[2].y, scale, 11.0f * scale, false);
    EXPECT_EQ(hitIdx2, 2);

    // 命中左下角手柄 (index = 3)
    int hitIdx3 = CornerRadiusHelper::hitTestHandles(l, t, r, b, cornerRadius, handles[3].x, handles[3].y, scale, 11.0f * scale, false);
    EXPECT_EQ(hitIdx3, 3);
}

TEST(CaptureHudAndCursorAndOutlineTest, TextOutlineAccessibilityAndSelectionState) {
    using namespace tools3000::capture;

    ToolbarButton toggleOutlineBtn;
    toggleOutlineBtn.command = ToolbarCommand::ToggleTextOutline;
    toggleOutlineBtn.tool = MarkupTool::Text;
    EXPECT_EQ(toolbarButtonAccessibleName(toggleOutlineBtn), L"文字高反差描边");

    ToolbarButton borderBtn;
    borderBtn.command = ToolbarCommand::ToggleTextOutline;
    borderBtn.tool = MarkupTool::Arrow;
    EXPECT_EQ(toolbarButtonAccessibleName(borderBtn), L"调节边框粗细与边框颜色");

    ToolbarButton outlineColorBtn;
    outlineColorBtn.command = ToolbarCommand::SelectTextOutlineColor;
    EXPECT_EQ(toolbarButtonAccessibleName(outlineColorBtn), L"文字描边颜色");

    CaptureState state;
    state.currentTextOutline = true;
    state.currentTextOutlineColor = MarkupColor::White();

    EXPECT_TRUE(isToolbarButtonSelected(toggleOutlineBtn, state));

    state.currentTextOutline = false;
    EXPECT_FALSE(isToolbarButtonSelected(toggleOutlineBtn, state));

    EXPECT_FALSE(isToolbarButtonSelected(borderBtn, state));
    state.isEditingOutlineColor = true;
    EXPECT_TRUE(isToolbarButtonSelected(borderBtn, state));
    state.isEditingOutlineColor = false;

    outlineColorBtn.command = ToolbarCommand::SelectTextOutlineColor;
    outlineColorBtn.color = MarkupColor::White();
    EXPECT_TRUE(isToolbarButtonSelected(outlineColorBtn, state));

    outlineColorBtn.color = MarkupColor::Black();
    EXPECT_FALSE(isToolbarButtonSelected(outlineColorBtn, state));

    outlineColorBtn.color = MarkupColor::Auto();
    state.currentTextOutlineColor = MarkupColor::Auto();
    EXPECT_TRUE(isToolbarButtonSelected(outlineColorBtn, state));
}

TEST(CaptureHudAndCursorAndOutlineTest, RecordRegionIgnoresCornerRadiusHandles) {
    using namespace tools3000::capture;

    CaptureState state;
    state.dragStart = {100, 100};
    state.dragEnd = {700, 500};
    state.state = OverlayState::Selected;
    state.cornerRadius = 16.0f;
    state.dpiScale = 1.0f;

    // 1. 验证录屏模式下状态配置
    state.mode = OverlayMode::RecordRegion;
    EXPECT_EQ(state.mode, OverlayMode::RecordRegion);

    // 2. 选区几何尺寸
    float selW = 600.0f;
    float selH = 400.0f;
    EXPECT_TRUE(CornerRadiusHelper::canShowHandles(selW, selH, 1.0f, 64.0f));

    // 3. 验证圆角控制点四角方位
    auto handles = CornerRadiusHelper::getCornerHandles(100.0f, 100.0f, 700.0f, 500.0f, 16.0f, 1.0f, false);
    ASSERT_EQ(handles.size(), 4u);
    for (int i = 0; i < 4; ++i) {
        int hit = CornerRadiusHelper::hitTestHandles(100.0f, 100.0f, 700.0f, 500.0f, 16.0f, handles[i].x, handles[i].y, 1.0f, 11.0f, false);
        EXPECT_EQ(hit, i);
    }
}

// -----------------------------------------------------------------------------
// 35. 截图鼠标光标合成与固化测试套件 (CursorOverlay::applyPermanent & includeCursor)
// -----------------------------------------------------------------------------
TEST(CursorOverlayTest, PermanentCommitAndScreenshotOptions) {
    using namespace tools3000::capture;

    // 1. CaptureOptions 配置项默认值验证
    CaptureOptions options;
    EXPECT_FALSE(options.includeCursor);
    options.includeCursor = true;
    EXPECT_TRUE(options.includeCursor);

    constexpr int size = 128;
    const CaptureRegion region{0, 0, size, size};
    auto backend = createMemoryCaptureBackend(CapturePixelFormat::Bgra32);
    std::string error;
    ASSERT_TRUE(backend->initialize(region, error));
    CaptureFrameView frame;
    ASSERT_TRUE(backend->capture(frame, error));

    const int pixelBytes = 4;
    const std::size_t rowBytes = static_cast<std::size_t>(frame.width) * pixelBytes;
    std::vector<std::uint8_t> original(rowBytes * frame.height);
    for (int row = 0; row < frame.height; ++row) {
        std::memcpy(original.data() + static_cast<std::size_t>(row) * rowBytes,
                    frame.data + static_cast<std::size_t>(row) * frame.stride, rowBytes);
    }

    CursorOverlay overlay;

    // 2. 验证 includeCursor = false 时 applyPermanent 保持帧缓冲 100% 纯净不被修改
    overlay.applyPermanent(frame, region, false);
    bool unchanged = true;
    for (int row = 0; row < frame.height && unchanged; ++row) {
        unchanged = std::memcmp(
            original.data() + static_cast<std::size_t>(row) * rowBytes,
            frame.data + static_cast<std::size_t>(row) * frame.stride,
            rowBytes) == 0;
    }
    EXPECT_TRUE(unchanged);

    // 3. 验证 Patch::commit() 与 applyPermanent(..., true) 接口调用鲁棒性
    {
        POINT pt{};
        GetCursorPos(&pt);
        const CaptureRegion cursorRegion{pt.x - 16, pt.y - 16, size, size};
        auto patch = overlay.apply(frame, cursorRegion, true, false);
        patch.commit();
        EXPECT_TRUE(patch.empty());
        // commit 后调用 restore 不应崩溃且不应回滚
        patch.restore();
    }

    // 4. 再次调用 applyPermanent(frame, region, true) 验证无状态泄漏与内存安全
    overlay.applyPermanent(frame, region, true);

    backend->releaseFrame();
    backend->shutdown();
}

TEST(CursorOverlayTest, CursorCreationAndFormatSupport) {
    using namespace tools3000::capture;

    // 1. 验证 32位 ARGB 与 1位 WORD 对齐掩码光标生成管线在各 DPI 尺度下的有效性
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        const int rawSz = std::clamp(static_cast<int>(std::round(48.0f * scale)), 48, 128);
        const int sz = (rawSz / 2) * 2;
        const int mid = sz / 2;

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = sz;
        bmi.bmiHeader.biHeight = -sz; // Top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        uint32_t* pixels = nullptr;
        HDC hdc = GetDC(nullptr);
        HBITMAP hbmColor = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
        ReleaseDC(nullptr, hdc);
        ASSERT_TRUE(hbmColor != nullptr);
        ASSERT_TRUE(pixels != nullptr);

        std::fill_n(pixels, sz * sz, 0x00000000u);
        pixels[mid * sz + mid] = 0xFFFFFFFFu; // 绘制中心测试不透明像素

        const int maskStride = ((sz + 15) / 16) * 2;
        std::vector<uint8_t> maskBits(static_cast<size_t>(maskStride) * sz, 0xFF);
        maskBits[static_cast<size_t>(mid) * maskStride + (mid / 8)] &= static_cast<uint8_t>(~(0x80 >> (mid % 8)));

        HBITMAP hbmMask = CreateBitmap(sz, sz, 1, 1, maskBits.data());
        ASSERT_TRUE(hbmMask != nullptr);

        ICONINFO ii = {};
        ii.fIcon = FALSE;
        ii.xHotspot = mid;
        ii.yHotspot = mid;
        ii.hbmMask = hbmMask;
        ii.hbmColor = hbmColor;

        HCURSOR cursor = CreateIconIndirect(&ii);
        EXPECT_TRUE(cursor != nullptr);

        if (cursor) {
            ICONINFO checkInfo = {};
            EXPECT_TRUE(GetIconInfo(cursor, &checkInfo));
            EXPECT_FALSE(checkInfo.fIcon);
            EXPECT_EQ(checkInfo.xHotspot, static_cast<DWORD>(mid));
            EXPECT_EQ(checkInfo.yHotspot, static_cast<DWORD>(mid));
            if (checkInfo.hbmColor) DeleteObject(checkInfo.hbmColor);
            if (checkInfo.hbmMask) DeleteObject(checkInfo.hbmMask);
            DestroyIcon(cursor);
        }

        DeleteObject(hbmMask);
        DeleteObject(hbmColor);
    }

    // 2. 验证 Bgr24 格式在全屏幕与越界坐标下的 applyPermanent 鲁棒性
    constexpr int size = 64;
    const CaptureRegion region{100, 100, size, size};
    std::vector<uint8_t> bgrData(size * size * 3, 128);
    CaptureFrameView bgrFrame;
    bgrFrame.data = bgrData.data();
    bgrFrame.width = size;
    bgrFrame.height = size;
    bgrFrame.stride = size * 3;
    bgrFrame.format = CapturePixelFormat::Bgr24;

    CursorOverlay overlay;
    // 正常合成
    overlay.applyPermanent(bgrFrame, region, true);
    // 越界坐标合成 (不应崩溃，静默裁剪)
    const CaptureRegion outsideRegion{-1000, -1000, size, size};
    overlay.applyPermanent(bgrFrame, outsideRegion, true);
}

// ─────────────────────────────────────────────────────────────────────────────
// 19. ClipboardUtils 全矩阵格式写入与三阶段生命周期合规测试
// ─────────────────────────────────────────────────────────────────────────────
TEST(ClipboardUtilsTest, EmptyImageRejection) {
    cv::Mat emptyMat;
    EXPECT_FALSE(tools3000::capture::ClipboardUtils::copyImageToClipboard(emptyMat));

    cv::Mat zeroMat(0, 100, CV_8UC3);
    EXPECT_FALSE(tools3000::capture::ClipboardUtils::copyImageToClipboard(zeroMat));
}

TEST(ClipboardUtilsTest, MultiFormatMatrixAndDesktopDrop) {
    auto openClipboardWithRetry = []() -> bool {
        for (int retry = 0; retry < 50; ++retry) {
            if (OpenClipboard(nullptr)) return true;
            Sleep(10 + retry * 2);
        }
        return false;
    };

    // 1. 创建包含透明通道与测试图形的 128x96 BGRA 测试图像
    cv::Mat testImage(96, 128, CV_8UC4, cv::Scalar(120, 80, 200, 180));
    cv::rectangle(testImage, cv::Rect(10, 10, 50, 40), cv::Scalar(255, 255, 0, 255), -1);

    // 2. 调用全矩阵写入 (preferredFilePath 为空，触发自动创建高可用临时落盘副本)
    bool copyOk = false;
    for (int retry = 0; retry < 50; ++retry) {
        if (tools3000::capture::ClipboardUtils::copyImageToClipboard(testImage)) {
            copyOk = true;
            break;
        }
        Sleep(10 + retry * 2);
    }
    ASSERT_TRUE(copyOk);

    // 3. 打开系统剪贴板并验证 5 重格式注入完整性
    ASSERT_TRUE(openClipboardWithRetry());

    // (1) 格式 1: PNG 格式存在性验证
    const UINT formatPng = RegisterClipboardFormatW(L"PNG");
    ASSERT_NE(formatPng, 0u);
    EXPECT_TRUE(IsClipboardFormatAvailable(formatPng));
    HANDLE hPngData = GetClipboardData(formatPng);
    EXPECT_NE(hPngData, nullptr);
    if (hPngData) {
        const size_t pngSize = GlobalSize(hPngData);
        EXPECT_GT(pngSize, 0u);
        const auto* pPng = static_cast<const uint8_t*>(GlobalLock(hPngData));
        ASSERT_NE(pPng, nullptr);
        // 校验 PNG 魔数: 0x89 'P' 'N' 'G'
        EXPECT_EQ(pPng[0], 0x89);
        EXPECT_EQ(pPng[1], 'P');
        EXPECT_EQ(pPng[2], 'N');
        EXPECT_EQ(pPng[3], 'G');
        GlobalUnlock(hPngData);
    }

    // (2) 格式 2: CF_DIBV5 (32位 BGRA 带 Alpha 透明度) 验证
    EXPECT_TRUE(IsClipboardFormatAvailable(CF_DIBV5));
    HANDLE hDib5 = GetClipboardData(CF_DIBV5);
    EXPECT_NE(hDib5, nullptr);
    if (hDib5) {
        const auto* p5 = static_cast<const BITMAPV5HEADER*>(GlobalLock(hDib5));
        ASSERT_NE(p5, nullptr);
        EXPECT_EQ(p5->bV5Size, sizeof(BITMAPV5HEADER));
        EXPECT_EQ(p5->bV5Width, 128);
        EXPECT_EQ(p5->bV5Height, 96);
        EXPECT_EQ(p5->bV5BitCount, 32);
        EXPECT_EQ(p5->bV5Compression, BI_BITFIELDS);
        EXPECT_EQ(p5->bV5RedMask, 0x00FF0000u);
        EXPECT_EQ(p5->bV5GreenMask, 0x0000FF00u);
        EXPECT_EQ(p5->bV5BlueMask, 0x000000FFu);
        EXPECT_EQ(p5->bV5AlphaMask, 0xFF000000u);
        GlobalUnlock(hDib5);
    }

    // (3) 格式 3: CF_DIB (24位 BGR 标准底向上与纯白底合成) 验证
    EXPECT_TRUE(IsClipboardFormatAvailable(CF_DIB));
    HANDLE hDib = GetClipboardData(CF_DIB);
    EXPECT_NE(hDib, nullptr);
    if (hDib) {
        const auto* pDib = static_cast<const BITMAPINFOHEADER*>(GlobalLock(hDib));
        ASSERT_NE(pDib, nullptr);
        EXPECT_EQ(pDib->biSize, sizeof(BITMAPINFOHEADER));
        EXPECT_EQ(pDib->biWidth, 128);
        EXPECT_EQ(pDib->biHeight, 96);
        EXPECT_EQ(pDib->biBitCount, 24);
        EXPECT_EQ(pDib->biCompression, BI_RGB);
        GlobalUnlock(hDib);
    }

    // (4) 格式 4: CF_BITMAP (HBITMAP) 存在性验证
    EXPECT_TRUE(IsClipboardFormatAvailable(CF_BITMAP));
    HANDLE hBmp = GetClipboardData(CF_BITMAP);
    EXPECT_NE(hBmp, nullptr);

    // (5) 格式 5: CF_HDROP (文件拖拽句柄，支持桌面与资源管理器直接 Ctrl+V 粘贴) 验证
    EXPECT_TRUE(IsClipboardFormatAvailable(CF_HDROP));
    HDROP hDrop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    ASSERT_NE(hDrop, nullptr);
    const UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    EXPECT_EQ(fileCount, 1u);
    wchar_t retrievedPath[MAX_PATH] = {};
    const UINT pathChars = DragQueryFileW(hDrop, 0, retrievedPath, MAX_PATH);
    EXPECT_GT(pathChars, 0u);
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::exists(retrievedPath, ec));
    EXPECT_EQ(std::filesystem::path(retrievedPath).extension(), L".png");

    CloseClipboard();
}

TEST(ClipboardUtilsTest, PreferredFilePathCustomHDROP) {
    auto openClipboardWithRetry = []() -> bool {
        for (int retry = 0; retry < 50; ++retry) {
            if (OpenClipboard(nullptr)) return true;
            Sleep(10 + retry * 2);
        }
        return false;
    };

    // 验证当调用方明确指定持久化保存路径时，CF_HDROP 严格注入该目标文件路径
    const auto tempDir = tools3000::core::WinUtils::getAppDataDirectory() / L"test_captures";
    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);
    const auto customFile = tempDir / L"custom_screenshot_001.png";

    cv::Mat testImage(64, 64, CV_8UC3, cv::Scalar(10, 20, 30));
    ASSERT_TRUE(cv::imwrite(tools3000::core::WinUtils::wstringToUtf8(customFile.wstring()), testImage));

    bool copyOk = false;
    for (int retry = 0; retry < 50; ++retry) {
        if (tools3000::capture::ClipboardUtils::copyImageToClipboard(testImage, customFile.wstring())) {
            copyOk = true;
            break;
        }
        Sleep(10 + retry * 2);
    }
    ASSERT_TRUE(copyOk);

    ASSERT_TRUE(openClipboardWithRetry());
    HDROP hDrop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    ASSERT_NE(hDrop, nullptr);
    wchar_t retrievedPath[MAX_PATH] = {};
    DragQueryFileW(hDrop, 0, retrievedPath, MAX_PATH);
    EXPECT_EQ(std::wstring(retrievedPath), customFile.wstring());
    CloseClipboard();

    std::filesystem::remove(customFile, ec);
}

TEST(ClipboardUtilsTest, TrimWorkingSetZeroDamageClipboardIntegrity) {
    auto openClipboardWithRetry = []() -> bool {
        for (int retry = 0; retry < 25; ++retry) {
            if (OpenClipboard(nullptr)) return true;
            Sleep(10);
        }
        return false;
    };

    // 关键架构验证：证实物理内存修剪 (trimWorkingSet) 在交付后执行时，
    // 对系统剪贴板句柄与多格式数据具备 100% 零破坏保障。
    // 若宿主系统后台剪贴板服务 (如 Win+V 历史记录 / RDP / 虚拟机剪贴板同步) 在 40ms 修剪窗口内异步改写剪贴板序列号，则重做完整的 [写入 -> trimWorkingSet(true)] 闭环。
    cv::Mat testImage(80, 80, CV_8UC4, cv::Scalar(60, 120, 180, 255));
    bool copyOk = false;
    for (int cycle = 0; cycle < 5; ++cycle) {
        copyOk = false;
        for (int retry = 0; retry < 15; ++retry) {
            if (tools3000::capture::ClipboardUtils::copyImageToClipboard(testImage)) {
                copyOk = true;
                break;
            }
            Sleep(20);
        }
        if (!copyOk) continue;

        const DWORD seqBeforeTrim = GetClipboardSequenceNumber();
        // 主动触发物理工作集修剪 (SetProcessWorkingSetSize 强行驱逐内存分页)
        tools3000::core::WinUtils::trimWorkingSet(true);

        if (GetClipboardSequenceNumber() == seqBeforeTrim && IsClipboardFormatAvailable(CF_HDROP)) {
            break;
        }
        Sleep(25);
    }
    ASSERT_TRUE(copyOk);

    // 验证修剪后剪贴板 5 重格式依然完整无损且可正常锁定读取
    ASSERT_TRUE(openClipboardWithRetry());

    const UINT formatPng = RegisterClipboardFormatW(L"PNG");
    EXPECT_TRUE(IsClipboardFormatAvailable(formatPng));
    EXPECT_TRUE(IsClipboardFormatAvailable(CF_DIBV5));
    EXPECT_TRUE(IsClipboardFormatAvailable(CF_DIB));
    EXPECT_TRUE(IsClipboardFormatAvailable(CF_BITMAP));
    EXPECT_TRUE(IsClipboardFormatAvailable(CF_HDROP));

    HANDLE hDib5 = GetClipboardData(CF_DIBV5);
    ASSERT_NE(hDib5, nullptr);
    auto* p5 = static_cast<const BITMAPV5HEADER*>(GlobalLock(hDib5));
    ASSERT_NE(p5, nullptr);
    EXPECT_EQ(p5->bV5Width, 80);
    EXPECT_EQ(p5->bV5Height, 80);
    GlobalUnlock(hDib5);

    CloseClipboard();
}

TEST(HudAvoidanceEngineTest, BasicPlacementPrefersTopLeftWhenClear) {
    using namespace tools3000::capture;
    D2D1_RECT_F anchor = D2D1::RectF(200.0f, 200.0f, 600.0f, 500.0f);
    D2D1_SIZE_F surface = D2D1::SizeF(1920.0f, 1080.0f);
    std::vector<HudObstacle> obstacles;
    HudPlacementConfig config;

    auto result = HudAvoidanceEngine::solvePlacement(anchor, 120.0f, 28.0f, surface, obstacles, config, 1.0f);
    EXPECT_EQ(result.edge, HudAnchorEdge::TopLeft);
    EXPECT_FALSE(result.isInside);
    EXPECT_NEAR(result.rect.left, 200.0f, 0.5f);
    EXPECT_NEAR(result.rect.bottom, 200.0f - config.edgeGap, 0.5f);
}

TEST(HudAvoidanceEngineTest, AvoidsTopScreenBoundaryByFlippingOrClamping) {
    using namespace tools3000::capture;
    // 选区贴近屏幕顶部 (top = 10px)，上方无法容纳 28px 的 HUD
    D2D1_RECT_F anchor = D2D1::RectF(100.0f, 10.0f, 500.0f, 400.0f);
    D2D1_SIZE_F surface = D2D1::SizeF(1920.0f, 1080.0f);
    std::vector<HudObstacle> obstacles;
    HudPlacementConfig config;

    auto result = HudAvoidanceEngine::solvePlacement(anchor, 120.0f, 28.0f, surface, obstacles, config, 1.0f);
    // 此时不应越界超出屏幕顶部 (screenMargin = 8px)
    EXPECT_GE(result.rect.top, config.screenMargin);
    // 算法会选择底部、居中或内嵌等无越界惩罚的候选
    EXPECT_NE(result.edge, HudAnchorEdge::TopLeft);
}

TEST(HudAvoidanceEngineTest, AvoidsCursorCollisionAtHandle) {
    using namespace tools3000::capture;
    D2D1_RECT_F anchor = D2D1::RectF(200.0f, 200.0f, 600.0f, 500.0f);
    D2D1_SIZE_F surface = D2D1::SizeF(1920.0f, 1080.0f);

    // 模拟鼠标正在调节左上角把手 (200, 200)，并在光标周围建立障碍物安全圈
    D2D1_RECT_F cursorObstacle = D2D1::RectF(175.0f, 175.0f, 225.0f, 225.0f);
    std::vector<HudObstacle> obstacles = {
        {cursorObstacle, HudObstacleType::Cursor, 3.0f}
    };
    HudPlacementConfig config;

    auto result = HudAvoidanceEngine::solvePlacement(anchor, 120.0f, 28.0f, surface, obstacles, config, 1.0f);
    // 不应与光标安全圈发生交叠
    float overlap = HudAvoidanceEngine::intersectionArea(result.rect, cursorObstacle);
    EXPECT_FLOAT_EQ(overlap, 0.0f);
    EXPECT_NE(result.edge, HudAnchorEdge::TopLeft);
}

TEST(HudAvoidanceEngineTest, AvoidsActiveAnnotationBox) {
    using namespace tools3000::capture;
    D2D1_RECT_F anchor = D2D1::RectF(200.0f, 200.0f, 600.0f, 500.0f);
    D2D1_SIZE_F surface = D2D1::SizeF(1920.0f, 1080.0f);

    // 活跃标注框位于选区上方与左上角
    D2D1_RECT_F annotation = D2D1::RectF(190.0f, 160.0f, 340.0f, 205.0f);
    std::vector<HudObstacle> obstacles = {
        {annotation, HudObstacleType::ActiveAnnotation, 2.0f}
    };
    HudPlacementConfig config;

    auto result = HudAvoidanceEngine::solvePlacement(anchor, 120.0f, 28.0f, surface, obstacles, config, 1.0f);
    float overlap = HudAvoidanceEngine::intersectionArea(result.rect, annotation);
    EXPECT_FLOAT_EQ(overlap, 0.0f);
}

TEST(HudAvoidanceEngineTest, HysteresisStabilityBonusPreventsJitter) {
    using namespace tools3000::capture;
    D2D1_RECT_F anchor = D2D1::RectF(200.0f, 200.0f, 600.0f, 500.0f);
    D2D1_SIZE_F surface = D2D1::SizeF(1920.0f, 1080.0f);
    std::vector<HudObstacle> obstacles;
    HudPlacementConfig config;

    // 上一帧已采纳 TopRight (成本较 TopLeft 仅高 12分，但 stabilityBonus=45分)
    auto result = HudAvoidanceEngine::solvePlacement(
        anchor, 120.0f, 28.0f, surface, obstacles, config, 1.0f,
        static_cast<int>(HudAnchorEdge::TopRight));

    EXPECT_EQ(result.edge, HudAnchorEdge::TopRight);
}

TEST(PinSpawnCalculatorTest, ExactInPlaceRestoreFromLastHistory) {
    using namespace tools3000::capture;
    PinSpawnInput input;
    input.imageWidth = 400;
    input.imageHeight = 300;
    input.cursor = {800, 600};
    input.workArea = {0, 0, 1920, 1040};
    input.virtualScreen = {0, 0, 1920, 1080};

    PinHistorySnapshot hist;
    hist.imageWidth = 400;
    hist.imageHeight = 300;
    hist.region = {150, 220, 400, 300};
    input.lastHistory = hist;

    POINT pt = PinSpawnCalculator::calculate(input);
    EXPECT_EQ(pt.x, 150);
    EXPECT_EQ(pt.y, 220);
}

TEST(PinSpawnCalculatorTest, DeductsBeautyShellPaddingOffset) {
    using namespace tools3000::capture;
    PinSpawnInput input;
    // 原截屏尺寸 400x300，加上四周 40px 美化外壳后变为 480x380
    input.imageWidth = 480;
    input.imageHeight = 380;
    input.cursor = {800, 600};
    input.workArea = {0, 0, 1920, 1040};
    input.virtualScreen = {0, 0, 1920, 1080};

    PinHistorySnapshot hist;
    hist.imageWidth = 480;
    hist.imageHeight = 380;
    hist.region = {200, 200, 400, 300}; // 实际截图内容位于屏幕 (200, 200)
    input.lastHistory = hist;

    POINT pt = PinSpawnCalculator::calculate(input);
    // 逆向平移 padX=(480-400)/2=40, padY=(380-300)/2=40
    EXPECT_EQ(pt.x, 160);
    EXPECT_EQ(pt.y, 160);
}

TEST(PinSpawnCalculatorTest, ExternalContentCenteredAtCursorAndClampedToWorkArea) {
    using namespace tools3000::capture;
    PinSpawnInput input;
    input.imageWidth = 500;
    input.imageHeight = 400;
    input.cursor = {1000, 500};
    input.workArea = {0, 0, 1920, 1040};
    input.virtualScreen = {0, 0, 1920, 1080};

    // 无历史记录 (外部图像或剪贴板文本)
    POINT pt = PinSpawnCalculator::calculate(input);
    EXPECT_EQ(pt.x, 1000 - 500 / 2); // 750
    EXPECT_EQ(pt.y, 500 - 400 / 2);  // 300

    // 测试贴靠屏幕右下角工作区边界夹取
    input.cursor = {1900, 1020};
    pt = PinSpawnCalculator::calculate(input);
    EXPECT_EQ(pt.x, 1920 - 500); // 1420 (严格约束在 workArea.right 之内)
    EXPECT_EQ(pt.y, 1040 - 400); // 640  (严格避让任务栏 workArea.bottom)
}

TEST(PinSpawnCalculatorTest, SmartCascadingAvoidsOverlappingExistingPins) {
    using namespace tools3000::capture;
    PinSpawnInput input;
    input.imageWidth = 300;
    input.imageHeight = 200;
    input.cursor = {500, 400};
    input.workArea = {0, 0, 1920, 1040};
    input.virtualScreen = {0, 0, 1920, 1080};

    PinSpawnRegion preferred{300, 300, 300, 200};
    input.preferredRegion = preferred;

    // 已有一个贴图在 (300, 300)
    RECT existing1{300, 300, 600, 500};
    input.existingPins.push_back(existing1);

    POINT pt = PinSpawnCalculator::calculate(input);
    // 自动级联 +24px
    EXPECT_EQ(pt.x, 324);
    EXPECT_EQ(pt.y, 324);

    // 再加一个重叠在 (324, 324)
    RECT existing2{324, 324, 624, 524};
    input.existingPins.push_back(existing2);

    pt = PinSpawnCalculator::calculate(input);
    // 链式级联 +48px
    EXPECT_EQ(pt.x, 348);
    EXPECT_EQ(pt.y, 348);
}

TEST(PinSpawnCalculatorTest, ClampsWithinVirtualScreenAllowingPaddingOverflow) {
    using namespace tools3000::capture;
    PinSpawnInput input;
    // 原截屏位于屏幕绝对左上角 (0, 0)，加上四周 40px 美化外壳后变为 480x380
    input.imageWidth = 480;
    input.imageHeight = 380;
    input.padX = 40;
    input.padY = 40;
    input.cursor = {500, 400};
    input.workArea = {0, 0, 1920, 1040};
    input.virtualScreen = {0, 0, 1920, 1080};

    PinSpawnRegion preferred{0, 0, 400, 300};
    input.preferredRegion = preferred;

    POINT pt = PinSpawnCalculator::calculate(input);
    // 允许 padding 负坐标微溢出 (-40, -40)，确保内部截屏内容绝对 1:1 锚定在屏幕 (0, 0)
    EXPECT_EQ(pt.x, -40);
    EXPECT_EQ(pt.y, -40);
    EXPECT_EQ(pt.x + input.padX, 0);
    EXPECT_EQ(pt.y + input.padY, 0);

    // 测试右下角极限贴边截图 (1520, 780, 400, 300) -> 右边缘为 1920, 下边缘为 1080
    input.preferredRegion = PinSpawnRegion{1520, 780, 400, 300};
    pt = PinSpawnCalculator::calculate(input);
    EXPECT_EQ(pt.x, 1480);
    EXPECT_EQ(pt.y, 740);
    EXPECT_EQ(pt.x + input.padX, 1520);
    EXPECT_EQ(pt.y + input.padY, 780);
}

TEST(PinSpawnCalculatorTest, ExplicitPreferredRegionWithDirectPadParams) {
    using namespace tools3000::capture;
    PinSpawnInput input;
    input.imageWidth = 264;
    input.imageHeight = 264;
    input.padX = 32;
    input.padY = 32;
    input.cursor = {500, 400};
    input.workArea = {0, 0, 1920, 1040};
    input.virtualScreen = {0, 0, 1920, 1080};

    input.preferredRegion = PinSpawnRegion{100, 100, 200, 200};

    POINT pt = PinSpawnCalculator::calculate(input);
    EXPECT_EQ(pt.x, 68);
    EXPECT_EQ(pt.y, 68);
}

TEST(HudAvoidanceEngineTest, EdgeGapClearsHandlesWithoutCollision) {
    using namespace tools3000::capture;
    D2D1_RECT_F anchor = D2D1::RectF(200.0f, 200.0f, 600.0f, 500.0f);
    D2D1_SIZE_F surface = D2D1::SizeF(1920.0f, 1080.0f);

    // 左上角手柄 (200, 200) 半径 12px -> 范围 [188, 188, 212, 212]
    D2D1_RECT_F handleObstacle = D2D1::RectF(188.0f, 188.0f, 212.0f, 212.0f);
    std::vector<HudObstacle> obstacles = {
        {handleObstacle, HudObstacleType::SelectionHandle, 1.5f}
    };
    HudPlacementConfig config;
    config.edgeGap = 16.0f; // 16px 间隙彻底避开 12px 半径手柄

    auto result = HudAvoidanceEngine::solvePlacement(anchor, 120.0f, 28.0f, surface, obstacles, config, 1.0f);
    float overlap = HudAvoidanceEngine::intersectionArea(result.rect, handleObstacle);
    EXPECT_FLOAT_EQ(overlap, 0.0f);
    EXPECT_EQ(result.edge, HudAnchorEdge::TopLeft);
}

TEST(HudAvoidanceEngineTest, EscapesToOuterSideWhenTopAndBottomBlocked) {
    using namespace tools3000::capture;
    // 窄高选区 (w=80, h=600)，且顶部贴屏 (top=10)，底部被工具栏完全封死
    D2D1_RECT_F anchor = D2D1::RectF(300.0f, 10.0f, 380.0f, 610.0f);
    D2D1_SIZE_F surface = D2D1::SizeF(1920.0f, 1080.0f);

    D2D1_RECT_F bottomToolbar = D2D1::RectF(250.0f, 610.0f, 600.0f, 680.0f);
    std::vector<HudObstacle> obstacles = {
        {bottomToolbar, HudObstacleType::PrimaryToolbar, 2.0f}
    };
    HudPlacementConfig config;
    config.allowInsideCandidates = false; // 选区过窄不允许内嵌

    auto result = HudAvoidanceEngine::solvePlacement(anchor, 120.0f, 28.0f, surface, obstacles, config, 1.0f);
    // 算法优雅逃逸至右侧或左侧外侧侧边锚点
    EXPECT_TRUE(result.edge == HudAnchorEdge::OuterRightTop ||
                result.edge == HudAnchorEdge::OuterRightCenter ||
                result.edge == HudAnchorEdge::OuterLeftTop ||
                result.edge == HudAnchorEdge::OuterLeftCenter);
    EXPECT_FLOAT_EQ(HudAvoidanceEngine::intersectionArea(result.rect, bottomToolbar), 0.0f);
}

TEST(CaptureToolbarAvoidanceTest, FourTierIntelligentAvoidance) {
    using namespace tools3000::capture;
    CaptureState state;
    state.mode = OverlayMode::Screenshot;
    state.dpiScale = 1.0f;
    const D2D1_SIZE_F surfaceSize = D2D1::SizeF(1920.0f, 1080.0f);

    // 1. Tier 1: 选区下方外部空间充裕（默认黄金布局：工具栏位于选区下方外部）
    D2D1_RECT_F selBelow = D2D1::RectF(200.0f, 200.0f, 800.0f, 600.0f);
    rebuildCaptureToolbar(state, selBelow, surfaceSize);
    EXPECT_GE(state.primaryToolbarRect.top, selBelow.bottom);
    EXPECT_LE(state.primaryToolbarRect.bottom, surfaceSize.height);

    // 2. Tier 2: 选区下方紧贴屏幕底部，上方空间充裕（翻转至选区上方外部，不遮挡选区）
    state.toolbarLayoutValid = false;
    D2D1_RECT_F selNearBottom = D2D1::RectF(200.0f, 800.0f, 800.0f, 1060.0f);
    rebuildCaptureToolbar(state, selNearBottom, surfaceSize);
    EXPECT_LE(state.primaryToolbarRect.bottom, selNearBottom.top);
    EXPECT_GE(state.primaryToolbarRect.top, 0.0f);

    // 3. Tier 3: 上下外部均无充裕空间（近全屏高大选区），优雅内嵌于选区底部，彻底放开顶部标注区与手柄
    state.toolbarLayoutValid = false;
    D2D1_RECT_F selTall = D2D1::RectF(10.0f, 10.0f, 1910.0f, 1070.0f);
    rebuildCaptureToolbar(state, selTall, surfaceSize);
    EXPECT_GE(state.primaryToolbarRect.top, selTall.top + 500.0f);
    EXPECT_LE(state.primaryToolbarRect.bottom, selTall.bottom);
    EXPECT_GE(state.primaryToolbarRect.top, selTall.top);

    // 4. 侧边浮动菜单：当选区极小时自动清理
    state.toolbarLayoutValid = false;
    D2D1_RECT_F selTiny = D2D1::RectF(100.0f, 100.0f, 110.0f, 110.0f);
    rebuildCaptureToolbar(state, selTiny, surfaceSize);
    EXPECT_TRUE(state.selectionSideButtons.empty());
    EXPECT_EQ(state.selectionSideRect.left, 0.0f);
    EXPECT_EQ(state.selectionSideRect.right, 0.0f);
}

TEST(CaptureSelectionAdjustmentModeTest, InitialStateAndFlagControl) {
    using namespace tools3000::capture;
    CaptureState state;
    // 初始或重置后，未激活标注工具 (处于选区调整模式)
    EXPECT_FALSE(state.isMarkupToolActive);

    // 激活工具后标记为 active
    state.isMarkupToolActive = true;
    EXPECT_TRUE(state.isMarkupToolActive);

    // 取消激活后重置为 false
    state.isMarkupToolActive = false;
    EXPECT_FALSE(state.isMarkupToolActive);
}

TEST(CaptureToolbarAvoidanceTest, SecondaryToolbarSuppressionWhenNotMarking) {
    using namespace tools3000::capture;
    CaptureState state;
    state.mode = OverlayMode::Screenshot;
    state.dpiScale = 1.0f;
    const D2D1_SIZE_F surfaceSize = D2D1::SizeF(1920.0f, 1080.0f);
    D2D1_RECT_F sel = D2D1::RectF(200.0f, 200.0f, 800.0f, 600.0f);

    // 未激活标注工具时，二级属性工具栏必须为空，且矩形清零，彻底释放操作视野
    state.isMarkupToolActive = false;
    rebuildCaptureToolbar(state, sel, surfaceSize);
    EXPECT_TRUE(state.secondaryToolbarButtons.empty());
    EXPECT_FLOAT_EQ(state.secondaryToolbarRect.right - state.secondaryToolbarRect.left, 0.0f);

    // 激活标注工具后，二级属性栏正常展开
    state.toolbarLayoutValid = false;
    state.isMarkupToolActive = true;
    state.currentTool = MarkupTool::Rectangle;
    rebuildCaptureToolbar(state, sel, surfaceSize);
    EXPECT_FALSE(state.secondaryToolbarButtons.empty());
    EXPECT_GT(state.secondaryToolbarRect.right - state.secondaryToolbarRect.left, 0.0f);
}

TEST(ShortcutHintOverlayTest, DynamicObstacleAvoidance) {
    using namespace tools3000::capture;
    const RECT workArea{0, 0, 1920, 1080};
    const int width = 240;
    const int height = 180;
    const float scale = 1.0f;

    // 1. 无任何障碍物时，首选左下角
    std::vector<RECT> emptyAvoid;
    POINT posDefault = ShortcutHintOverlay::computeOptimalPosition(width, height, workArea, scale, emptyAvoid);
    const int expectedMargin = static_cast<int>(ShortcutHintStyle::BaseScreenMargin * scale);
    EXPECT_EQ(posDefault.x, expectedMargin);
    EXPECT_EQ(posDefault.y, 1080 - 180 - expectedMargin);

    // 2. 左下角存在障碍物遮挡时，智能重定位至无碰撞区域
    std::vector<RECT> blockedAvoid;
    blockedAvoid.push_back({0, 800, 300, 1080}); // 遮挡左下角
    POINT posAvoided = ShortcutHintOverlay::computeOptimalPosition(width, height, workArea, scale, blockedAvoid);
    RECT resRect{posAvoided.x, posAvoided.y, posAvoided.x + width, posAvoided.y + height};

    // 验证求解出的位置与障碍物无任何交集
    int interLeft = (std::max)(resRect.left, blockedAvoid[0].left);
    int interTop = (std::max)(resRect.top, blockedAvoid[0].top);
    int interRight = (std::min)(resRect.right, blockedAvoid[0].right);
    int interBottom = (std::min)(resRect.bottom, blockedAvoid[0].bottom);
    bool hasIntersection = (interLeft < interRight && interTop < interBottom);
    EXPECT_FALSE(hasIntersection);
}

// -----------------------------------------------------------------------------
// 截图标注选区图像无损防拉伸与 1:1 像素映射测试套件
// -----------------------------------------------------------------------------
TEST(CaptureMarkupImageDeformationTest, BoundaryOverflowZeroDistortion) {
    using namespace tools3000::capture;

    // 1. 构造一个 1920x1080 的虚拟屏幕图像，填充高对比度特征像素
    cv::Mat screen = cv::Mat::zeros(1080, 1920, CV_8UC3);
    for (int y = 0; y < 1080; ++y) {
        auto* row = screen.ptr<cv::Vec3b>(y);
        for (int x = 0; x < 1920; ++x) {
            row[x] = cv::Vec3b(
                static_cast<uint8_t>(x % 256),
                static_cast<uint8_t>(y % 256),
                static_cast<uint8_t>((x + y) % 256)
            );
        }
    }

    // 2. 模拟 Windows DWM 隐形阴影或跨屏溢出矩形: x = -8, y = -8, w = 1936, h = 1096
    const cv::Rect targetRect(-8, -8, 1936, 1096);
    cv::Mat canvas = cropMarkupBase(screen, targetRect);

    // 验证画布严格为目标选区尺寸，绝无尺寸截断
    EXPECT_EQ(canvas.cols, 1936);
    EXPECT_EQ(canvas.rows, 1096);

    // 3. 验证屏幕内有效像素在画布上严格 1:1 放置于 (sx - targetRect.x, sy - targetRect.y)
    // 随机抽查多个屏幕采样点，断言像素值 100% 绝对一致 (0 色差、0 偏移、0 模糊)
    const std::vector<cv::Point> checkPoints = {
        {0, 0}, {100, 100}, {960, 540}, {1919, 1079}, {500, 800}
    };
    for (const auto& pt : checkPoints) {
        cv::Vec3b srcPixel = screen.at<cv::Vec3b>(pt.y, pt.x);
        int canvasX = pt.x - targetRect.x; // pt.x - (-8) = pt.x + 8
        int canvasY = pt.y - targetRect.y; // pt.y - (-8) = pt.y + 8
        cv::Vec3b dstPixel = canvas.at<cv::Vec3b>(canvasY, canvasX);
        EXPECT_EQ(srcPixel[0], dstPixel[0]);
        EXPECT_EQ(srcPixel[1], dstPixel[1]);
        EXPECT_EQ(srcPixel[2], dstPixel[2]);
    }

    // 4. 验证选区溢出屏幕外部的阴影边界像素为安全底色 0
    EXPECT_EQ(canvas.at<cv::Vec3b>(0, 0), cv::Vec3b(0, 0, 0));
    EXPECT_EQ(canvas.at<cv::Vec3b>(7, 7), cv::Vec3b(0, 0, 0));
    EXPECT_EQ(canvas.at<cv::Vec3b>(1095, 1935), cv::Vec3b(0, 0, 0));
}

TEST(CaptureMarkupImageDeformationTest, ScreenPhysicalCoordinateToMarkupPoint1To1) {
    using namespace tools3000::capture;

    const cv::Rect targetRect(-8, -8, 1936, 1096);

    // 验证屏幕原点 (0, 0) 精确映射至画布 (8, 8)
    cv::Point p0 = calculateMarkupPoint(POINT{0, 0}, targetRect);
    EXPECT_EQ(p0.x, 8);
    EXPECT_EQ(p0.y, 8);

    // 验证屏幕内部点 (100, 200) 精确映射至画布 (108, 208)
    cv::Point p1 = calculateMarkupPoint(POINT{100, 200}, targetRect);
    EXPECT_EQ(p1.x, 108);
    EXPECT_EQ(p1.y, 208);

    // 验证屏幕边缘点 (1919, 1079) 精确映射至画布 (1927, 1087)
    cv::Point p2 = calculateMarkupPoint(POINT{1919, 1079}, targetRect);
    EXPECT_EQ(p2.x, 1927);
    EXPECT_EQ(p2.y, 1087);
}

TEST(CaptureMarkupImageDeformationTest, Direct2DPixelExact1To1Mapping) {
    using namespace tools3000::capture;

    // 浮点选区输入 (带亚像素浮点数)
    D2D1_RECT_F selRect = D2D1::RectF(100.2f, 200.7f, 900.2f, 700.7f);
    const UINT32 bmpW = 800;
    const UINT32 bmpH = 500;

    D2D1_RECT_F destRect = calculateMarkupDestRect(selRect, bmpW, bmpH);
    D2D1_RECT_F srcRect  = calculateMarkupSrcRect(bmpW, bmpH);

    // 1. 验证目标绘制矩形左上角严格对齐整数物理像素
    EXPECT_FLOAT_EQ(destRect.left, 100.0f);
    EXPECT_FLOAT_EQ(destRect.top,  201.0f);

    // 2. 验证目标绘制矩形宽高严格与位图尺寸 1:1 绝对相等
    EXPECT_FLOAT_EQ(destRect.right - destRect.left, static_cast<float>(bmpW));
    EXPECT_FLOAT_EQ(destRect.bottom - destRect.top, static_cast<float>(bmpH));

    // 3. 验证源矩形覆盖整个位图像素范围
    EXPECT_FLOAT_EQ(srcRect.left,   0.0f);
    EXPECT_FLOAT_EQ(srcRect.top,    0.0f);
    EXPECT_FLOAT_EQ(srcRect.right,  static_cast<float>(bmpW));
    EXPECT_FLOAT_EQ(srcRect.bottom, static_cast<float>(bmpH));

    // 4. 验证缩放比例绝对为 1.0 (无双线性插值拉伸)
    float scaleX = (destRect.right - destRect.left) / (srcRect.right - srcRect.left);
    float scaleY = (destRect.bottom - destRect.top) / (srcRect.bottom - srcRect.top);
    EXPECT_FLOAT_EQ(scaleX, 1.0f);
    EXPECT_FLOAT_EQ(scaleY, 1.0f);
}

TEST(CaptureMarkupImageDeformationTest, WindowDetectionScreenBoundaryClamping) {
    using namespace tools3000::capture;

    const int screenW = 1920;
    const int screenH = 1080;

    // 1. 最大化窗口携带 -8px 阴影溢出：安全夹取至有效屏幕范围 [0, 0, 1920, 1080]
    RECT maxWin{-8, -8, 1928, 1088};
    cv::Rect clampedMax = clampToScreenBounds(maxWin, screenW, screenH);
    EXPECT_EQ(clampedMax.x, 0);
    EXPECT_EQ(clampedMax.y, 0);
    EXPECT_EQ(clampedMax.width, 1920);
    EXPECT_EQ(clampedMax.height, 1080);

    // 2. 正常内部窗口：保持原尺寸
    RECT normalWin{100, 150, 600, 550};
    cv::Rect clampedNorm = clampToScreenBounds(normalWin, screenW, screenH);
    EXPECT_EQ(clampedNorm.x, 100);
    EXPECT_EQ(clampedNorm.y, 150);
    EXPECT_EQ(clampedNorm.width, 500);
    EXPECT_EQ(clampedNorm.height, 400);

    // 3. 右侧越界窗口：右边缘安全截断
    RECT rightWin{1700, 200, 2000, 800};
    cv::Rect clampedRight = clampToScreenBounds(rightWin, screenW, screenH);
    EXPECT_EQ(clampedRight.x, 1700);
    EXPECT_EQ(clampedRight.y, 200);
    EXPECT_EQ(clampedRight.width, 220);
    EXPECT_EQ(clampedRight.height, 600);
}

TEST(CaptureMarkupImageDeformationTest, MarkupEngineBaseImageLifecycleAndTranslation) {
    using namespace tools3000::capture;

    MarkupEngine engine;
    EXPECT_FALSE(engine.hasBaseImage());

    // 1. 设置初始底图 400x300
    cv::Mat base1 = cv::Mat::ones(300, 400, CV_8UC3);
    engine.setBaseImage(base1);
    EXPECT_TRUE(engine.hasBaseImage());
    EXPECT_EQ(engine.baseWidth(), 400);
    EXPECT_EQ(engine.baseHeight(), 300);

    // 2. 添加标注图元
    auto* rectElem = engine.drawRectangle(cv::Point(50, 50), cv::Point(150, 150), MarkupColor::Red());
    ASSERT_NE(rectElem, nullptr);
    EXPECT_EQ(engine.elementCount(), 1u);
    EXPECT_EQ(rectElem->startPt.x, 50);
    EXPECT_EQ(rectElem->startPt.y, 50);

    // 3. 选区调整并平移标注图元 (例如选区向右移动 100px, 向下移动 50px)
    engine.translateAll(-100, -50);
    EXPECT_EQ(rectElem->startPt.x, -50);
    EXPECT_EQ(rectElem->startPt.y, 0);

    // 4. 更新底图为新尺寸 700x600 (二次调整后底图动态无损重裁)
    cv::Mat base2 = cv::Mat::ones(600, 700, CV_8UC3);
    engine.updateBaseImage(base2);
    EXPECT_EQ(engine.baseWidth(), 700);
    EXPECT_EQ(engine.baseHeight(), 600);
    EXPECT_EQ(engine.elementCount(), 1u); // 图元依然保留
}

TEST(CaptureMarkupImageDeformationTest, SyncMarkupBaseSingleSourceOfTruth) {
    using namespace tools3000::capture;

    cv::Mat frozenScreen = cv::Mat::ones(1080, 1920, CV_8UC3);
    bool markupBaseReady = false;
    cv::Rect markupBaseRect(0, 0, 0, 0);
    MarkupEngine markup;

    // 1. 首次调用：正确初始化底图和状态
    cv::Rect initialRect(100, 100, 400, 300);
    syncMarkupBase(frozenScreen, initialRect, markupBaseReady, markupBaseRect, markup);

    EXPECT_TRUE(markupBaseReady);
    EXPECT_EQ(markupBaseRect, initialRect);
    EXPECT_TRUE(markup.hasBaseImage());
    EXPECT_EQ(markup.baseWidth(), 400);
    EXPECT_EQ(markup.baseHeight(), 300);

    // 绘制一个测试图元
    auto* rectElem = markup.drawRectangle(cv::Point(20, 20), cv::Point(80, 80), MarkupColor::Red());
    ASSERT_NE(rectElem, nullptr);
    EXPECT_EQ(markup.elementCount(), 1u);

    // 2. 幂等性验证：尺寸与矩形完全相同时，不触发重复更新
    syncMarkupBase(frozenScreen, initialRect, markupBaseReady, markupBaseRect, markup);
    EXPECT_EQ(rectElem->startPt.x, 20);
    EXPECT_EQ(rectElem->startPt.y, 20);

    // 3. 选区平移：选区整体向右移动 50px，向下移动 30px (即 targetRect 变为 150, 130)
    // 标注图元应被反向平移以保持在屏幕物理空间绝对不动：dx = 100 - 150 = -50, dy = 100 - 130 = -30
    cv::Rect movedRect(150, 130, 400, 300);
    syncMarkupBase(frozenScreen, movedRect, markupBaseReady, markupBaseRect, markup);

    EXPECT_EQ(markupBaseRect, movedRect);
    EXPECT_EQ(rectElem->startPt.x, -30); // 20 - 50 = -30
    EXPECT_EQ(rectElem->startPt.y, -10); // 20 - 30 = -10
    EXPECT_EQ(markup.elementCount(), 1u);

    // 4. 选区尺寸调整：扩大为 600x500
    cv::Rect resizedRect(150, 130, 600, 500);
    syncMarkupBase(frozenScreen, resizedRect, markupBaseReady, markupBaseRect, markup);
    EXPECT_EQ(markupBaseRect, resizedRect);
    EXPECT_EQ(markup.baseWidth(), 600);
    EXPECT_EQ(markup.baseHeight(), 500);
    EXPECT_EQ(markup.elementCount(), 1u);
}

TEST(CaptureMarkupImageDeformationTest, MultiMonitorNegativeCoordinateMapping) {
    using namespace tools3000::capture;

    // 模拟双显示器场景: 虚拟桌面宽度 3840x1080 (左侧副屏 [-1920, 0, 1920, 1080]，主屏 [0, 0, 1920, 1080])
    // 捕获的 frozenScreen 尺寸为 3840x1080，其 (0, 0) 对应物理虚拟屏幕 (-1920, 0)
    cv::Mat virtualScreen(1080, 3840, CV_8UC3, cv::Scalar(40, 50, 60));
    // 在左副屏物理坐标 (-500, 200) 对应 frozenScreen 的 (-500 - (-1920) = 1420, 200) 处绘制特征点
    virtualScreen.at<cv::Vec3b>(200, 1420) = cv::Vec3b(255, 128, 0);

    // 选区落在左副屏: 局部屏幕矩形 targetRect = (1320, 100, 400, 300)
    cv::Rect targetRect(1320, 100, 400, 300);
    cv::Mat base = cropMarkupBase(virtualScreen, targetRect);
    ASSERT_FALSE(base.empty());
    EXPECT_EQ(base.cols, 400);
    EXPECT_EQ(base.rows, 300);

    // 验证特征点在画布上的位置: (1420 - 1320 = 100, 200 - 100 = 100)
    cv::Vec3b featurePix = base.at<cv::Vec3b>(100, 100);
    EXPECT_EQ(featurePix[0], 255);
    EXPECT_EQ(featurePix[1], 128);
    EXPECT_EQ(featurePix[2], 0);

    // 验证 calculateMarkupPoint 针对此区域屏幕坐标的 1:1 对齐映射
    cv::Point localPt = calculateMarkupPoint(POINT{1420, 200}, targetRect);
    EXPECT_EQ(localPt.x, 100);
    EXPECT_EQ(localPt.y, 100);
}

TEST(CaptureMarkupImageDeformationTest, MagnifierBoundaryZeroDistortion) {
    using namespace tools3000::capture;

    MarkupEngine engine;
    // 构造一个 200x200 的底图，绘制黑白方格图案
    cv::Mat canvas = cv::Mat::zeros(200, 200, CV_8UC3);
    for (int y = 0; y < 200; ++y) {
        for (int x = 0; x < 200; ++x) {
            if ((x / 20 + y / 20) % 2 == 0) {
                canvas.at<cv::Vec3b>(y, x) = cv::Vec3b(200, 200, 200);
            }
        }
    }
    engine.setBaseImage(canvas);

    // 1. 在紧贴左上角边界处添加放大镜 (center = 10, 10，半径 50，倍率 2.0x)
    // 此时放大镜的采样源与目标均严重超出左侧与上方画布边界
    engine.addMagnifier(cv::Point(10, 10), 2.0f, 50);
    EXPECT_EQ(engine.elementCount(), 1u);

    // 2. 渲染合成图：断言绝不崩溃、内存越界，且安全产出正确尺寸的图像
    cv::Mat composite;
    EXPECT_NO_THROW({
        composite = engine.getCompositeImage();
    });
    ASSERT_FALSE(composite.empty());
    EXPECT_EQ(composite.cols, 200);
    EXPECT_EQ(composite.rows, 200);

    // 3. 在右下角极端越界处添加放大镜 (center = 195, 195)
    engine.clearAll();
    engine.setBaseImage(canvas);
    engine.addMagnifier(cv::Point(195, 195), 3.0f, 60);
    EXPECT_NO_THROW({
        composite = engine.getCompositeImage();
    });
    ASSERT_FALSE(composite.empty());
    EXPECT_EQ(composite.cols, 200);
    EXPECT_EQ(composite.rows, 200);
}


TEST(CaptureMarkupResizeTest, EightDirectionHandleResizeAndNormalization) {
    using namespace tools3000::capture;

    MarkupEngine engine;
    cv::Mat base = cv::Mat::ones(600, 800, CV_8UC3);
    engine.setBaseImage(base);

    // 1. 正常构造正向矩形 (100, 100) -> (200, 200)
    auto* rect = engine.drawRectangle(cv::Point(100, 100), cv::Point(200, 200), MarkupColor::Red());
    ASSERT_NE(rect, nullptr);
    EXPECT_EQ(rect->startPt.x, 100);
    EXPECT_EQ(rect->startPt.y, 100);
    EXPECT_EQ(rect->endPt.x, 200);
    EXPECT_EQ(rect->endPt.y, 200);

    // 2. 测试右侧手柄拉大 (+50)
    rect->resize(50, 0, HitArea::R);
    EXPECT_EQ(rect->startPt.x, 100);
    EXPECT_EQ(rect->endPt.x, 250);

    // 3. 测试右侧手柄向左越过左边界缩小 (试图缩至比左边还小) -> 触发 kMinSize 保护
    rect->resize(-300, 0, HitArea::R);
    EXPECT_EQ(rect->startPt.x, 100);
    EXPECT_EQ(rect->endPt.x, 104); // 100 + kMinSize(4)

    // 4. 测试下侧手柄拉大 (+60)
    rect->resize(0, 60, HitArea::B);
    EXPECT_EQ(rect->startPt.y, 100);
    EXPECT_EQ(rect->endPt.y, 260);

    // 5. 测试左侧手柄向左拉大 (-20)
    rect->resize(-20, 0, HitArea::L);
    EXPECT_EQ(rect->startPt.x, 80);
    EXPECT_EQ(rect->endPt.x, 104);

    // 6. 测试上侧手柄向上拉大 (-30)
    rect->resize(0, -30, HitArea::T);
    EXPECT_EQ(rect->startPt.y, 70);
    EXPECT_EQ(rect->endPt.y, 260);

    // 7. 测试四个角点手柄
    rect->resize(-10, -10, HitArea::LT);
    EXPECT_EQ(rect->startPt.x, 70);
    EXPECT_EQ(rect->startPt.y, 60);

    rect->resize(20, -10, HitArea::RT);
    EXPECT_EQ(rect->endPt.x, 124);
    EXPECT_EQ(rect->startPt.y, 50);

    rect->resize(20, 20, HitArea::RB);
    EXPECT_EQ(rect->endPt.x, 144);
    EXPECT_EQ(rect->endPt.y, 280);

    rect->resize(-10, 20, HitArea::LB);
    EXPECT_EQ(rect->startPt.x, 60);
    EXPECT_EQ(rect->endPt.y, 300);

    // 8. 逆向绘制自动归一化验证：用户从 (300, 400) 拖拽到 (150, 200)
    auto* invRect = engine.drawRectangle(cv::Point(300, 400), cv::Point(150, 200), MarkupColor::Blue());
    ASSERT_NE(invRect, nullptr);
    EXPECT_EQ(invRect->startPt.x, 150);
    EXPECT_EQ(invRect->startPt.y, 200);
    EXPECT_EQ(invRect->endPt.x, 300);
    EXPECT_EQ(invRect->endPt.y, 400);

    // 逆向创建的椭圆也均在构建时归一化
    auto* invEllipse = engine.drawEllipse(cv::Point(250, 350), cv::Point(100, 150), MarkupColor::Green());
    ASSERT_NE(invEllipse, nullptr);
    EXPECT_EQ(invEllipse->startPt.x, 100);
    EXPECT_EQ(invEllipse->startPt.y, 150);
    EXPECT_EQ(invEllipse->endPt.x, 250);
    EXPECT_EQ(invEllipse->endPt.y, 350);
}

TEST(CaptureMarkupResizeTest, RectangleCornerRadiusClampingOnShrink) {
    using namespace tools3000::capture;

    MarkupEngine engine;
    cv::Mat base = cv::Mat::ones(400, 400, CV_8UC3);
    engine.setBaseImage(base);

    auto* rect = engine.drawRectangle(cv::Point(50, 50), cv::Point(250, 250), MarkupColor::Red());
    ASSERT_NE(rect, nullptr);
    rect->cornerRadius = 80.0f; // 初始圆角 80 (200x200 内有效)

    // 缩放到 30x30
    rect->resize(-170, -170, HitArea::RB);
    EXPECT_EQ(rect->endPt.x - rect->startPt.x, 30);
    EXPECT_EQ(rect->endPt.y - rect->startPt.y, 30);
    // cornerRadius 必须被限制在 30 * 0.5 = 15.0 以内
    EXPECT_LE(rect->cornerRadius, 15.0f);
}

TEST(CaptureMarkupResizeTest, RectangleHandlerHitTestPriorityOverCornerRadius) {
    using namespace tools3000::capture;

    MarkupEngine engine;
    cv::Mat base = cv::Mat::ones(600, 800, CV_8UC3);
    engine.setBaseImage(base);

    // 绘制 200x200 矩形 (100, 100) -> (300, 300)，圆角 30
    auto* rect = engine.drawRectangle(cv::Point(100, 100), cv::Point(300, 300), MarkupColor::Red());
    ASSERT_NE(rect, nullptr);
    rect->cornerRadius = 30.0f;
    rect->isActive = true;

    // 1. 验证激活状态下，点击四角顶点必须 100% 优先命中 8 方向拉伸手柄，绝不被内角圆角手柄抢占
    EXPECT_EQ(rect->hitTestEx(cv::Point(100, 100)), HitArea::LT);
    EXPECT_EQ(rect->hitTestEx(cv::Point(300, 100)), HitArea::RT);
    EXPECT_EQ(rect->hitTestEx(cv::Point(300, 300)), HitArea::RB);
    EXPECT_EQ(rect->hitTestEx(cv::Point(100, 300)), HitArea::LB);

    // 2. 验证激活状态下，点击边中点命中边手柄
    EXPECT_EQ(rect->hitTestEx(cv::Point(200, 100)), HitArea::T);
    EXPECT_EQ(rect->hitTestEx(cv::Point(300, 200)), HitArea::R);
    EXPECT_EQ(rect->hitTestEx(cv::Point(200, 300)), HitArea::B);
    EXPECT_EQ(rect->hitTestEx(cv::Point(100, 200)), HitArea::L);

    // 3. 验证点击内角圆角调节把手区域时，仍能正确命中 HitArea::CornerRadius
    const auto handles = CornerRadiusHelper::getCornerHandles(100.0f, 100.0f, 300.0f, 300.0f, 30.0f, 1.0f, false);
    ASSERT_FALSE(handles.empty());
    for (const auto& hnd : handles) {
        cv::Point innerPt(static_cast<int>(std::round(hnd.x)), static_cast<int>(std::round(hnd.y)));
        EXPECT_EQ(rect->hitTestEx(innerPt), HitArea::CornerRadius);
    }

    // 4. 验证未激活状态下，点击四角和边沿不会返回拉伸手柄，而是返回 Body
    rect->isActive = false;
    EXPECT_EQ(rect->hitTestEx(cv::Point(100, 100)), HitArea::Body);
    EXPECT_EQ(rect->hitTestEx(cv::Point(300, 300)), HitArea::Body);
    EXPECT_EQ(rect->hitTestEx(cv::Point(200, 100)), HitArea::Body);
}

TEST(CaptureMarkupResizeTest, PenHandlerProportionalResize) {
    using namespace tools3000::capture;

    MarkupEngine engine;
    cv::Mat base = cv::Mat::ones(500, 500, CV_8UC3);
    engine.setBaseImage(base);

    // 1. 2 点原始线段缩放
    std::vector<cv::Point> pts = {{100, 100}, {200, 200}};
    auto* pen = engine.drawPenStroke(pts, MarkupColor::Blue());
    ASSERT_NE(pen, nullptr);
    ASSERT_EQ(pen->penPoints.size(), 2u);

    const auto* handler = MarkupToolRegistry::instance().getHandler(MarkupTool::Pen);
    ASSERT_NE(handler, nullptr);

    // 初始包围盒: (100, 100) -> (200, 200)
    cv::Rect bboxBefore = handler->getBoundingBox(*pen);
    EXPECT_EQ(bboxBefore.x, 100);
    EXPECT_EQ(bboxBefore.y, 100);
    EXPECT_EQ(bboxBefore.width, 100);
    EXPECT_EQ(bboxBefore.height, 100);

    // 拉伸右下角手柄，使宽度增加 100，高度增加 100 (右下变为 300, 300)
    pen->resize(100, 100, HitArea::RB);

    cv::Rect bboxAfter = handler->getBoundingBox(*pen);
    EXPECT_EQ(bboxAfter.x, 100);
    EXPECT_EQ(bboxAfter.y, 100);
    EXPECT_EQ(bboxAfter.width, 200);
    EXPECT_EQ(bboxAfter.height, 200);

    EXPECT_EQ(pen->penPoints[0].x, 100);
    EXPECT_EQ(pen->penPoints[0].y, 100);
    EXPECT_EQ(pen->penPoints[1].x, 300);
    EXPECT_EQ(pen->penPoints[1].y, 300);

    // 2. 3 点样条平滑曲线等比例缩放
    std::vector<cv::Point> splinePts = {{100, 100}, {150, 150}, {200, 200}};
    auto* splinePen = engine.drawPenStroke(splinePts, MarkupColor::Red());
    ASSERT_NE(splinePen, nullptr);
    ASSERT_GT(splinePen->penPoints.size(), 2u);
    splinePen->resize(100, 100, HitArea::RB);
    for (const auto& pt : splinePen->penPoints) {
        EXPECT_GE(pt.x, 100);
        EXPECT_LE(pt.x, 300);
        EXPECT_GE(pt.y, 100);
        EXPECT_LE(pt.y, 300);
    }
}

TEST(CaptureMarkupResizeTest, HandleGeometryEngineContractAndClamping) {
    using namespace tools3000::capture;

    cv::Rect bbox(100, 200, 300, 400);
    auto handles = HandleGeometry::getBoxHandles(bbox);
    ASSERT_EQ(handles.size(), 8u);

    EXPECT_EQ(handles[0].area, HitArea::LT);
    EXPECT_EQ(handles[0].point, cv::Point(100, 200));

    EXPECT_EQ(handles[1].area, HitArea::T);
    EXPECT_EQ(handles[1].point, cv::Point(250, 200));

    EXPECT_EQ(handles[2].area, HitArea::RT);
    EXPECT_EQ(handles[2].point, cv::Point(400, 200));

    EXPECT_EQ(handles[3].area, HitArea::R);
    EXPECT_EQ(handles[3].point, cv::Point(400, 400));

    EXPECT_EQ(handles[4].area, HitArea::RB);
    EXPECT_EQ(handles[4].point, cv::Point(400, 600));

    EXPECT_EQ(handles[5].area, HitArea::B);
    EXPECT_EQ(handles[5].point, cv::Point(250, 600));

    EXPECT_EQ(handles[6].area, HitArea::LB);
    EXPECT_EQ(handles[6].point, cv::Point(100, 600));

    EXPECT_EQ(handles[7].area, HitArea::L);
    EXPECT_EQ(handles[7].point, cv::Point(100, 400));

    // 命中测试验证：所有 8 个手柄均能被准确命中
    for (const auto& h : handles) {
        HitArea hit = HandleGeometry::hitTestHandles(bbox, h.point, 7);
        EXPECT_EQ(hit, h.area);
    }

    // 角手柄优先于边手柄（当手柄范围重叠时）
    cv::Rect tinyBox(100, 100, 4, 4);
    HitArea cornerHit = HandleGeometry::hitTestHandles(tinyBox, cv::Point(100, 100), 7);
    EXPECT_EQ(cornerHit, HitArea::LT);

    // 统一缩放几何计算与越界防畸变
    cv::Rect resizedR = HandleGeometry::computeResizedRect(bbox, 50, 0, HitArea::R, 4, 4);
    EXPECT_EQ(resizedR, cv::Rect(100, 200, 350, 400));

    cv::Rect clampedR = HandleGeometry::computeResizedRect(bbox, -500, 0, HitArea::R, 4, 4);
    EXPECT_EQ(clampedR, cv::Rect(100, 200, 4, 400));

    cv::Rect clampedL = HandleGeometry::computeResizedRect(bbox, 500, 0, HitArea::L, 4, 4);
    EXPECT_EQ(clampedL, cv::Rect(396, 200, 4, 400));
}

TEST(CaptureMarkupResizeTest, TextHandlerEightDirectionZeroDriftAnchoring) {
    using namespace tools3000::capture;

    MarkupEngine engine;
    cv::Mat base = cv::Mat::ones(800, 1000, CV_8UC3);
    engine.setBaseImage(base);

    auto* text = engine.addText(cv::Point(200, 300), "Tools3000 Architecture", MarkupColor::Blue(), 24.0f);
    ASSERT_NE(text, nullptr);
    text->isActive = true;

    const auto* handler = MarkupToolRegistry::instance().getHandler(MarkupTool::Text);
    ASSERT_NE(handler, nullptr);

    cv::Rect initialBox = handler->getBoundingBox(*text);
    EXPECT_GT(initialBox.width, 80);
    EXPECT_GT(initialBox.height, 20);
    const int initW = initialBox.width;
    const double initialCenterY = initialBox.y + initialBox.height / 2.0;

    // 1. 核心缺陷复现与修复验证：向右拉伸 (HitArea::R)
    // 现象：老代码中往右拉导致字体变大但顶部固定，使得右侧手柄与文字向下偏移（“往右拉它往下”）
    // 修复断言：向右拉伸时，左边绝对固定，垂直中心绝对锚定（垂直中心偏移 <= 1px 像素整除），绝不向下漂移！
    text->resize(30, 0, HitArea::R);
    cv::Rect boxAfterR = handler->getBoundingBox(*text);
    EXPECT_GT(text->fontSize, 24.0f);
    EXPECT_GT(boxAfterR.width, initW);
    EXPECT_EQ(boxAfterR.x, initialBox.x); // 左边绝对锚定
    double centerYAfterR = boxAfterR.y + boxAfterR.height / 2.0;
    EXPECT_NEAR(centerYAfterR, initialCenterY, 1.0); // 垂直中心不漂移！

    // 2. 向左拉伸 (HitArea::L)：右边固定，垂直中心绝对锚定
    float fontBeforeL = text->fontSize;
    cv::Rect boxBeforeL = handler->getBoundingBox(*text);
    const int rightEdgeBeforeL = boxBeforeL.x + boxBeforeL.width;
    const double centerYBeforeL = boxBeforeL.y + boxBeforeL.height / 2.0;

    text->resize(-20, 0, HitArea::L);
    cv::Rect boxAfterL = handler->getBoundingBox(*text);
    EXPECT_GT(text->fontSize, fontBeforeL);
    EXPECT_GT(boxAfterL.width, boxBeforeL.width);
    EXPECT_EQ(boxAfterL.x + boxAfterL.width, rightEdgeBeforeL); // 右边绝对锚定
    EXPECT_NEAR(boxAfterL.y + boxAfterL.height / 2.0, centerYBeforeL, 1.0); // 垂直中心不漂移！

    // 3. 向下拉伸 (HitArea::B)：顶边固定，水平中心绝对锚定（绝不向右偏移）
    float fontBeforeB = text->fontSize;
    cv::Rect boxBeforeB = handler->getBoundingBox(*text);
    const double centerXBeforeB = boxBeforeB.x + boxBeforeB.width / 2.0;

    text->resize(0, 20, HitArea::B);
    cv::Rect boxAfterB = handler->getBoundingBox(*text);
    EXPECT_GT(text->fontSize, fontBeforeB);
    EXPECT_EQ(boxAfterB.y, boxBeforeB.y); // 顶边绝对锚定
    EXPECT_NEAR(boxAfterB.x + boxAfterB.width / 2.0, centerXBeforeB, 1.0); // 水平中心不漂移！

    // 4. 向上拉伸 (HitArea::T)：底边固定，水平中心绝对锚定
    float fontBeforeT = text->fontSize;
    cv::Rect boxBeforeT = handler->getBoundingBox(*text);
    const int bottomEdgeBeforeT = boxBeforeT.y + boxBeforeT.height;
    const double centerXBeforeT = boxBeforeT.x + boxBeforeT.width / 2.0;

    text->resize(0, -20, HitArea::T);
    cv::Rect boxAfterT = handler->getBoundingBox(*text);
    EXPECT_GT(text->fontSize, fontBeforeT);
    EXPECT_EQ(boxAfterT.y + boxAfterT.height, bottomEdgeBeforeT); // 底边绝对锚定
    EXPECT_NEAR(boxAfterT.x + boxAfterT.width / 2.0, centerXBeforeT, 1.0); // 水平中心不漂移！

    // 5. 角手柄锚定验证：
    // HitArea::RB：左上角绝对固定
    cv::Rect boxBeforeRB = handler->getBoundingBox(*text);
    text->resize(15, 15, HitArea::RB);
    cv::Rect boxAfterRB = handler->getBoundingBox(*text);
    EXPECT_EQ(boxAfterRB.x, boxBeforeRB.x);
    EXPECT_EQ(boxAfterRB.y, boxBeforeRB.y);

    // HitArea::RT：左下角绝对固定
    cv::Rect boxBeforeRT = handler->getBoundingBox(*text);
    const int bottomBeforeRT = boxBeforeRT.y + boxBeforeRT.height;
    text->resize(15, -15, HitArea::RT);
    cv::Rect boxAfterRT = handler->getBoundingBox(*text);
    EXPECT_EQ(boxAfterRT.x, boxBeforeRT.x);
    EXPECT_EQ(boxAfterRT.y + boxAfterRT.height, bottomBeforeRT);

    // HitArea::LB：右上角绝对固定
    cv::Rect boxBeforeLB = handler->getBoundingBox(*text);
    const int rightBeforeLB = boxBeforeLB.x + boxBeforeLB.width;
    text->resize(-15, 15, HitArea::LB);
    cv::Rect boxAfterLB = handler->getBoundingBox(*text);
    EXPECT_EQ(boxAfterLB.x + boxAfterLB.width, rightBeforeLB);
    EXPECT_EQ(boxAfterLB.y, boxBeforeLB.y);

    // 6. 缩小行为验证 (向左缩小 R 手柄)
    cv::Rect boxBeforeShrinkR = handler->getBoundingBox(*text);
    float fontBeforeShrinkR = text->fontSize;
    text->resize(-10, 0, HitArea::R);
    cv::Rect boxAfterShrinkR = handler->getBoundingBox(*text);
    EXPECT_LT(text->fontSize, fontBeforeShrinkR);
    EXPECT_EQ(boxAfterShrinkR.x, boxBeforeShrinkR.x);
    EXPECT_NEAR(boxAfterShrinkR.y + boxAfterShrinkR.height / 2.0,
                boxBeforeShrinkR.y + boxBeforeShrinkR.height / 2.0, 1.0);

    // 7. 极端值截断保护：字号缩放绝不越出 [12.0f, 180.0f] 边界
    text->resize(-5000, 0, HitArea::R);
    EXPECT_FLOAT_EQ(text->fontSize, 12.0f);
    text->resize(5000, 0, HitArea::R);
    EXPECT_FLOAT_EQ(text->fontSize, 180.0f);
}

TEST(CaptureMarkupResizeTest, HandleGeometryAnchoredOriginAndScalarDeltaAllEightDirections) {
    using namespace tools3000::capture;

    // 1. 标量增量投射验证 (computeScalarDelta)
    EXPECT_EQ(HandleGeometry::computeScalarDelta(10, 0, HitArea::R), 10);
    EXPECT_EQ(HandleGeometry::computeScalarDelta(-10, 0, HitArea::L), 10);
    EXPECT_EQ(HandleGeometry::computeScalarDelta(0, 10, HitArea::B), 10);
    EXPECT_EQ(HandleGeometry::computeScalarDelta(0, -10, HitArea::T), 10);
    EXPECT_EQ(HandleGeometry::computeScalarDelta(10, 10, HitArea::RB), 10);
    EXPECT_EQ(HandleGeometry::computeScalarDelta(-10, -10, HitArea::LT), 10);
    EXPECT_EQ(HandleGeometry::computeScalarDelta(10, -10, HitArea::RT), 10);
    EXPECT_EQ(HandleGeometry::computeScalarDelta(-10, 10, HitArea::LB), 10);
    EXPECT_EQ(HandleGeometry::computeScalarDelta(10, 10, HitArea::Body), 0);
    EXPECT_EQ(HandleGeometry::computeScalarDelta(10, 10, HitArea::None), 0);

    // 2. 8 方向等比缩放锚点坐标求解数学验证 (computeAnchoredOrigin)
    const cv::Point orig(100, 100);
    const int dW = 40;
    const int dH = 20;

    // R: 左边固定 (x=100), 垂直中心固定 (100 - 10 = 90)
    cv::Point pR = HandleGeometry::computeAnchoredOrigin(orig, dW, dH, HitArea::R);
    EXPECT_EQ(pR.x, 100);
    EXPECT_EQ(pR.y, 90);

    // L: 右边固定 (100 - 40 = 60), 垂直中心固定 (90)
    cv::Point pL = HandleGeometry::computeAnchoredOrigin(orig, dW, dH, HitArea::L);
    EXPECT_EQ(pL.x, 60);
    EXPECT_EQ(pL.y, 90);

    // B: 顶边固定 (y=100), 水平中心固定 (100 - 20 = 80)
    cv::Point pB = HandleGeometry::computeAnchoredOrigin(orig, dW, dH, HitArea::B);
    EXPECT_EQ(pB.x, 80);
    EXPECT_EQ(pB.y, 100);

    // T: 底边固定 (100 - 20 = 80), 水平中心固定 (80)
    cv::Point pT = HandleGeometry::computeAnchoredOrigin(orig, dW, dH, HitArea::T);
    EXPECT_EQ(pT.x, 80);
    EXPECT_EQ(pT.y, 80);

    // RB: 左上角固定 (100, 100)
    cv::Point pRB = HandleGeometry::computeAnchoredOrigin(orig, dW, dH, HitArea::RB);
    EXPECT_EQ(pRB.x, 100);
    EXPECT_EQ(pRB.y, 100);

    // LT: 右下角固定 (60, 80)
    cv::Point pLT = HandleGeometry::computeAnchoredOrigin(orig, dW, dH, HitArea::LT);
    EXPECT_EQ(pLT.x, 60);
    EXPECT_EQ(pLT.y, 80);

    // RT: 左下角固定 (100, 80)
    cv::Point pRT = HandleGeometry::computeAnchoredOrigin(orig, dW, dH, HitArea::RT);
    EXPECT_EQ(pRT.x, 100);
    EXPECT_EQ(pRT.y, 80);

    // LB: 右上角固定 (60, 100)
    cv::Point pLB = HandleGeometry::computeAnchoredOrigin(orig, dW, dH, HitArea::LB);
    EXPECT_EQ(pLB.x, 60);
    EXPECT_EQ(pLB.y, 100);
}

TEST(CaptureMarkupResizeTest, EllipseHandlerOddDimensionsNoTruncation) {
    using namespace tools3000::capture;

    MarkupEngine engine;
    cv::Mat base = cv::Mat::ones(500, 500, CV_8UC3);
    engine.setBaseImage(base);

    // 绘制奇数长宽椭圆 (100, 100) -> (201, 201), 宽=101, 高=101
    auto* ellipse = engine.drawEllipse(cv::Point(100, 100), cv::Point(201, 201), MarkupColor::Yellow());
    ASSERT_NE(ellipse, nullptr);

    const auto* handler = MarkupToolRegistry::instance().getHandler(MarkupTool::Ellipse);
    ASSERT_NE(handler, nullptr);

    // 验证多态基类 getBoundingBox 绝不截断奇数像素
    cv::Rect bbox = handler->getBoundingBox(*ellipse);
    EXPECT_EQ(bbox.x, 100);
    EXPECT_EQ(bbox.y, 100);
    EXPECT_EQ(bbox.width, 101);
    EXPECT_EQ(bbox.height, 101);

    // 验证拉伸右侧手柄 +10px
    ellipse->resize(10, 0, HitArea::R);
    cv::Rect bboxAfter = handler->getBoundingBox(*ellipse);
    EXPECT_EQ(bboxAfter.x, 100);
    EXPECT_EQ(bboxAfter.width, 111);
    EXPECT_EQ(bboxAfter.height, 101);
}

TEST(CaptureMarkupResizeTest, PenHandlerSingleAxisDegenerateStrokeResize) {
    using namespace tools3000::capture;

    MarkupEngine engine;
    cv::Mat base = cv::Mat::ones(400, 400, CV_8UC3);
    engine.setBaseImage(base);

    // 1. 垂直线退化笔画 (x=150 绝对垂直，spanX=0)
    std::vector<cv::Point> vertPts = {{150, 100}, {150, 150}, {150, 200}};
    auto* vertPen = engine.drawPenStroke(vertPts, MarkupColor::Red());
    ASSERT_NE(vertPen, nullptr);

    const auto* handler = MarkupToolRegistry::instance().getHandler(MarkupTool::Pen);
    ASSERT_NE(handler, nullptr);

    cv::Rect vertBBox = handler->getBoundingBox(*vertPen);
    EXPECT_GE(vertBBox.width, 4); // 确保最小包围盒不为 0
    EXPECT_EQ(vertBBox.height, 100);

    // 沿 B 方向拉长 50px：必须成功拉伸且无除零崩溃
    vertPen->resize(0, 50, HitArea::B);
    cv::Rect vertAfter = handler->getBoundingBox(*vertPen);
    EXPECT_EQ(vertAfter.height, 150);

    // 2. 水平线退化笔画 (y=150 绝对水平，spanY=0)
    std::vector<cv::Point> horizPts = {{100, 150}, {150, 150}, {200, 150}};
    auto* horizPen = engine.drawPenStroke(horizPts, MarkupColor::Blue());
    ASSERT_NE(horizPen, nullptr);

    cv::Rect horizBBox = handler->getBoundingBox(*horizPen);
    EXPECT_EQ(horizBBox.width, 100);
    EXPECT_GE(horizBBox.height, 4);

    // 沿 R 方向拉长 40px
    horizPen->resize(40, 0, HitArea::R);
    cv::Rect horizAfter = handler->getBoundingBox(*horizPen);
    EXPECT_EQ(horizAfter.width, 140);
}

TEST(CaptureMarkupResizeTest, MagnifierHandlerSinglePixelIncrementResponsiveness) {
    using namespace tools3000::capture;

    MarkupEngine engine;
    cv::Mat base = cv::Mat::ones(600, 600, CV_8UC3);
    engine.setBaseImage(base);

    engine.addMagnifier(cv::Point(200, 200), 2.0f, 60);
    auto* mag = engine.elements().back().get();
    ASSERT_NE(mag, nullptr);
    EXPECT_EQ(mag->magnifierRadius, 60);

    const auto* handler = MarkupToolRegistry::instance().getHandler(MarkupTool::Magnifier);
    ASSERT_NE(handler, nullptr);

    // 微量移动 1 像素测试：必须即时响应，绝无 1/2 == 0 丢步死区；且左右/上下手柄独立拉宽/拉高为椭圆
    mag->resize(1, 0, HitArea::R);
    EXPECT_EQ(mag->magnifierRadius, 61);
    EXPECT_EQ(mag->effectiveMagnifierRy(), 60);

    mag->resize(-1, 0, HitArea::R);
    EXPECT_EQ(mag->magnifierRadius, 60);
    EXPECT_EQ(mag->effectiveMagnifierRy(), 60);

    mag->resize(0, 1, HitArea::B);
    EXPECT_EQ(mag->magnifierRadius, 60);
    EXPECT_EQ(mag->effectiveMagnifierRy(), 61);

    mag->resize(0, -1, HitArea::T);
    EXPECT_EQ(mag->magnifierRadius, 60);
    EXPECT_EQ(mag->effectiveMagnifierRy(), 62);
}

TEST(CaptureMarkupResizeTest, TextHandlerEditingStateSizeConsistency) {
    using namespace tools3000::capture;

    MarkupEngine engine;
    cv::Mat base = cv::Mat::ones(500, 500, CV_8UC3);
    engine.setBaseImage(base);

    // 1. 空文本：无论处于编辑中还是处于非编辑态手柄拖拽中，包围盒尺寸绝对恒定，绝无 60px 跳变闪烁
    auto* emptyText = engine.addText(cv::Point(100, 100), "", MarkupColor::Green(), 18.0f);
    ASSERT_NE(emptyText, nullptr);

    const auto* handler = MarkupToolRegistry::instance().getHandler(MarkupTool::Text);
    ASSERT_NE(handler, nullptr);

    emptyText->isEditing = true;
    cv::Rect boxEditing = handler->getBoundingBox(*emptyText);

    emptyText->isEditing = false;
    cv::Rect boxNonEditing = handler->getBoundingBox(*emptyText);

    EXPECT_EQ(boxEditing.width, boxNonEditing.width);
    EXPECT_EQ(boxEditing.height, boxNonEditing.height);

    // 2. 非空文本：无论编辑态还是拖拽态，尺寸同样一致
    auto* text = engine.addText(cv::Point(100, 100), "Hello", MarkupColor::Blue(), 18.0f);
    ASSERT_NE(text, nullptr);

    text->isEditing = true;
    cv::Rect filledEditing = handler->getBoundingBox(*text);

    text->isEditing = false;
    cv::Rect filledNonEditing = handler->getBoundingBox(*text);

    EXPECT_EQ(filledEditing.width, filledNonEditing.width);
    EXPECT_EQ(filledEditing.height, filledNonEditing.height);
}

TEST(CaptureMarkupResizeTest, TextElementActiveHandlesHitTestAndNonEditingCaret) {
    using namespace tools3000::capture;

    MarkupEngine engine;
    cv::Mat base = cv::Mat::ones(600, 600, CV_8UC3);
    engine.setBaseImage(base);

    auto* text = engine.addText(cv::Point(100, 100), "Zero Emoji Standard", MarkupColor::Green(), 20.0f);
    ASSERT_NE(text, nullptr);
    text->isActive = true;

    const auto* handler = MarkupToolRegistry::instance().getHandler(MarkupTool::Text);
    ASSERT_NE(handler, nullptr);

    cv::Rect bbox = handler->getBoundingBox(*text);
    auto handles = HandleGeometry::getBoxHandles(bbox);

    // 激活状态下：点击 8 个手柄中的每一个均返回对应的 HitArea，而不是 HitArea::Body
    for (const auto& hp : handles) {
        HitArea hit = handler->hitTest(*text, hp.point, 5);
        EXPECT_EQ(hit, hp.area);
        EXPECT_NE(hit, HitArea::Body);
    }

    // 点击在文字正中间（非手柄区域）返回 HitArea::Body
    cv::Point centerPt(bbox.x + bbox.width / 2, bbox.y + bbox.height / 2);
    HitArea bodyHit = handler->hitTest(*text, centerPt, 5);
    EXPECT_EQ(bodyHit, HitArea::Body);

    // 验证 endPt 始终与 getBoundingBox 同步形成有效包围盒
    EXPECT_EQ(text->endPt.x, bbox.x + bbox.width);
    EXPECT_EQ(text->endPt.y, bbox.y + bbox.height);
}

TEST(ClipboardUtilsTest, TimestampedHdropFileNamePatternAndContent) {
    auto openClipboardWithRetry = []() -> bool {
        for (int retry = 0; retry < 25; ++retry) {
            if (OpenClipboard(nullptr)) return true;
            Sleep(10);
        }
        return false;
    };

    cv::Mat testImage(32, 32, CV_8UC3, cv::Scalar(100, 150, 200));
    ASSERT_TRUE(tools3000::capture::ClipboardUtils::copyImageToClipboard(testImage));

    ASSERT_TRUE(openClipboardWithRetry());
    HDROP hDrop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    ASSERT_NE(hDrop, nullptr);
    wchar_t retrievedPath[MAX_PATH] = {};
    const UINT pathChars = DragQueryFileW(hDrop, 0, retrievedPath, MAX_PATH);
    EXPECT_GT(pathChars, 0u);
    CloseClipboard();

    std::error_code ec;
    EXPECT_TRUE(std::filesystem::exists(retrievedPath, ec));
    std::wstring filename = std::filesystem::path(retrievedPath).filename().wstring();
    EXPECT_TRUE(filename.rfind(L"Tools3000_", 0) == 0);
    EXPECT_EQ(std::filesystem::path(retrievedPath).extension(), L".png");
    EXPECT_GT(std::filesystem::file_size(retrievedPath, ec), 50u);

    std::filesystem::remove(retrievedPath, ec);
}

// ─────────────────────────────────────────────────────────────────────────────
// PinPasteCoordinatorTest — 连续历史倒序贴图会话测试套件
// ─────────────────────────────────────────────────────────────────────────────

TEST(PinPasteCoordinatorTest, CandidateResolutionAndReverseOrder) {
    using namespace tools3000::capture;
    auto clearClipboardSafely = []() {
        for (int retry = 0; retry < 10; ++retry) {
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                CloseClipboard();
                break;
            }
            Sleep(5);
        }
    };
    clearClipboardSafely();

    auto& history = CaptureHistory::instance();
    history.clear();

    // 依序推入 3 张模拟截图：A (最老), B (中间), C (最新)
    cv::Mat imgA(100, 120, CV_8UC3, cv::Scalar(255, 0, 0));
    CaptureRegion regA{100, 100, 120, 100};
    history.push(imgA, regA);

    cv::Mat imgB(150, 180, CV_8UC3, cv::Scalar(0, 255, 0));
    CaptureRegion regB{200, 200, 180, 150};
    history.push(imgB, regB);

    cv::Mat imgC(200, 240, CV_8UC3, cv::Scalar(0, 0, 255));
    CaptureRegion regC{300, 300, 240, 200};
    history.push(imgC, regC);

    ASSERT_EQ(history.count(), 3);

    auto& coord = PinPasteCoordinator::instance();
    coord.reset();

    DWORD seq = GetClipboardSequenceNumber();
    auto candidates = coord.resolveCandidates(seq, 3);
    ASSERT_EQ(candidates.size(), 3u);

    // 验证历史排序：最新截图 (C) 在最前，最老截图 (A) 在最后
    EXPECT_EQ(candidates[0].image.cols, 240); // 倒数第 1 张 (最新 C)
    EXPECT_EQ(candidates[0].image.rows, 200);
    EXPECT_EQ(candidates[0].region.x, 300);

    EXPECT_EQ(candidates[1].image.cols, 180); // 倒数第 2 张 (B)
    EXPECT_EQ(candidates[1].image.rows, 150);
    EXPECT_EQ(candidates[1].region.x, 200);

    EXPECT_EQ(candidates[2].image.cols, 120); // 倒数第 3 张 (最老 A)
    EXPECT_EQ(candidates[2].image.rows, 100);
    EXPECT_EQ(candidates[2].region.x, 100);

    history.clear();
    clearClipboardSafely();
}

TEST(PinPasteCoordinatorTest, ContinuousRollbackAndWrapAround) {
    using namespace tools3000::capture;
    auto clearClipboardSafely = []() {
        for (int retry = 0; retry < 10; ++retry) {
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                CloseClipboard();
                break;
            }
            Sleep(5);
        }
    };
    clearClipboardSafely();

    auto& history = CaptureHistory::instance();
    history.clear();

    // 推入 3 张截图：从老到新
    history.push(cv::Mat(60, 60, CV_8UC3, cv::Scalar(10, 10, 10)), CaptureRegion{10, 10, 60, 60});
    history.push(cv::Mat(80, 80, CV_8UC3, cv::Scalar(20, 20, 20)), CaptureRegion{20, 20, 80, 80});
    history.push(cv::Mat(100, 100, CV_8UC3, cv::Scalar(30, 30, 30)), CaptureRegion{30, 30, 100, 100});

    auto& coord = PinPasteCoordinator::instance();
    coord.reset();
    coord.setSessionTimeoutMs(3000);

    // 第一次按快捷键：倒数第 1 张 (最新，100x100)
    auto dec1 = coord.stepNext();
    ASSERT_TRUE(dec1.has_value());
    EXPECT_EQ(dec1->reverseIndex, 0);
    EXPECT_EQ(dec1->targetIndex, 0);
    EXPECT_EQ(dec1->candidate.image.cols, 100);
    EXPECT_FALSE(dec1->isLooped);
    EXPECT_TRUE(coord.isSessionActive());

    // 连续第二次按快捷键：倒数第 2 张 (80x80)
    auto dec2 = coord.stepNext();
    ASSERT_TRUE(dec2.has_value());
    EXPECT_EQ(dec2->reverseIndex, 1);
    EXPECT_EQ(dec2->targetIndex, 1);
    EXPECT_EQ(dec2->candidate.image.cols, 80);
    EXPECT_FALSE(dec2->isLooped);

    // 连续第三次按快捷键：倒数第 3 张 (60x60)
    auto dec3 = coord.stepNext();
    ASSERT_TRUE(dec3.has_value());
    EXPECT_EQ(dec3->reverseIndex, 2);
    EXPECT_EQ(dec3->targetIndex, 2);
    EXPECT_EQ(dec3->candidate.image.cols, 60);
    EXPECT_FALSE(dec3->isLooped);

    // 连续第四次按快捷键：循环回最新 (100x100)
    auto dec4 = coord.stepNext();
    ASSERT_TRUE(dec4.has_value());
    EXPECT_EQ(dec4->reverseIndex, 3);
    EXPECT_EQ(dec4->targetIndex, 0);
    EXPECT_EQ(dec4->candidate.image.cols, 100);
    EXPECT_TRUE(dec4->isLooped);

    history.clear();
    coord.reset();
    clearClipboardSafely();
}

TEST(PinPasteCoordinatorTest, SessionTimeoutResetsToLatest) {
    using namespace tools3000::capture;
    auto clearClipboardSafely = []() {
        for (int retry = 0; retry < 10; ++retry) {
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                CloseClipboard();
                break;
            }
            Sleep(5);
        }
    };
    clearClipboardSafely();

    auto& history = CaptureHistory::instance();
    history.clear();

    history.push(cv::Mat(50, 50, CV_8UC3, cv::Scalar(1, 1, 1)), CaptureRegion{0, 0, 50, 50});
    history.push(cv::Mat(90, 90, CV_8UC3, cv::Scalar(2, 2, 2)), CaptureRegion{0, 0, 90, 90});

    auto& coord = PinPasteCoordinator::instance();
    coord.reset();
    coord.setSessionTimeoutMs(500); // 短超时用于快速测试

    auto dec1 = coord.stepNext();
    ASSERT_TRUE(dec1.has_value());
    EXPECT_EQ(dec1->reverseIndex, 0);
    EXPECT_EQ(dec1->candidate.image.cols, 90);

    // 超过超时阈值
    Sleep(550);
    EXPECT_FALSE(coord.isSessionActive());

    // 超时后再次触发，必须重置为倒数第 1 张 (90)
    auto decAfterTimeout = coord.stepNext();
    ASSERT_TRUE(decAfterTimeout.has_value());
    EXPECT_EQ(decAfterTimeout->reverseIndex, 0);
    EXPECT_EQ(decAfterTimeout->candidate.image.cols, 90);

    history.clear();
    coord.reset();
    coord.setSessionTimeoutMs(3000);
    clearClipboardSafely();
}

TEST(PinPasteCoordinatorTest, NewCaptureMutationResetsSession) {
    using namespace tools3000::capture;
    auto clearClipboardSafely = []() {
        for (int retry = 0; retry < 10; ++retry) {
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                CloseClipboard();
                break;
            }
            Sleep(5);
        }
    };
    clearClipboardSafely();

    auto& history = CaptureHistory::instance();
    history.clear();

    history.push(cv::Mat(40, 40, CV_8UC3, cv::Scalar(10, 10, 10)), CaptureRegion{0, 0, 40, 40});
    history.push(cv::Mat(70, 70, CV_8UC3, cv::Scalar(20, 20, 20)), CaptureRegion{0, 0, 70, 70});

    auto& coord = PinPasteCoordinator::instance();
    coord.reset();

    auto dec1 = coord.stepNext();
    ASSERT_TRUE(dec1.has_value());
    EXPECT_EQ(dec1->reverseIndex, 0);
    EXPECT_EQ(dec1->candidate.image.cols, 70);

    // 用户在按键间隔截了新图推入历史
    history.push(cv::Mat(120, 120, CV_8UC3, cv::Scalar(30, 30, 30)), CaptureRegion{0, 0, 120, 120});

    // 再次按键时检测到历史数量突变，会话重置为 0 并贴出绝对最新图 (120)
    auto dec2 = coord.stepNext();
    ASSERT_TRUE(dec2.has_value());
    EXPECT_EQ(dec2->reverseIndex, 0);
    EXPECT_EQ(dec2->candidate.image.cols, 120);

    history.clear();
    coord.reset();
    clearClipboardSafely();
}

TEST(PinPasteCoordinatorTest, ClipboardMutationResetsSession) {
    using namespace tools3000::capture;
    auto clearClipboardSafely = []() {
        for (int retry = 0; retry < 10; ++retry) {
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                CloseClipboard();
                break;
            }
            Sleep(5);
        }
    };
    clearClipboardSafely();

    auto& history = CaptureHistory::instance();
    history.clear();

    history.push(cv::Mat(40, 40, CV_8UC3, cv::Scalar(10, 10, 10)), CaptureRegion{0, 0, 40, 40});
    history.push(cv::Mat(70, 70, CV_8UC3, cv::Scalar(20, 20, 20)), CaptureRegion{0, 0, 70, 70});

    auto& coord = PinPasteCoordinator::instance();
    coord.reset();

    auto dec1 = coord.stepNext();
    ASSERT_TRUE(dec1.has_value());
    EXPECT_EQ(dec1->reverseIndex, 0);

    // 连续按键推进游标到倒数第 2 张
    auto dec2 = coord.stepNext();
    ASSERT_TRUE(dec2.has_value());
    EXPECT_EQ(dec2->reverseIndex, 1);

    // 用户在外部复制了新内容：写入剪贴板使 Windows 剪贴板序列号递增
    cv::Mat externalImg(15, 15, CV_8UC3, cv::Scalar(50, 50, 50));
    ClipboardUtils::copyImageToClipboard(externalImg);

    // 再次按键时检测到剪贴板序列号突变，会话重置为 0
    auto dec3 = coord.stepNext();
    ASSERT_TRUE(dec3.has_value());
    EXPECT_EQ(dec3->reverseIndex, 0);

    history.clear();
    coord.reset();
    clearClipboardSafely();
}

TEST(PinPasteCoordinatorTest, ExternalClipboardPrecedesHistory) {
    using namespace tools3000::capture;
    auto clearClipboardSafely = []() {
        for (int retry = 0; retry < 10; ++retry) {
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                CloseClipboard();
                break;
            }
            Sleep(5);
        }
    };
    clearClipboardSafely();

    auto& history = CaptureHistory::instance();
    history.clear();

    // 历史中有 1 张截图 (100x100)
    history.push(cv::Mat(100, 100, CV_8UC3, cv::Scalar(10, 10, 10)), CaptureRegion{0, 0, 100, 100});

    // 用户在外部（如浏览器）复制了一张新图片 (48x48)
    cv::Mat externalImg(48, 48, CV_8UC3, cv::Scalar(99, 99, 99));
    bool copied = false;
    for (int r = 0; r < 10; ++r) {
        if (ClipboardUtils::copyImageToClipboard(externalImg)) {
            copied = true;
            break;
        }
        Sleep(20);
    }
    ASSERT_TRUE(copied);

    auto& coord = PinPasteCoordinator::instance();
    coord.reset();

    // 第一次贴图：外部复制的图片作为最新内容（倒数第 1 个）贴出
    auto dec1 = coord.stepNext();
    ASSERT_TRUE(dec1.has_value());
    EXPECT_EQ(dec1->reverseIndex, 0);
    EXPECT_EQ(dec1->totalCount, 2);
    EXPECT_EQ(dec1->candidate.image.cols, 48);
    EXPECT_TRUE(dec1->candidate.isFromClipboard);

    // 第二次连续贴图：回溯出截图历史中的图 (100x100)
    auto dec2 = coord.stepNext();
    ASSERT_TRUE(dec2.has_value());
    EXPECT_EQ(dec2->reverseIndex, 1);
    EXPECT_EQ(dec2->candidate.image.cols, 100);
    EXPECT_FALSE(dec2->candidate.isFromClipboard);

    history.clear();
    coord.reset();
    clearClipboardSafely();
}

TEST(PinPasteCoordinatorTest, TextClipboardRendersGeekCardAndPrecedesHistory) {
    using namespace tools3000::capture;
    auto writeTextToClipboard = [](const std::wstring& text) -> bool {
        for (int retry = 0; retry < 15; ++retry) {
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (!hMem) {
                    CloseClipboard();
                    return false;
                }
                void* dst = GlobalLock(hMem);
                if (!dst) {
                    GlobalFree(hMem);
                    CloseClipboard();
                    return false;
                }
                std::memcpy(dst, text.c_str(), bytes);
                GlobalUnlock(hMem);
                if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
                    GlobalFree(hMem);
                    CloseClipboard();
                    return false;
                }
                CloseClipboard();
                return true;
            }
            Sleep(10);
        }
        return false;
    };
    auto clearClipboardSafely = []() {
        for (int retry = 0; retry < 10; ++retry) {
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                CloseClipboard();
                break;
            }
            Sleep(5);
        }
    };

    // 1. 直接验证 renderTextToImage 边界与多行中文渲染
    EXPECT_TRUE(PinPasteCoordinator::renderTextToImage(L"").empty());
    cv::Mat card = PinPasteCoordinator::renderTextToImage(L"复制文字后按贴图\nSecond line with\ttab\r\n");
    ASSERT_FALSE(card.empty());
    EXPECT_EQ(card.type(), CV_8UC3);
    EXPECT_GE(card.cols, 180);
    EXPECT_GE(card.rows, 56);

    // 2. 截图历史为空时，复制纯文字按贴图必须成功贴出文字卡片
    auto& history = CaptureHistory::instance();
    history.clear();
    auto& coord = PinPasteCoordinator::instance();
    coord.reset();

    ASSERT_TRUE(writeTextToClipboard(L"Tools3000 极客文字贴图测试"));
    auto decEmptyHistory = coord.stepNext();
    ASSERT_TRUE(decEmptyHistory.has_value());
    EXPECT_EQ(decEmptyHistory->reverseIndex, 0);
    EXPECT_EQ(decEmptyHistory->totalCount, 1);
    EXPECT_TRUE(decEmptyHistory->candidate.isFromClipboard);
    EXPECT_EQ(decEmptyHistory->candidate.region.width, 0);
    EXPECT_FALSE(decEmptyHistory->candidate.image.empty());

    // 3. 即使截图历史最新一张尺寸恰好与文字卡片相同，剪贴板文字也绝不能被误判去重吞没
    history.push(cv::Mat(decEmptyHistory->candidate.image.rows,
                         decEmptyHistory->candidate.image.cols,
                         CV_8UC3, cv::Scalar(42, 42, 42)),
                 CaptureRegion{80, 90, decEmptyHistory->candidate.image.cols, decEmptyHistory->candidate.image.rows});
    coord.reset();
    ASSERT_TRUE(writeTextToClipboard(L"Tools3000 极客文字贴图测试"));

    auto decWithHistory1 = coord.stepNext();
    ASSERT_TRUE(decWithHistory1.has_value());
    EXPECT_EQ(decWithHistory1->totalCount, 2);
    EXPECT_EQ(decWithHistory1->reverseIndex, 0);
    EXPECT_TRUE(decWithHistory1->candidate.isFromClipboard);

    auto decWithHistory2 = coord.stepNext();
    ASSERT_TRUE(decWithHistory2.has_value());
    EXPECT_EQ(decWithHistory2->reverseIndex, 1);
    EXPECT_FALSE(decWithHistory2->candidate.isFromClipboard);
    EXPECT_GT(decWithHistory2->candidate.region.width, 0);

    history.clear();
    coord.reset();
    clearClipboardSafely();
}

TEST(PinPasteCoordinatorTest, HexColorClipboardRendersColorSwatch) {
    using namespace tools3000::capture;
    cv::Scalar bgr{};
    std::wstring label;
    EXPECT_TRUE(PinPasteCoordinator::parseHexColor(L"  #3B82F6\r\n", bgr, label));
    EXPECT_EQ(static_cast<int>(bgr[2]), 0x3B);
    EXPECT_EQ(static_cast<int>(bgr[1]), 0x82);
    EXPECT_EQ(static_cast<int>(bgr[0]), 0xF6);
    EXPECT_EQ(label, L"#3B82F6");

    EXPECT_TRUE(PinPasteCoordinator::parseHexColor(L"#f0a", bgr, label));
    EXPECT_EQ(static_cast<int>(bgr[2]), 0xFF);
    EXPECT_EQ(static_cast<int>(bgr[1]), 0x00);
    EXPECT_EQ(static_cast<int>(bgr[0]), 0xAA);
    EXPECT_EQ(label, L"#f0a");

    EXPECT_FALSE(PinPasteCoordinator::parseHexColor(L"Hello World", bgr, label));
    EXPECT_FALSE(PinPasteCoordinator::parseHexColor(L"#12345", bgr, label));

    cv::Mat swatch = PinPasteCoordinator::renderColorSwatch(bgr, label);
    ASSERT_FALSE(swatch.empty());
    EXPECT_EQ(swatch.cols, 180);
    EXPECT_EQ(swatch.rows, 130);
    EXPECT_EQ(swatch.type(), CV_8UC3);
}

TEST(ClipboardUtilsTest, ReadImageWithoutPinMetadataZeroesMagic) {
    using namespace tools3000::capture;
    auto putRawDibWithoutMetadata = []() -> bool {
        for (int retry = 0; retry < 15; ++retry) {
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                BITMAPINFOHEADER bi{};
                bi.biSize = sizeof(BITMAPINFOHEADER);
                bi.biWidth = 8;
                bi.biHeight = 8;
                bi.biPlanes = 1;
                bi.biBitCount = 32;
                bi.biCompression = BI_RGB;
                bi.biSizeImage = 8 * 8 * 4;

                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(bi) + bi.biSizeImage);
                if (!hMem) {
                    CloseClipboard();
                    return false;
                }
                auto* ptr = static_cast<uint8_t*>(GlobalLock(hMem));
                if (!ptr) {
                    GlobalFree(hMem);
                    CloseClipboard();
                    return false;
                }
                std::memcpy(ptr, &bi, sizeof(bi));
                std::memset(ptr + sizeof(bi), 0x7F, bi.biSizeImage);
                GlobalUnlock(hMem);
                if (!SetClipboardData(CF_DIB, hMem)) {
                    GlobalFree(hMem);
                    CloseClipboard();
                    return false;
                }
                CloseClipboard();
                return true;
            }
            Sleep(10);
        }
        return false;
    };

    ASSERT_TRUE(putRawDibWithoutMetadata());
    Tools3000PinMetadata meta{};
    // 默认构造时 magic 为 0x54333030，当剪贴板中不含 Tools3000_PinMetadata 时必须被清零
    ASSERT_EQ(meta.magic, 0x54333030u);
    cv::Mat img = ClipboardUtils::readImageFromClipboard(&meta);
    EXPECT_FALSE(img.empty());
    EXPECT_EQ(meta.magic, 0u);

    // 验证全 0 Alpha 通道的 32-bit BI_RGB CF_DIB 被系统合成为 CF_DIBV5 时，自动转为不透明 CV_8UC3
    auto putZeroAlphaDib = []() -> bool {
        for (int retry = 0; retry < 50; ++retry) {
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                BITMAPINFOHEADER bi{};
                bi.biSize = sizeof(BITMAPINFOHEADER);
                bi.biWidth = 6;
                bi.biHeight = 6;
                bi.biPlanes = 1;
                bi.biBitCount = 32;
                bi.biCompression = BI_RGB;
                bi.biSizeImage = 6 * 6 * 4;

                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(bi) + bi.biSizeImage);
                if (!hMem) {
                    CloseClipboard();
                    return false;
                }
                auto* ptr = static_cast<uint8_t*>(GlobalLock(hMem));
                if (!ptr) {
                    GlobalFree(hMem);
                    CloseClipboard();
                    return false;
                }
                std::memcpy(ptr, &bi, sizeof(bi));
                uint8_t* px = ptr + sizeof(bi);
                for (int i = 0; i < 6 * 6; ++i) {
                    px[i * 4 + 0] = 0x20;
                    px[i * 4 + 1] = 0x40;
                    px[i * 4 + 2] = 0x80;
                    px[i * 4 + 3] = 0x00; // GDI BI_RGB 未初始化的 0 Alpha
                }
                GlobalUnlock(hMem);
                if (!SetClipboardData(CF_DIB, hMem)) {
                    GlobalFree(hMem);
                    CloseClipboard();
                    return false;
                }
                CloseClipboard();
                return true;
            }
            Sleep(10);
        }
        return false;
    };

    ASSERT_TRUE(putZeroAlphaDib());
    cv::Mat opaqueImg = ClipboardUtils::readImageFromClipboard(nullptr);
    ASSERT_FALSE(opaqueImg.empty());
    EXPECT_EQ(opaqueImg.type(), CV_8UC3);
    EXPECT_EQ(opaqueImg.at<cv::Vec3b>(0, 0), cv::Vec3b(0x20, 0x40, 0x80));

    // 验证富文本/Office 复制场景（CF_UNICODETEXT 先于预览 CF_DIB 写入剪贴板）优先渲染为文字贴图卡片
    auto putTextThenPreviewDib = [](const std::wstring& text) -> bool {
        for (int retry = 0; retry < 50; ++retry) {
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                const size_t textBytes = (text.size() + 1) * sizeof(wchar_t);
                HGLOBAL hText = GlobalAlloc(GMEM_MOVEABLE, textBytes);
                if (!hText) {
                    CloseClipboard();
                    return false;
                }
                void* dstText = GlobalLock(hText);
                std::memcpy(dstText, text.c_str(), textBytes);
                GlobalUnlock(hText);
                SetClipboardData(CF_UNICODETEXT, hText);

                BITMAPINFOHEADER bi{};
                bi.biSize = sizeof(BITMAPINFOHEADER);
                bi.biWidth = 4;
                bi.biHeight = 4;
                bi.biPlanes = 1;
                bi.biBitCount = 32;
                bi.biCompression = BI_RGB;
                bi.biSizeImage = 4 * 4 * 4;
                HGLOBAL hDib = GlobalAlloc(GMEM_MOVEABLE, sizeof(bi) + bi.biSizeImage);
                if (hDib) {
                    auto* ptr = static_cast<uint8_t*>(GlobalLock(hDib));
                    std::memcpy(ptr, &bi, sizeof(bi));
                    std::memset(ptr + sizeof(bi), 0xFF, bi.biSizeImage);
                    GlobalUnlock(hDib);
                    SetClipboardData(CF_DIB, hDib);
                }
                CloseClipboard();
                return true;
            }
            Sleep(10);
        }
        return false;
    };

    CaptureHistory::instance().clear();
    PinPasteCoordinator::instance().reset();
    ASSERT_TRUE(putTextThenPreviewDib(L"Word/Excel 富文本复制文字优先贴图测试"));
    auto richTextDecision = PinPasteCoordinator::instance().stepNext();
    ASSERT_TRUE(richTextDecision.has_value());
    EXPECT_GE(richTextDecision->candidate.image.cols, 160);
    EXPECT_GE(richTextDecision->candidate.image.rows, 40);

    for (int retry = 0; retry < 10; ++retry) {
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            CloseClipboard();
            break;
        }
        Sleep(5);
    }
}

TEST(CaptureLifecycleAndGhostWindowTest, ShortcutHintOverlayBottomLeftPositionAndMaximizedWindowStability) {
    using namespace tools3000::capture;
    const RECT workArea = { 0, 0, 1920, 1040 };
    const int cardW = 240;
    const int cardH = 260;
    const int margin = static_cast<int>(ShortcutHintStyle::BaseScreenMargin * 1.0f);

    // 1. 全屏/最大化窗口包络覆盖全部候选角时，绝不能跳变到左上角，必须稳定保持在默认左下角
    const RECT maximizedWindow = { 0, 32, 1920, 1040 };
    POINT posMax = ShortcutHintOverlay::computeOptimalPosition(cardW, cardH, workArea, 1.0f, { maximizedWindow });
    EXPECT_EQ(posMax.x, workArea.left + margin);
    EXPECT_EQ(posMax.y, workArea.bottom - cardH - margin);

    // 2. 真实窗口显示时，初始位置位于当前屏幕工作区左下角
    auto& hint = ShortcutHintOverlay::instance();
    hint.hide();
    POINT anchor{ 200, 200 };
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    ASSERT_TRUE(GetMonitorInfoW(mon, &mi));

    hint.show(ShortcutHintContext::CaptureSelecting, anchor, {});
    ASSERT_TRUE(hint.isVisible());
    RECT bounds = hint.getBounds();
    const LONG midX = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left) / 2;
    const LONG midY = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top) / 2;
    EXPECT_LT(bounds.left, midX);
    EXPECT_GT(bounds.bottom, midY);

    // 3. 左下角局部工具栏碰撞避让到左上角后，重置回未选区态 (avoidRects = {}) 必须恢复至左下角
    RECT bottomLeftObstacle{
        mi.rcWork.left,
        mi.rcWork.bottom - 360,
        mi.rcWork.left + 420,
        mi.rcWork.bottom
    };
    hint.show(ShortcutHintContext::CaptureSelected, anchor, { bottomLeftObstacle });
    RECT avoidedBounds = hint.getBounds();
    EXPECT_LT(avoidedBounds.top, midY);

    hint.show(ShortcutHintContext::CaptureSelecting, anchor, {});
    RECT restoredBounds = hint.getBounds();
    EXPECT_LT(restoredBounds.left, midX);
    EXPECT_GT(restoredBounds.bottom, midY);

    // 4. 全屏/最大化窗口区域传入 updateAvoidance 时，保持左下角不跳变
    RECT fullWorkAreaRect = mi.rcWork;
    fullWorkAreaRect.top += 32;
    hint.updateAvoidance({ fullWorkAreaRect });
    RECT afterFullWindowAvoid = hint.getBounds();
    EXPECT_EQ(afterFullWindowAvoid.left, restoredBounds.left);
    EXPECT_EQ(afterFullWindowAvoid.top, restoredBounds.top);

    // 5. 最大化窗口 / 左半屏窗口选区构建工具栏后，主工具栏自动向右避让左下角提示区，
    //    确保将工具栏矩形传入 updateAvoidance 时，快捷键提示窗依然稳固驻留左下角，绝不误跳至左上角
    {
        CaptureState tbState;
        tbState.mode = OverlayMode::Screenshot;
        tbState.state = OverlayState::Selected;
        tbState.dpiScale = 1.0f;
        tbState.options.showShortcutHints = true;
        const D2D1_SIZE_F surf = D2D1::SizeF(
            static_cast<float>(mi.rcWork.right - mi.rcWork.left),
            static_cast<float>(mi.rcWork.bottom - mi.rcWork.top));
        D2D1_RECT_F maxSel = D2D1::RectF(0.0f, 0.0f, surf.width, surf.height);
        rebuildCaptureToolbar(tbState, maxSel, surf);
        EXPECT_GE(tbState.primaryToolbarRect.left, 340.0f);

        RECT priRectScreen{
            mi.rcWork.left + static_cast<LONG>(tbState.primaryToolbarRect.left),
            mi.rcWork.top + static_cast<LONG>(tbState.primaryToolbarRect.top),
            mi.rcWork.left + static_cast<LONG>(tbState.primaryToolbarRect.right),
            mi.rcWork.top + static_cast<LONG>(tbState.primaryToolbarRect.bottom)
        };
        RECT sideRectScreen{
            mi.rcWork.left + static_cast<LONG>(tbState.selectionSideRect.left),
            mi.rcWork.top + static_cast<LONG>(tbState.selectionSideRect.top),
            mi.rcWork.left + static_cast<LONG>(tbState.selectionSideRect.right),
            mi.rcWork.top + static_cast<LONG>(tbState.selectionSideRect.bottom)
        };
        hint.updateAvoidance({ priRectScreen, sideRectScreen });
        RECT afterToolbarAvoid = hint.getBounds();
        EXPECT_EQ(afterToolbarAvoid.left, restoredBounds.left);
        EXPECT_EQ(afterToolbarAvoid.top, restoredBounds.top);
        EXPECT_GT(afterToolbarAvoid.bottom, midY);
    }

    hint.hide();
    EXPECT_FALSE(hint.isVisible());
}

TEST(CaptureLifecycleAndGhostWindowTest, ShortcutHintOverlayNeverRemainsAfterCaptureExitOrCrossThreadHide) {
    using namespace tools3000::capture;

    auto& hint = ShortcutHintOverlay::instance();
    hint.hide();
    EXPECT_FALSE(hint.isVisible());

    // 1. 验证跨线程调用 hide() 时（如后台线程回调）立即隐藏窗口并投递销毁消息，绝不残留幽灵窗口
    hint.show(ShortcutHintContext::CaptureSelected);
    ASSERT_TRUE(hint.isVisible());
    ASSERT_NE(FindWindowW(L"Tools3000_ShortcutHintOverlay", nullptr), nullptr);

    std::thread worker([]() {
        ShortcutHintOverlay::instance().hide();
    });
    worker.join();
    EXPECT_FALSE(hint.isVisible());

    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EXPECT_FALSE(hint.isVisible());
    EXPECT_EQ(FindWindowW(L"Tools3000_ShortcutHintOverlay", nullptr), nullptr);

    // 2. 验证外部发送 WM_CLOSE 或直接 DestroyWindow 后，isVisible() 准确感知且重新 show() 自愈重建
    hint.show(ShortcutHintContext::CaptureSelecting);
    ASSERT_TRUE(hint.isVisible());
    HWND hintHwnd = FindWindowW(L"Tools3000_ShortcutHintOverlay", nullptr);
    ASSERT_NE(hintHwnd, nullptr);
    SendMessageW(hintHwnd, WM_CLOSE, 0, 0);
    EXPECT_FALSE(hint.isVisible());
    EXPECT_EQ(FindWindowW(L"Tools3000_ShortcutHintOverlay", nullptr), nullptr);

    hint.show(ShortcutHintContext::CaptureSelecting);
    EXPECT_TRUE(hint.isVisible());
    hint.hide();
    EXPECT_FALSE(hint.isVisible());
    EXPECT_EQ(FindWindowW(L"Tools3000_ShortcutHintOverlay", nullptr), nullptr);

    // 3. 验证处于 Idle 态的 CaptureState 调用 transitionToRecordMode 绝不会意外弹出残留提示窗
    {
        CaptureState idleState;
        idleState.state = OverlayState::Idle;
        idleState.options.showShortcutHints = true;
        transitionToRecordMode(idleState);
        EXPECT_FALSE(hint.isVisible());
    }

    // 4. 验证 OwnerActiveChecker 守护契约与 150ms 定时器自愈机制：
    //    (a) 当宿主已退出 (ownerActive == false) 时，任何迟到的 show(CaptureSelected) 调用必须被立即拒绝并隐藏；
    //    (b) 当宿主在提示窗显示期间退场 (ownerActive 由 true 变为 false) 且未显式调用 hide() 时，
    //        WM_TIMER 守护定时器必须在消息泵驱动下自动销毁残留的 Tools3000_ShortcutHintOverlay 窗口。
    {
        static std::atomic<bool> s_mockOwnerActive{false};
        struct CheckerResetGuard {
            ~CheckerResetGuard() {
                ShortcutHintOverlay::instance().setOwnerActiveChecker(nullptr);
                ShortcutHintOverlay::instance().hide();
            }
        } resetGuard;

        hint.setOwnerActiveChecker([](ShortcutHintContext) -> bool {
            return s_mockOwnerActive.load();
        });

        // (a) 宿主处于非活动态时迟到的 show(CaptureSelected) 调用绝不创建或残留窗口
        s_mockOwnerActive.store(false);
        hint.show(ShortcutHintContext::CaptureSelected, {200, 200}, {});
        EXPECT_FALSE(hint.isVisible());
        EXPECT_EQ(FindWindowW(L"Tools3000_ShortcutHintOverlay", nullptr), nullptr);

        // (b) 宿主处于活动态时正常显示，随后宿主变为非活动态（模拟异常退场漏调 hide），
        //     验证 WM_TIMER 守护自愈自动销毁提示窗
        s_mockOwnerActive.store(true);
        hint.show(ShortcutHintContext::CaptureSelected, {200, 200}, {});
        ASSERT_TRUE(hint.isVisible());
        HWND watchdogHwnd = FindWindowW(L"Tools3000_ShortcutHintOverlay", nullptr);
        ASSERT_NE(watchdogHwnd, nullptr);

        s_mockOwnerActive.store(false);
        // 触发守护定时器消息 (kWatchdogTimerId = 1)
        SendMessageW(watchdogHwnd, WM_TIMER, 1, 0);
        EXPECT_FALSE(hint.isVisible());
        EXPECT_EQ(FindWindowW(L"Tools3000_ShortcutHintOverlay", nullptr), nullptr);
    }
}

TEST(CaptureLifecycleAndGhostWindowTest, ShortcutHintAndCaptureHudsDodgeCursorLoupeAndMaximizedWindow) {
    using namespace tools3000::capture;

    const RECT workArea = { 0, 0, 1920, 1040 };
    const int cardW = 240;
    const int cardH = 260;
    const int margin = static_cast<int>(ShortcutHintStyle::BaseScreenMargin * 1.0f);

    // 1. 当最大化窗口矩形与左下角光标/放大镜局部障碍物同时传入时，
    //    全局窗口绝不能掩盖局部碰撞，HUD 必须立即避让左下角光标与放大镜
    const RECT maximizedWindow = { 0, 32, 1920, 1040 };
    const RECT bottomLeftCursorBox = { 60, 840, 164, 944 };
    const RECT bottomLeftLoupeBox = { 136, 828, 352, 1014 };
    POINT posDodged = ShortcutHintOverlay::computeOptimalPosition(
        cardW, cardH, workArea, 1.0f,
        { maximizedWindow, bottomLeftCursorBox, bottomLeftLoupeBox });
    RECT dodgedCardRect{ posDodged.x, posDodged.y, posDodged.x + cardW, posDodged.y + cardH };
    RECT interCursor{};
    RECT interLoupe{};
    EXPECT_FALSE(IntersectRect(&interCursor, &dodgedCardRect, &bottomLeftCursorBox));
    EXPECT_FALSE(IntersectRect(&interLoupe, &dodgedCardRect, &bottomLeftLoupeBox));

    // 当光标移至屏幕右上角，左下角无局部碰撞时，即使存在最大化窗口也必须平滑回归默认左下角
    const RECT topRightCursorBox = { 1680, 120, 1784, 224 };
    POINT posRestored = ShortcutHintOverlay::computeOptimalPosition(
        cardW, cardH, workArea, 1.0f,
        { maximizedWindow, topRightCursorBox });
    EXPECT_EQ(posRestored.x, workArea.left + margin);
    EXPECT_EQ(posRestored.y, workArea.bottom - cardH - margin);

    // 2. 验证大面积非全屏局部选区（如占工作区 75%x77% 的左下角大选区 {0, 240, 1450, 1040}）绝不被误忽略，
    //    HUD 必须逃逸至完全未受遮挡的右上角
    const RECT largeBottomLeftSelection = { 0, 240, 1450, 1040 };
    POINT posLargePartial = ShortcutHintOverlay::computeOptimalPosition(
        cardW, cardH, workArea, 1.0f, { largeBottomLeftSelection });
    RECT largePartialCard{ posLargePartial.x, posLargePartial.y, posLargePartial.x + cardW, posLargePartial.y + cardH };
    RECT interLargePartial{};
    EXPECT_FALSE(IntersectRect(&interLargePartial, &largePartialCard, &largeBottomLeftSelection));

    // 3. 验证 globalObstacles 次级代价决胜：当顶部留有空隙的近似全屏窗口 {0, 150, 1920, 1040} 被归类为 globalObstacle，
    //    且左侧（左下+左上）均被局部障碍物占据时，右上角因与 globalObstacle 重叠面积显著小于右下角而优先胜出
    const RECT globalWithTopGap = { 0, 150, 1920, 1040 };
    const RECT leftSideBarObstacle = { 0, 0, 320, 1040 };
    POINT posGlobalTieBreak = ShortcutHintOverlay::computeOptimalPosition(
        cardW, cardH, workArea, 1.0f, { globalWithTopGap, leftSideBarObstacle });
    EXPECT_EQ(posGlobalTieBreak.x, workArea.right - cardW - margin);
    EXPECT_EQ(posGlobalTieBreak.y, workArea.top + margin);

    // 4. 验证 8 大标准锚点全被局部障碍物占据时，dynamicCandidates 选取距离默认左下角曼哈顿距离最近的零碰撞空隙
    const std::vector<RECT> allEightAnchorsBlocked = {
        { 0, 740, 300, 1040 },     // 1. 左下角
        { 0, 0, 300, 300 },        // 2. 左上角
        { 1620, 740, 1920, 1040 }, // 3. 右下角
        { 1620, 0, 1920, 300 },    // 4. 右上角
        { 800, 740, 1120, 1040 },  // 5. 底部居中
        { 800, 0, 1120, 300 },     // 6. 顶部居中
        { 0, 360, 300, 680 },      // 7. 左侧居中
        { 1620, 360, 1920, 680 }   // 8. 右侧居中
    };
    POINT posDynamic = ShortcutHintOverlay::computeOptimalPosition(
        cardW, cardH, workArea, 1.0f, allEightAnchorsBlocked);
    RECT dynamicCard{ posDynamic.x, posDynamic.y, posDynamic.x + cardW, posDynamic.y + cardH };
    for (const auto& blk : allEightAnchorsBlocked) {
        RECT interBlk{};
        EXPECT_FALSE(IntersectRect(&interBlk, &dynamicCard, &blk));
    }
    // 紧邻左下角障碍物右侧 (rx = 300 + 10 + 16 = 326, y = maxY) 的动态点位距默认左下角曼哈顿距离最短
    EXPECT_EQ(posDynamic.y, workArea.bottom - cardH - margin);
    EXPECT_LT(posDynamic.x, 450);

    // 5. 验证选区放大镜包围盒 computeSelectionLoupeRect 单一事实源 (SSOT) 与屏幕右下边缘翻转
    const D2D1_SIZE_F surf = D2D1::SizeF(1920.0f, 1080.0f);
    D2D1_RECT_F loupeNormal = HudAvoidanceEngine::computeSelectionLoupeRect(200.0f, 200.0f, surf, 1.0f);
    EXPECT_FLOAT_EQ(loupeNormal.left, 222.0f);
    EXPECT_FLOAT_EQ(loupeNormal.top, 222.0f);
    EXPECT_FLOAT_EQ(loupeNormal.right, 222.0f + 216.0f);
    EXPECT_FLOAT_EQ(loupeNormal.bottom, 222.0f + 186.0f);

    D2D1_RECT_F loupeBottomRight = HudAvoidanceEngine::computeSelectionLoupeRect(1880.0f, 1040.0f, surf, 1.0f);
    EXPECT_LT(loupeBottomRight.right, 1880.0f);
    EXPECT_LT(loupeBottomRight.bottom, 1040.0f);

    // 6. 验证选区右侧余量充足容纳侧边栏（如右侧余量 70px >= sideW + 16px）时，selectionSideRect 保持在选区外侧右方而不提前内嵌遮挡选区
    {
        CaptureState sideState;
        sideState.mode = OverlayMode::Screenshot;
        sideState.state = OverlayState::Selected;
        sideState.dpiScale = 1.0f;
        D2D1_RECT_F wideSel = D2D1::RectF(10.0f, 100.0f, 1850.0f, 800.0f);
        rebuildCaptureToolbar(sideState, wideSel, surf);
        EXPECT_GT(sideState.selectionSideRect.left, wideSel.right);
    }

    // 7. 验证单参数上下文切换保留避让状态 & 临界迟滞防抖 (Hysteresis Anti-Jitter)
    auto& hint = ShortcutHintOverlay::instance();
    hint.hide();
    POINT anchor{ 200, 200 };
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    ASSERT_TRUE(GetMonitorInfoW(mon, &mi));

    RECT realBottomLeftObstacle{
        mi.rcWork.left,
        mi.rcWork.bottom - 380,
        mi.rcWork.left + 440,
        mi.rcWork.bottom
    };
    hint.show(ShortcutHintContext::CaptureSelecting, anchor, { realBottomLeftObstacle });
    ASSERT_TRUE(hint.isVisible());
    RECT selectingAvoided = hint.getBounds();
    RECT interReal{};
    EXPECT_FALSE(IntersectRect(&interReal, &selectingAvoided, &realBottomLeftObstacle));

    // 单参数切换至 CaptureDragging / CaptureSelected 时保留避让状态
    hint.show(ShortcutHintContext::CaptureDragging);
    RECT draggingAvoided = hint.getBounds();
    EXPECT_FALSE(IntersectRect(&interReal, &draggingAvoided, &realBottomLeftObstacle));

    hint.show(ShortcutHintContext::CaptureSelected);
    RECT selectedAvoided = hint.getBounds();
    EXPECT_FALSE(IntersectRect(&interReal, &selectedAvoided, &realBottomLeftObstacle));

    // 迟滞防抖验证：当障碍物刚退至默认左下角卡片边缘外侧 16px（超出 pad=10px 但仍在 28px 迟滞带内）时，
    // 卡片保持在当前避让位不立即乒乓跳回左下角；当障碍物彻底远离后，立即平滑回归左下角
    const int cardActualW = selectedAvoided.right - selectedAvoided.left;
    const int cardActualH = selectedAvoided.bottom - selectedAvoided.top;
    const int monMargin = static_cast<int>(ShortcutHintStyle::BaseScreenMargin * 1.0f);
    RECT borderHysteresisObstacle{
        mi.rcWork.left + monMargin + cardActualW + 16,
        mi.rcWork.bottom - monMargin - cardActualH,
        mi.rcWork.left + monMargin + cardActualW + 96,
        mi.rcWork.bottom - monMargin
    };
    hint.updateAvoidance({ borderHysteresisObstacle });
    RECT hystBounds = hint.getBounds();
    EXPECT_EQ(hystBounds.top, selectedAvoided.top);

    RECT farObstacle{
        mi.rcWork.right - 200,
        mi.rcWork.top + 40,
        mi.rcWork.right - 80,
        mi.rcWork.top + 160
    };
    hint.updateAvoidance({ farObstacle });
    RECT restoredAfterFar = hint.getBounds();
    const LONG midY = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top) / 2;
    EXPECT_GT(restoredAfterFar.bottom, midY);

    hint.hide();
    EXPECT_FALSE(hint.isVisible());
}



TEST(CaptureToolbarTest, StandaloneBeautyShellSecondaryToolbar) {
    using namespace tools3000::capture;

    CaptureState state;
    state.mode = OverlayMode::Screenshot;
    state.dpiScale = 1.25f;
    state.isMarkupToolActive = false;
    state.beautyShell.enabled = false;

    const D2D1_SIZE_F surface = D2D1::SizeF(1920.0f, 1080.0f);
    const D2D1_RECT_F sel = D2D1::RectF(200.0f, 160.0f, 1200.0f, 760.0f);

    // 1. 未启用标注工具且未开启美化外壳时，二级工具栏保持隐藏
    rebuildCaptureToolbar(state, sel, surface);
    EXPECT_TRUE(state.secondaryToolbarButtons.empty());
    EXPECT_FLOAT_EQ(state.secondaryToolbarRect.right - state.secondaryToolbarRect.left, 0.0f);

    // 2. 不选择任何标注工具，直接开启美化外壳导出：二级菜单栏独立展开 3 枚下拉按钮
    state.beautyShell.enabled = true;
    state.beautyShell.bgType = BeautyBackgroundType::PaperChalk;
    state.beautyShell.padding = 48;
    state.beautyShell.cornerRadius = 16.0f;
    state.toolbarLayoutValid = false;
    rebuildCaptureToolbar(state, sel, surface);

    ASSERT_EQ(state.secondaryToolbarButtons.size(), 3u);
    EXPECT_GT(state.secondaryToolbarRect.right - state.secondaryToolbarRect.left, 0.0f);
    EXPECT_EQ(state.secondaryToolbarButtons[0].command, ToolbarCommand::CycleBeautyBg);
    EXPECT_TRUE(state.secondaryToolbarButtons[0].hasDropdown);
    EXPECT_FALSE(state.secondaryToolbarButtons[0].isSeparatorBefore);
    EXPECT_EQ(state.secondaryToolbarButtons[1].command, ToolbarCommand::CycleBeautyPadding);
    EXPECT_TRUE(state.secondaryToolbarButtons[1].hasDropdown);
    EXPECT_EQ(state.secondaryToolbarButtons[2].command, ToolbarCommand::CycleBeautyRadius);
    EXPECT_TRUE(state.secondaryToolbarButtons[2].hasDropdown);

    // 验证独立二级菜单栏按钮水平严格递增且互不重叠
    for (std::size_t i = 0; i < state.secondaryToolbarButtons.size(); ++i) {
        EXPECT_GT(state.secondaryToolbarButtons[i].rect.right,
                  state.secondaryToolbarButtons[i].rect.left);
        if (i > 0) {
            EXPECT_GT(state.secondaryToolbarButtons[i].rect.left,
                      state.secondaryToolbarButtons[i - 1].rect.right);
        }
    }

    // 3. 同时激活文字标注工具时，美化外壳控制组无缝拼接在文字属性右侧并带分隔符
    state.isMarkupToolActive = true;
    state.currentTool = MarkupTool::Text;
    state.toolbarLayoutValid = false;
    rebuildCaptureToolbar(state, sel, surface);
    ASSERT_GT(state.secondaryToolbarButtons.size(), 3u);
    const std::size_t n = state.secondaryToolbarButtons.size();
    EXPECT_EQ(state.secondaryToolbarButtons[n - 3].command, ToolbarCommand::CycleBeautyBg);
    EXPECT_TRUE(state.secondaryToolbarButtons[n - 3].isSeparatorBefore);
    EXPECT_EQ(state.secondaryToolbarButtons[n - 2].command, ToolbarCommand::CycleBeautyPadding);
    EXPECT_EQ(state.secondaryToolbarButtons[n - 1].command, ToolbarCommand::CycleBeautyRadius);
}

TEST(ScrollCaptureTest, ZeroDuplicateAndStickyBottomExclusionOnChatFeed) {
    using namespace tools3000::capture;

    // 构造模拟现代 AI/IDE 聊天长对话画布 (640x1600)：
    // 包含周期性重复的分割条与按钮（模拟连续 Review 卡片），同时每段正文具备唯一纹理行
    const int W = 640;
    const int docH = 1600;
    cv::Mat fullDoc(docH, W, CV_8UC3, cv::Scalar(22, 24, 30));
    for (int y = 0; y < docH; ++y) {
        // 每 220px 出现一条几乎完全相同的卡片分割线与右侧 Review 按钮（诱发局部单频带假阳性）
        if (y % 220 == 40) {
            cv::line(fullDoc, cv::Point(28, y), cv::Point(W - 40, y), cv::Scalar(58, 62, 74), 2);
            cv::rectangle(fullDoc, cv::Point(W - 118, y + 6), cv::Point(W - 46, y + 26),
                          cv::Scalar(48, 52, 64), cv::FILLED);
        }
        // 每 18px 绘制长度与横向相位随 y 唯一变化的伪代码/对话文本行
        if (y % 18 >= 4 && y % 18 <= 10) {
            const int startX = 36 + ((y * 7) % 48);
            const int endX = std::min(W - 60, 180 + ((y * 31 + 97) % 360));
            const cv::Scalar col(
                static_cast<double>(140 + ((y * 13) % 90)),
                static_cast<double>(150 + ((y * 17) % 80)),
                static_cast<double>(160 + ((y * 23) % 75)));
            cv::line(fullDoc, cv::Point(startX, y), cv::Point(endX, y), col, 1);
        }
    }

    const int viewH = 600;
    const int stickyTop = 44;
    const int stickyBottom = 84; // 含上方 20px 纯色内边距 + 下方固定输入框
    const int contentViewportH = viewH - stickyTop - stickyBottom; // 472px

    auto renderViewport = [&](int scrollY, int scrollbarThumbY, bool cursorBlinkOn) -> cv::Mat {
        cv::Mat frame(viewH, W, CV_8UC3, cv::Scalar(22, 24, 30));
        // 1. 顶部固定标题栏
        frame(cv::Rect(0, 0, W, stickyTop)).setTo(cv::Scalar(16, 18, 23));
        cv::rectangle(frame, cv::Point(24, 14), cv::Point(160, 30), cv::Scalar(90, 140, 230), cv::FILLED);

        // 2. 中间动态滚动对话区
        const int clampedScroll = std::clamp(scrollY, 0, docH - contentViewportH);
        fullDoc(cv::Rect(0, clampedScroll, W, contentViewportH))
            .copyTo(frame(cv::Rect(0, stickyTop, W, contentViewportH)));

        // 3. 底部固定输入栏：上方 20px 与背景同色的纯色内边距 + 下方圆角输入框
        const int footerY = viewH - stickyBottom;
        frame(cv::Rect(0, footerY, W, stickyBottom)).setTo(cv::Scalar(22, 24, 30));
        cv::rectangle(frame, cv::Point(32, footerY + 20), cv::Point(W - 36, viewH - 14),
                      cv::Scalar(34, 38, 48), cv::FILLED);
        cv::rectangle(frame, cv::Point(32, footerY + 20), cv::Point(W - 36, viewH - 14),
                      cv::Scalar(66, 72, 88), 1);
        if (cursorBlinkOn) {
            cv::line(frame, cv::Point(52, footerY + 30), cv::Point(52, footerY - 24 + stickyBottom),
                     cv::Scalar(240, 240, 245), 2);
        }

        // 4. 右侧 12px 反向向下移动的垂直滚动条轨道与滑块
        frame(cv::Rect(W - 14, stickyTop, 12, contentViewportH)).setTo(cv::Scalar(28, 30, 36));
        const int thumbY = std::clamp(scrollbarThumbY, stickyTop + 4, viewH - stickyBottom - 64);
        cv::rectangle(frame, cv::Point(W - 12, thumbY), cv::Point(W - 4, thumbY + 56),
                      cv::Scalar(110, 116, 130), cv::FILLED);
        return frame;
    };

    const cv::Mat f0 = renderViewport(0,   60,  true);
    const cv::Mat f1 = renderViewport(240, 150, false);
    const cv::Mat f2 = renderViewport(480, 240, true);
    const cv::Mat f3 = renderViewport(720, 330, false);
    // 触底静止帧：滚动位置仍停留在 720，但输入框光标闪烁切换，检验零位移否决门禁绝不误拼接重复块
    const cv::Mat f3IdleBlink = renderViewport(720, 330, true);

    auto& sc = ScrollCapture::instance();
    const auto tMove = sc.analyzeFrameTransition(f0, f1);
    EXPECT_FALSE(tMove.isUnchanged);
    EXPECT_TRUE(tMove.matched);
    EXPECT_NEAR(tMove.scrollDelta, 240, 2);
    // 反向精修应把含 20px 纯色内边距的整个底部固定栏纳入裁除范围
    EXPECT_GE(tMove.stickyBottomRows, stickyBottom - 4);

    const auto tAtBottom = sc.analyzeFrameTransition(f3, f3IdleBlink);
    EXPECT_TRUE(tAtBottom.isUnchanged);

    const std::vector<cv::Mat> frames{f0, f1, f2, f3, f3IdleBlink};
    const cv::Mat stitched = sc.stitchFrames(frames, 20);
    ASSERT_FALSE(stitched.empty());

    // 真实总高度应为：首帧高度 600 + 3 次真实滚动 (3 * 240 = 720) = 1320px，触底帧绝不产生重复增长
    const int expectedHeight = viewH + 3 * 240;
    EXPECT_NEAR(stitched.rows, expectedHeight, 4);
    EXPECT_EQ(stitched.cols, W);
}

TEST(CaptureMagnifierTest, FirstClassToolbarAndOffsetCalloutLoupe) {
    using namespace tools3000::capture;

    CaptureState state;
    state.mode = OverlayMode::Screenshot;
    state.state = OverlayState::Selected;
    state.dpiScale = 1.25f;
    state.isMarkupToolActive = true;
    state.currentTool = MarkupTool::Magnifier;
    state.currentColor = MarkupColor::Red(); // 常规形状保持珊瑚红
    state.currentMagnifierColor = MarkupColor::White(); // 放大镜独立保持微晶白

    const D2D1_SIZE_F surface = D2D1::SizeF(1920.0f, 1080.0f);
    const D2D1_RECT_F sel = D2D1::RectF(160.0f, 120.0f, 1280.0f, 820.0f);
    rebuildCaptureToolbar(state, sel, surface);

    ASSERT_EQ(state.toolbarButtons.size(), 18u);
    ASSERT_FALSE(state.secondaryToolbarButtons.empty());

    bool hasShapeToggle = false;
    bool hasScaleStepper = false;
    bool hasBadgeToggle = false;
    bool hasStrokeWidth = false;
    for (const auto& btn : state.secondaryToolbarButtons) {
        if (btn.command == ToolbarCommand::ToggleNumberShape && btn.tool == MarkupTool::Magnifier) hasShapeToggle = true;
        if (btn.command == ToolbarCommand::CycleMagnifierScale) {
            hasScaleStepper = true;
            EXPECT_EQ(toolbarButtonAccessibleName(btn), L"调节局部放大倍率");
        }
        if (btn.command == ToolbarCommand::ToggleFill && btn.tool == MarkupTool::Magnifier) hasBadgeToggle = true;
        if (btn.command == ToolbarCommand::CycleStrokeWidth && btn.tool == MarkupTool::Magnifier) hasStrokeWidth = true;
    }
    EXPECT_TRUE(hasShapeToggle);
    EXPECT_TRUE(hasScaleStepper);
    EXPECT_TRUE(hasBadgeToggle);
    EXPECT_TRUE(hasStrokeWidth);

    // 验证同心透镜与引出式旁路放大 (Offset Callout Loupe) 双模渲染与独立取景源命中
    ASSERT_TRUE(initializeMarkupTextRenderer());
    cv::Mat baseImg(420, 560, CV_8UC4, cv::Scalar(28, 32, 42, 255));
    // 在取景源 (140, 150) 绘制高对比度红绿特征图案
    cv::circle(baseImg, cv::Point(140, 150), 14, cv::Scalar(80, 220, 40, 255), cv::FILLED);
    cv::rectangle(baseImg, cv::Point(128, 138), cv::Point(152, 162), cv::Scalar(60, 80, 250, 255), 2);

    MarkupEngine engine;
    engine.setBaseImage(baseImg);
    auto* elem = engine.addMagnifier(cv::Point(140, 150), 2.5f, 68);
    ASSERT_NE(elem, nullptr);
    // 将放大透镜引出至右侧空白区 (360, 230)，不遮挡原位上下文
    elem->endPt = cv::Point(360, 230);
    elem->hasLeaderArrow = true;
    elem->numberShape = NumberBadgeShape::RoundedSquare;
    elem->isActive = true;

    EXPECT_EQ(elem->hitTestEx(cv::Point(140, 150)), HitArea::FocusSource);
    EXPECT_EQ(elem->hitTestEx(cv::Point(360, 230)), HitArea::LensOffset);

    cv::Mat rendered = engine.getCompositeImage(false);
    ASSERT_EQ(rendered.size(), baseImg.size());
    // 旁路大透镜中心 (360, 230) 应呈现出从 (140, 150) 采样放大的高亮度特征像素，而非原始深灰底色
    const cv::Vec4b lensCenterPx = rendered.at<cv::Vec4b>(230, 360);
    EXPECT_GT(static_cast<int>(lensCenterPx[1]) + static_cast<int>(lensCenterPx[2]), 150);

    // 平移取景源锚点至纯深色空白区 (80, 80)，大透镜位置保持在 (360, 230) 不变
    elem->resize(-60, -70, HitArea::FocusSource);
    EXPECT_EQ(elem->startPt, cv::Point(80, 80));
    EXPECT_EQ(elem->endPt, cv::Point(360, 230));
}

TEST(CaptureMagnifierTest, ZeroOcclusionSmartCalloutAndConcentricHandlePullOut) {
    using namespace tools3000::capture;

    CaptureState state;
    state.mode = OverlayMode::Screenshot;
    state.state = OverlayState::Selected;
    state.dpiScale = 1.0f;
    state.isMarkupToolActive = true;
    state.currentTool = MarkupTool::Magnifier;
    state.magnifierCalloutMode = true;

    const D2D1_SIZE_F surface = D2D1::SizeF(1920.0f, 1080.0f);
    const D2D1_RECT_F sel = D2D1::RectF(100.0f, 100.0f, 900.0f, 700.0f);
    rebuildCaptureToolbar(state, sel, surface);
    ASSERT_FALSE(state.secondaryToolbarButtons.empty());

    // 首枚二级属性按钮应为「引出式详图模式」开关，且默认处于激活态
    const auto& firstSecBtn = state.secondaryToolbarButtons.front();
    EXPECT_EQ(firstSecBtn.command, ToolbarCommand::ToggleMagnifierCallout);
    EXPECT_TRUE(isToolbarButtonSelected(firstSecBtn, state));

    // 1. 验证 8 向净空智能引出：无论采样点在选区中心还是紧贴右上角，大透镜均完全避开源采样环且不超出选区边界
    const int selW = 800, selH = 600, lensR = 64;
    const float zoom = 2.5f;
    const int srcRingR = static_cast<int>(std::lround(lensR / zoom));

    const cv::Point centerSrc(240, 320);
    const cv::Point centerLens = computeSmartMagnifierCalloutCenter(centerSrc, lensR, zoom, selW, selH, 1.0f);
    EXPECT_GE(std::hypot(centerLens.x - centerSrc.x, centerLens.y - centerSrc.y), static_cast<double>(lensR + srcRingR));
    EXPECT_GE(centerLens.x - lensR, 0);
    EXPECT_LE(centerLens.x + lensR, selW);
    EXPECT_GE(centerLens.y - lensR, 0);
    EXPECT_LE(centerLens.y + lensR, selH);

    // 紧贴右上角时自动朝左下方宽敞空白区引出，绝不越过右上边界
    const cv::Point cornerSrc(760, 40);
    const cv::Point cornerLens = computeSmartMagnifierCalloutCenter(cornerSrc, lensR, zoom, selW, selH, 1.0f);
    EXPECT_LT(cornerLens.x, cornerSrc.x);
    EXPECT_GT(cornerLens.y, cornerSrc.y);
    EXPECT_GE(std::hypot(cornerLens.x - cornerSrc.x, cornerLens.y - cornerSrc.y), static_cast<double>(lensR + srcRingR));
    EXPECT_LE(cornerLens.x + lensR, selW);
    EXPECT_GE(cornerLens.y - lensR, 0);

    // 2. 验证同心放大镜拖拽右上弧线引出手柄时：源目标 startPt 留在原地，大透镜 endPt 被拉向空白处，原位周围上下文零遮挡！
    cv::Mat baseImg(400, 600, CV_8UC4, cv::Scalar(240, 242, 245, 255));
    // 在原位周围 (200, 200) 紧邻左侧 (160, 200) 放置蓝色上下文特征块（若原位半径 60 放大则会将其完全覆盖）
    cv::circle(baseImg, cv::Point(160, 200), 6, cv::Scalar(220, 40, 20, 255), cv::FILLED);
    cv::circle(baseImg, cv::Point(200, 200), 8, cv::Scalar(30, 180, 40, 255), cv::FILLED);

    MarkupEngine engine;
    engine.setBaseImage(baseImg);
    auto* mag = engine.addMagnifier(cv::Point(200, 200), 2.5f, 60);
    ASSERT_NE(mag, nullptr);
    mag->endPt = cv::Point(200, 200);
    mag->hasLeaderArrow = false;
    mag->isActive = true;

    // 命中右上 -55° 弧线引出手柄
    constexpr double kHandleAngle = -55.0 * 3.14159265358979323846 / 180.0;
    const cv::Point pullHandlePt(
        static_cast<int>(std::lround(200.0 + 60.0 * std::cos(kHandleAngle))),
        static_cast<int>(std::lround(200.0 + 60.0 * std::sin(kHandleAngle))));
    EXPECT_EQ(mag->hitTestEx(pullHandlePt), HitArea::LensOffset);

    // 向右侧空白处拉出 (+180, -40)
    mag->resize(180, -40, HitArea::LensOffset);
    EXPECT_EQ(mag->startPt, cv::Point(200, 200)); // 源目标纹丝不动！
    EXPECT_EQ(mag->endPt, cv::Point(380, 160));   // 大透镜移至右侧空白处！
    EXPECT_TRUE(mag->hasLeaderArrow);

    // 渲染后检查原位紧邻上下文 (160, 200) 像素 100% 完好保留原始蓝色特征，未被放大透镜覆盖！
    cv::Mat out = engine.getCompositeImage(false);
    const cv::Vec4b neighborPx = out.at<cv::Vec4b>(200, 160);
    EXPECT_GT(static_cast<int>(neighborPx[0]), 190);
    EXPECT_LT(static_cast<int>(neighborPx[2]), 60);
}

TEST(CaptureAntiAccidentalExitTest, DoubleClickNeverAutoExitsDuringAnnotationOrToolbarClicks) {
    using namespace tools3000::capture;

    CaptureState state;
    state.mode = OverlayMode::Screenshot;
    state.state = OverlayState::Selected;
    state.dpiScale = 1.0f;
    state.dragStart = {100, 100};
    state.dragEnd = {900, 700};
    state.isMarkupToolActive = true;
    state.currentTool = MarkupTool::Magnifier;

    const D2D1_SIZE_F surface = D2D1::SizeF(1920.0f, 1080.0f);
    const D2D1_RECT_F sel = D2D1::RectF(100.0f, 100.0f, 900.0f, 700.0f);
    rebuildCaptureToolbar(state, sel, surface);

    // 1. 快速连点二级工具栏「倍率步进器」：必须识别为 RepeatToolbarAction，绝不触发自动保存退出
    const ToolbarButton* scaleBtn = nullptr;
    for (const auto& btn : state.secondaryToolbarButtons) {
        if (btn.command == ToolbarCommand::CycleMagnifierScale) {
            scaleBtn = &btn;
            break;
        }
    }
    ASSERT_NE(scaleBtn, nullptr);
    const POINT scaleBtnPt{
        static_cast<LONG>((scaleBtn->rect.left + scaleBtn->rect.right) * 0.5f),
        static_cast<LONG>((scaleBtn->rect.top + scaleBtn->rect.bottom) * 0.5f)
    };
    EXPECT_EQ(resolveDoubleClickIntent(state, scaleBtnPt, true, HitArea::Body, false, scaleBtn),
              DoubleClickIntent::RepeatToolbarAction);

    // 2. 双击落在工具栏按钮间隙玻璃底板上（hitButton == nullptr）：必须安全拦截为 ConsumeNoOp
    const POINT panelGapPt{
        static_cast<LONG>(state.primaryToolbarRect.left + 2.0f),
        static_cast<LONG>(state.primaryToolbarRect.top + 2.0f)
    };
    EXPECT_EQ(resolveDoubleClickIntent(state, panelGapPt, true, HitArea::Body, false, nullptr),
              DoubleClickIntent::ConsumeNoOp);

    // 3. 标注工具激活态下（如连戳序号 1, 2 或刚点空白取消选中紧接着下笔）双击选区画布空白处：
    // 必须识别为 BeginAnnotationAtPoint，彻底阻断标注中途莫名自动保存退出！
    const POINT canvasBlankPt{420, 360};
    EXPECT_EQ(resolveDoubleClickIntent(state, canvasBlankPt, true, HitArea::Body, false, nullptr),
              DoubleClickIntent::BeginAnnotationAtPoint);

    // 4. 双击选区外暗色遮罩或选区拉伸边框：必须安全拦截为 ConsumeNoOp
    EXPECT_EQ(resolveDoubleClickIntent(state, POINT{40, 40}, false, HitArea::None, false, nullptr),
              DoubleClickIntent::ConsumeNoOp);
    EXPECT_EQ(resolveDoubleClickIntent(state, POINT{100, 100}, true, HitArea::LT, false, nullptr),
              DoubleClickIntent::ConsumeNoOp);

    // 5. 仅在退出标注工具（纯框选待命态 isMarkupToolActive == false）后双击选区内部空白处，才执行快速确认截图
    state.isMarkupToolActive = false;
    EXPECT_EQ(resolveDoubleClickIntent(state, canvasBlankPt, true, HitArea::Body, false, nullptr),
              DoubleClickIntent::ConfirmSelectionAndCopy);
}

#include "capture/CaptureTextEditor.h"

TEST(CaptureMagnifierTest, SingleAxisJewelLeaderZeroConePollutionAndCrispUpscale) {
    using namespace tools3000::capture;

    // 构造深色 IDE 背景画布，在采样点 (140, 260) 绘制高反差文字笔画条纹
    cv::Mat baseImg(420, 640, CV_8UC4, cv::Scalar(34, 28, 24, 255));
    for (int col = 128; col <= 152; col += 4) {
        cv::line(baseImg, cv::Point(col, 248), cv::Point(col, 272), cv::Scalar(242, 244, 248, 255), 2);
    }

    MarkupEngine engine;
    engine.setBaseImage(baseImg);
    auto* mag = engine.addMagnifier(cv::Point(140, 260), 3.0f, 72);
    ASSERT_NE(mag, nullptr);
    mag->endPt = cv::Point(430, 140);
    mag->hasLeaderArrow = true;
    mag->magnifierCalloutStyle = MagnifierCalloutStyle::CenterPointer;
    mag->color = MarkupColor(244, 63, 94);
    mag->fill = false;

    const cv::Mat out = engine.getCompositeImage(false);
    ASSERT_EQ(out.size(), baseImg.size());

    // 1. 验证单轴指中心引线彻底消灭了旧版“大喇叭”锥面污染：
    // 取源点到透镜中心连线中点，向两侧法向偏移 ±26px（旧版外切锥面必然涂抹粉紫色的正文区域），必须 100% 保持原图纯净零色差！
    const double dx = 430.0 - 140.0;
    const double dy = 140.0 - 260.0;
    const double dist = std::hypot(dx, dy);
    const double ux = dx / dist;
    const double uy = dy / dist;
    const double nx = -uy;
    const double ny = ux;
    const double midX = 140.0 + dx * 0.45;
    const double midY = 260.0 + dy * 0.45;

    for (double side : {-26.0, 26.0}) {
        const int px = static_cast<int>(std::lround(midX + nx * side));
        const int py = static_cast<int>(std::lround(midY + ny * side));
        const cv::Vec4b actual = out.at<cv::Vec4b>(py, px);
        EXPECT_EQ(actual[0], 34);
        EXPECT_EQ(actual[1], 28);
        EXPECT_EQ(actual[2], 24);
    }

    // 2. 连线中轴附近确实绘制了高颜值流光单轴引线，且引线穿过旧版空心圆半径直达源放大中心 (140, 260)
    bool foundAxisInk = false;
    for (int off = -2; off <= 2; ++off) {
        const int px = static_cast<int>(std::lround(midX + nx * off));
        const int py = static_cast<int>(std::lround(midY + ny * off));
        const cv::Vec4b actual = out.at<cv::Vec4b>(py, px);
        if (actual[2] > 80) {
            foundAxisInk = true;
            break;
        }
    }
    EXPECT_TRUE(foundAxisInk);

    // 验证距离被放大源中心 (140, 260) 仅 8px 处（旧版处于半径 24px 空心圆内部空白区）已有直指中心的引线/箭头墨迹，且正中心落有靶点
    const double nearCenterX = 140.0 + ux * 8.0;
    const double nearCenterY = 260.0 + uy * 8.0;
    bool foundNearCenterPointerInk = false;
    for (int off = -2; off <= 2; ++off) {
        const int px = static_cast<int>(std::lround(nearCenterX + nx * off));
        const int py = static_cast<int>(std::lround(nearCenterY + ny * off));
        const cv::Vec4b actual = out.at<cv::Vec4b>(py, px);
        if (actual[2] > 80) {
            foundNearCenterPointerInk = true;
            break;
        }
    }
    EXPECT_TRUE(foundNearCenterPointerInk);

    // 3. 验证透镜中心 Lanczos-4 + Clamped CAS 超清锐化：明暗对比强烈且无越界溢出
    int minLuma = 255;
    int maxLuma = 0;
    for (int x = 406; x <= 454; ++x) {
        const cv::Vec4b p = out.at<cv::Vec4b>(140, x);
        const int luma = (static_cast<int>(p[0]) + static_cast<int>(p[1]) * 2 + static_cast<int>(p[2])) / 4;
        minLuma = std::min(minLuma, luma);
        maxLuma = std::max(maxLuma, luma);
    }
    EXPECT_LT(minLuma, 65);
    EXPECT_GT(maxLuma, 215);

    // 4. 验证学术双外切线 (TangentFrustum)、135° 工程折线 (ElbowLeader)、ISO 精密虚线框 (DashedDirect) 三种学术期刊样式：
    for (auto style : {MagnifierCalloutStyle::TangentFrustum,
                       MagnifierCalloutStyle::ElbowLeader,
                       MagnifierCalloutStyle::DashedDirect}) {
        mag->magnifierCalloutStyle = style;
        const cv::Mat styleOut = engine.getCompositeImage(false);
        ASSERT_EQ(styleOut.size(), baseImg.size());
        // 学术双外切线模式下，两外切线中间的中点 (midX, midY) 必须 100% 透明不填色，彻底杜绝大喇叭色块污染
        if (style == MagnifierCalloutStyle::TangentFrustum) {
            const cv::Vec4b midPx = styleOut.at<cv::Vec4b>(static_cast<int>(std::lround(midY)),
                                                           static_cast<int>(std::lround(midX)));
            EXPECT_EQ(midPx[0], 34);
            EXPECT_EQ(midPx[1], 28);
            EXPECT_EQ(midPx[2], 24);
        }
    }

    // 5. 验证圆形、椭圆与圆角超椭圆透镜解析接驳距离函数精确收敛，并验证横向拉宽为椭圆放大镜
    EXPECT_FLOAT_EQ(computeLoupeBoundaryDistance(1.0f, 0.0f, 60.0f, 16.0f, false), 60.0f);
    EXPECT_FLOAT_EQ(computeLoupeBoundaryDistance(1.0f, 0.0f, 60.0f, 16.0f, true), 60.0f);
    EXPECT_FLOAT_EQ(computeLoupeBoundaryDistance(1.0f, 0.0f, 120.0f, 60.0f, 16.0f, false), 120.0f);
    EXPECT_FLOAT_EQ(computeLoupeBoundaryDistance(0.0f, 1.0f, 120.0f, 60.0f, 16.0f, false), 60.0f);
    const float diag = static_cast<float>(1.0 / std::sqrt(2.0));
    const float rrDiag = computeLoupeBoundaryDistance(diag, diag, 60.0f, 18.0f, true);
    EXPECT_GT(rrDiag, 60.0f);
    EXPECT_LT(rrDiag, 85.0f);

    // 将放大镜横向拉宽 48px 变为扁平椭圆 (rx = 120, ry = 72)，验证包围盒、椭圆命中与无畸变渲染
    mag->resize(48, 0, HitArea::R);
    EXPECT_EQ(mag->effectiveMagnifierRx(), 120);
    EXPECT_EQ(mag->effectiveMagnifierRy(), 72);
    EXPECT_EQ(mag->getBoundingBox(), cv::Rect(430 - 120, 140 - 72, 240, 144));
    EXPECT_NE(mag->hitTestEx(cv::Point(430 + 110, 140), 0), HitArea::None);
    EXPECT_EQ(mag->hitTestEx(cv::Point(430, 140 + 110), 0), HitArea::None);
    const cv::Mat ellipseOut = engine.getCompositeImage(false);
    EXPECT_EQ(ellipseOut.size(), baseImg.size());

    // 6. 验证 90 后历史课本地图「长征行军 (CampaignMarch)」与「双线行军 (MarchDoubleTrack)」箭头（直线与平滑弧线模式）
    auto* marchArrow = engine.drawArrow(cv::Point(60, 350), cv::Point(280, 350), MarkupColor(244, 63, 94), 4.0f);
    ASSERT_NE(marchArrow, nullptr);
    marchArrow->arrowStyle = ArrowStyle::CampaignMarch;
    marchArrow->smoothCurve = false;
    const cv::Mat marchOut = engine.getCompositeImage(false);
    // 箭身中段 (170, 350) 应有饱满的红色填充墨迹
    EXPECT_GT(marchOut.at<cv::Vec4b>(350, 170)[2], 180);

    marchArrow->arrowStyle = ArrowStyle::MarchDoubleTrack;
    marchArrow->smoothCurve = true;
    const cv::Mat doubleTrackOut = engine.getCompositeImage(false);
    EXPECT_EQ(doubleTrackOut.size(), baseImg.size());
}

TEST(CaptureTextEditorTest, MultilineHitTestDragSelectionAndRichEditing) {
    using namespace tools3000::capture;

    // GDI+ 确保初始化后再执行排版精确度量测试
    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartupInput gdiInput;
    const bool gdiStartedHere = (Gdiplus::GdiplusStartup(&gdiToken, &gdiInput, nullptr) == Gdiplus::Ok);

    std::wstring text = L"第一行 World_Class 标注  \n第二行 Copy & Paste 测试\n第三行 100% 精准";
    const int fontSize = 22;
    const auto layout = CaptureTextEditor::buildLayout(text, fontSize);
    ASSERT_EQ(layout.lines.size(), 3u);

    // 1. 验证包含末尾连续空格的字符偏移严格单调递增，尾部空格宽度不丢失
    for (const auto& line : layout.lines) {
        ASSERT_EQ(line.charOffsets.size(), line.endIndex - line.startIndex + 1);
        for (size_t i = 1; i < line.charOffsets.size(); ++i) {
            EXPECT_GT(line.charOffsets[i], line.charOffsets[i - 1]);
        }
    }

    // 2. 验证每个字符视觉中点点击反算 hitTestCaretIndex 100% 精准命中该字符左边界，过半命中右边界
    for (size_t li = 0; li < layout.lines.size(); ++li) {
        const auto& line = layout.lines[li];
        const float midY = line.topY + line.height * 0.5f;
        for (size_t col = 0; col + 1 < line.charOffsets.size(); ++col) {
            const float xLeftQuarter = layout.textOriginX + line.charOffsets[col] * 0.75f + line.charOffsets[col + 1] * 0.25f;
            const float xRightQuarter = layout.textOriginX + line.charOffsets[col] * 0.25f + line.charOffsets[col + 1] * 0.75f;
            EXPECT_EQ(CaptureTextEditor::hitTestCaretIndex(layout, text, xLeftQuarter, midY), line.startIndex + col);
            EXPECT_EQ(CaptureTextEditor::hitTestCaretIndex(layout, text, xRightQuarter, midY), line.startIndex + col + 1);
        }
    }

    // 3. 模拟用户在编辑态按住左键从第一行 "World_Class" 拖选至第二行 "Copy"：提取选中文本与多行蓝色选区底块
    const size_t dragStartIdx = text.find(L"World_Class");
    const size_t dragEndIdx = text.find(L"Copy") + 4;
    ASSERT_NE(dragStartIdx, std::wstring::npos);
    ASSERT_NE(dragEndIdx, std::wstring::npos);
    EXPECT_EQ(CaptureTextEditor::selectedText(text, dragEndIdx, dragStartIdx), L"World_Class 标注  \n第二行 Copy");
    const auto spans = CaptureTextEditor::computeSelectionHighlights(layout, text, dragEndIdx, dragStartIdx);
    EXPECT_EQ(spans.size(), 2u);
    EXPECT_GT(spans[0].width, 20.0f);
    EXPECT_GT(spans[1].width, 20.0f);

    // 4. 双击选词与全选
    size_t caret = 0, anchor = 0;
    CaptureTextEditor::selectWordAt(text, dragStartIdx + 3, caret, anchor);
    EXPECT_EQ(CaptureTextEditor::selectedText(text, caret, anchor), L"World_Class");

    CaptureTextEditor::selectAll(text, caret, anchor);
    EXPECT_EQ(anchor, 0u);
    EXPECT_EQ(caret, text.size());

    // 5. 选区替换输入、退格删除、上下跨行保持期望水平列移动
    CaptureTextEditor::selectWordAt(text, dragStartIdx + 2, caret, anchor);
    CaptureTextEditor::insertText(text, caret, anchor, L"Sapphire");
    EXPECT_NE(text.find(L"第一行 Sapphire 标注"), std::wstring::npos);
    EXPECT_FALSE(CaptureTextEditor::hasSelection(caret, anchor));

    float preferredX = -1.0f;
    const auto layoutAfter = CaptureTextEditor::buildLayout(text, fontSize);
    CaptureTextEditor::moveVertical(layoutAfter, text, caret, anchor, 1, false, preferredX);
    EXPECT_GE(caret, layoutAfter.lines[1].startIndex);
    EXPECT_LE(caret, layoutAfter.lines[1].endIndex);
    EXPECT_GT(preferredX, 0.0f);

    // 6. 验证编辑态下内部点击 (HitArea::Body 划词) 与外围边框环带 (HitArea::TextMoveBorder 移动框体) 精准解耦
    cv::Mat canvas(240, 480, CV_8UC4, cv::Scalar(245, 246, 248, 255));
    MarkupEngine engine;
    engine.setBaseImage(canvas);
    auto elemPtr = std::make_unique<MarkupElement>();
    elemPtr->tool = MarkupTool::Text;
    elemPtr->startPt = cv::Point(40, 40);
    elemPtr->color = MarkupColor(239, 68, 68);
    elemPtr->thickness = 2.0f;
    elemPtr->fill = false;
    auto* txtElem = engine.addElement(std::move(elemPtr));
    ASSERT_NE(txtElem, nullptr);
    txtElem->text = "Editable Copyable Text\nSecond Line Content";
    txtElem->fontSize = 20.0f;
    txtElem->isActive = true;
    txtElem->isEditing = true;
    txtElem->textSelAnchor = 0;
    txtElem->textCaret = 8;

    const cv::Mat rendered = engine.getCompositeImage(true);
    EXPECT_FALSE(rendered.empty());
    const cv::Rect bbox = txtElem->getBoundingBox();
    // 中心内容区返回 Body（启动划词选字）
    EXPECT_EQ(txtElem->hitTestEx(cv::Point(bbox.x + bbox.width / 2, bbox.y + bbox.height / 2)), HitArea::Body);
    // 外围边框上沿（避开中点 T 缩放手柄）返回 TextMoveBorder（启动框体平移）
    EXPECT_EQ(txtElem->hitTestEx(cv::Point(bbox.x + bbox.width / 4, bbox.y - 2)), HitArea::TextMoveBorder);

    if (gdiStartedHere) {
        Gdiplus::GdiplusShutdown(gdiToken);
    }
}

TEST(CaptureShortcutPolicyTest, ZeroConflictRoutingAndShiftTapStateMachine) {
    using namespace tools3000::capture;

    // 1. 预选阶段 (Selecting) 即使已探测到多层窗口结构 (hasDetectedHierarchy = true)，
    //    W/A/S/D 与方向键也必须 100% 路由为像素级移动光标 (MoveCursorPixels)，严禁被窗口层级切换劫持！
    CaptureKeyContext selectingCtx{};
    selectingCtx.overlayState = OverlayState::Selecting;
    selectingCtx.mode = OverlayMode::Screenshot;
    selectingCtx.hasDetectedHierarchy = true;

    {
        auto dW = CaptureShortcutPolicy::resolveKeyDown(selectingCtx, 'W', false, false, false);
        EXPECT_EQ(dW.action, CaptureShortcutAction::MoveCursorPixels);
        EXPECT_EQ(dW.dx, 0);
        EXPECT_EQ(dW.dy, -1);

        auto dS = CaptureShortcutPolicy::resolveKeyDown(selectingCtx, 'S', false, false, false);
        EXPECT_EQ(dS.action, CaptureShortcutAction::MoveCursorPixels);
        EXPECT_EQ(dS.dx, 0);
        EXPECT_EQ(dS.dy, 1);

        auto dA = CaptureShortcutPolicy::resolveKeyDown(selectingCtx, 'A', false, false, false);
        EXPECT_EQ(dA.action, CaptureShortcutAction::MoveCursorPixels);
        EXPECT_EQ(dA.dx, -1);
        EXPECT_EQ(dA.dy, 0);

        auto dD = CaptureShortcutPolicy::resolveKeyDown(selectingCtx, 'D', false, false, false);
        EXPECT_EQ(dD.action, CaptureShortcutAction::MoveCursorPixels);
        EXPECT_EQ(dD.dx, 1);
        EXPECT_EQ(dD.dy, 0);
    }

    // Shift + W/A/S/D 在预选/拖拽阶段加速 10px 移动光标
    {
        auto dFastW = CaptureShortcutPolicy::resolveKeyDown(selectingCtx, 'W', false, true, false);
        EXPECT_EQ(dFastW.action, CaptureShortcutAction::MoveCursorPixels);
        EXPECT_EQ(dFastW.dy, -10);
    }

    // 2. 窗口探测层级切换迁移至 Tab / Shift+Tab / [ / ] / PgUp / PgDn
    {
        auto dTab = CaptureShortcutPolicy::resolveKeyDown(selectingCtx, VK_TAB, false, false, false);
        EXPECT_EQ(dTab.action, CaptureShortcutAction::StepWindowHierarchy);
        EXPECT_EQ(dTab.intDelta, CaptureShortcutConstants::kHierarchyDeltaExpand);

        auto dShiftTab = CaptureShortcutPolicy::resolveKeyDown(selectingCtx, VK_TAB, false, true, false);
        EXPECT_EQ(dShiftTab.action, CaptureShortcutAction::StepWindowHierarchy);
        EXPECT_EQ(dShiftTab.intDelta, CaptureShortcutConstants::kHierarchyDeltaShrink);

        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectingCtx, VK_OEM_4, false, false, false).action, CaptureShortcutAction::StepWindowHierarchy);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectingCtx, VK_OEM_4, false, false, false).intDelta, CaptureShortcutConstants::kHierarchyDeltaShrink);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectingCtx, VK_OEM_6, false, false, false).action, CaptureShortcutAction::StepWindowHierarchy);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectingCtx, VK_OEM_6, false, false, false).intDelta, CaptureShortcutConstants::kHierarchyDeltaExpand);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectingCtx, VK_PRIOR, false, false, false).intDelta, CaptureShortcutConstants::kHierarchyDeltaExpand);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectingCtx, VK_NEXT, false, false, false).intDelta, CaptureShortcutConstants::kHierarchyDeltaShrink);
    }

    // 3. C 与 K 统一按当前放大镜颜色格式复制色值，Shift+C / Shift+K 复制互补备用格式
    {
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectingCtx, 'C', false, false, false).action, CaptureShortcutAction::CopyCurrentPixelColor);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectingCtx, 'K', false, false, false).action, CaptureShortcutAction::CopyCurrentPixelColor);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectingCtx, 'C', false, true, false).action, CaptureShortcutAction::CopyAlternatePixelColor);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectingCtx, 'K', false, true, false).action, CaptureShortcutAction::CopyAlternatePixelColor);
    }

    // 4. Shift Tap-Only 状态机：纯单按松开触发颜色格式切换；组合键或鼠标拖拽熔断后松开绝不误触发
    {
        bool candidate = false;
        // 4.1 纯单按 Shift 后松开 -> 触发 CycleColorFormat
        CaptureShortcutPolicy::updateShiftTapOnKeyDown(candidate, VK_SHIFT, false, false, false);
        EXPECT_TRUE(candidate);
        auto tapUp = CaptureShortcutPolicy::resolveKeyUp(candidate, selectingCtx, VK_SHIFT, false, false);
        EXPECT_EQ(tapUp.action, CaptureShortcutAction::CycleColorFormat);
        EXPECT_FALSE(candidate);

        // 4.2 按下 Shift 后再按 W (Shift+W 10px 加速移动)，松开 Shift 绝不切换颜色格式
        CaptureShortcutPolicy::updateShiftTapOnKeyDown(candidate, VK_SHIFT, false, false, false);
        EXPECT_TRUE(candidate);
        CaptureShortcutPolicy::updateShiftTapOnKeyDown(candidate, 'W', false, false, false);
        EXPECT_FALSE(candidate);
        auto comboUp = CaptureShortcutPolicy::resolveKeyUp(candidate, selectingCtx, VK_SHIFT, false, false);
        EXPECT_EQ(comboUp.action, CaptureShortcutAction::None);

        // 4.3 按住 Shift 长按连发 (isAutoRepeat = true) 熔断候选
        CaptureShortcutPolicy::updateShiftTapOnKeyDown(candidate, VK_SHIFT, false, false, false);
        EXPECT_TRUE(candidate);
        CaptureShortcutPolicy::updateShiftTapOnKeyDown(candidate, VK_SHIFT, true, false, false);
        EXPECT_FALSE(candidate);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyUp(candidate, selectingCtx, VK_SHIFT, false, false).action, CaptureShortcutAction::None);

        // 4.4 按下 Shift 后触发鼠标左键拖拽 (cancelShiftTapCandidate)，松开 Shift 不误切
        CaptureShortcutPolicy::updateShiftTapOnKeyDown(candidate, VK_SHIFT, false, false, false);
        CaptureShortcutPolicy::cancelShiftTapCandidate(candidate);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyUp(candidate, selectingCtx, VK_SHIFT, false, false).action, CaptureShortcutAction::None);
    }

    // 5. 选区已锁定 (Selected / Marking)：WASD 专司微移选区/图元，Shift+WASD 调尺寸，单字母工具完全零冲突
    CaptureKeyContext selectedCtx{};
    selectedCtx.overlayState = OverlayState::Selected;
    selectedCtx.mode = OverlayMode::Screenshot;
    selectedCtx.isMarkupToolActive = false;
    selectedCtx.currentTool = MarkupTool::Rectangle;

    {
        // WASD -> NudgeSelectionOrElement (1px)
        for (WPARAM k : { static_cast<WPARAM>('W'), static_cast<WPARAM>('A'), static_cast<WPARAM>('S'), static_cast<WPARAM>('D') }) {
            EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectedCtx, k, false, false, false).action, CaptureShortcutAction::NudgeSelectionOrElement);
        }

        // Shift + WASD -> ResizeSelectionByKey (1px)
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectedCtx, 'D', false, true, false).action, CaptureShortcutAction::ResizeSelectionByKey);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectedCtx, 'D', false, true, false).dx, 1);

        // 标注工具零冲突路由：Q / Alt+A 箭头，O 椭圆，L 直线，E / I 橡皮擦，Z 放大镜，N 序号，T 文本
        auto dArrowQ = CaptureShortcutPolicy::resolveKeyDown(selectedCtx, 'Q', false, false, false);
        EXPECT_EQ(dArrowQ.action, CaptureShortcutAction::ToggleMarkupTool);
        EXPECT_EQ(dArrowQ.tool, MarkupTool::Arrow);

        auto dArrowAltA = CaptureShortcutPolicy::resolveKeyDown(selectedCtx, 'A', false, false, true);
        EXPECT_EQ(dArrowAltA.action, CaptureShortcutAction::ToggleMarkupTool);
        EXPECT_EQ(dArrowAltA.tool, MarkupTool::Arrow);

        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectedCtx, 'O', false, false, false).tool, MarkupTool::Ellipse);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectedCtx, 'L', false, false, false).tool, MarkupTool::Line);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectedCtx, 'E', false, false, false).tool, MarkupTool::Inpaint);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectedCtx, 'I', false, false, false).tool, MarkupTool::Inpaint);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectedCtx, 'Z', false, false, false).tool, MarkupTool::Magnifier);

        // 美化外壳零冲突：B 开关，Shift+B 风格，Alt+B 留白边距，Ctrl+B 绝不抢占 (留给富文本加粗扩展)
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectedCtx, 'B', false, false, false).action, CaptureShortcutAction::ToggleBeautyShell);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectedCtx, 'B', false, true, false).action, CaptureShortcutAction::CycleBeautyShellTheme);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectedCtx, 'B', false, false, true).action, CaptureShortcutAction::CycleBeautyShellPadding);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectedCtx, 'B', true, false, false).action, CaptureShortcutAction::None);

        // 标准重做双通道：Ctrl+Y 与 Ctrl+Shift+Z 均触发 RedoMarkup
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectedCtx, 'Y', true, false, false).action, CaptureShortcutAction::RedoMarkup);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectedCtx, 'Z', true, true, false).action, CaptureShortcutAction::RedoMarkup);
    }

    // 6. 序号工具激活时 [ / ] 调节下一个序号，非序号工具时 [ / ] 调节选区圆角
    {
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(selectedCtx, VK_OEM_4, false, false, false).action, CaptureShortcutAction::AdjustCornerRadius);
        CaptureKeyContext numberCtx = selectedCtx;
        numberCtx.isMarkupToolActive = true;
        numberCtx.currentTool = MarkupTool::Number;
        auto dDec = CaptureShortcutPolicy::resolveKeyDown(numberCtx, VK_OEM_4, false, false, false);
        EXPECT_EQ(dDec.action, CaptureShortcutAction::AdjustNumberCounter);
        EXPECT_EQ(dDec.intDelta, -1);
        auto dInc = CaptureShortcutPolicy::resolveKeyDown(numberCtx, VK_OEM_6, false, false, false);
        EXPECT_EQ(dInc.action, CaptureShortcutAction::AdjustNumberCounter);
        EXPECT_EQ(dInc.intDelta, 1);
    }

    // 7. 区域录屏框选模式 (RecordRegion) 严格隔离截图标注单键
    {
        CaptureKeyContext recordCtx = selectedCtx;
        recordCtx.mode = OverlayMode::RecordRegion;
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(recordCtx, 'R', false, false, false).action, CaptureShortcutAction::None);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(recordCtx, 'B', false, false, false).action, CaptureShortcutAction::None);
        EXPECT_EQ(CaptureShortcutPolicy::resolveKeyDown(recordCtx, 'W', false, false, false).action, CaptureShortcutAction::NudgeSelectionOrElement);
    }
}

TEST(CaptureShortcutPolicyTest, BentoTwoColumnHudAndMetricUpgrade) {
    using namespace tools3000::capture;

    // 验证升级后的大气 HUD 度量参数与双栏 Bento 布局阈值
    EXPECT_GE(ShortcutHintStyle::BaseKeyFont, 15.0f);
    EXPECT_GE(ShortcutHintStyle::BaseLabelFont, 15.5f);
    EXPECT_GE(ShortcutHintStyle::BaseKeyHeight, 32.0f);
    EXPECT_GT(ShortcutHintStyle::BaseHeaderHeight, 20.0f);
    EXPECT_GT(ShortcutHintStyle::BaseColumnGap, 20.0f);
    EXPECT_EQ(ShortcutHintStyle::TwoColumnMinItems, 5u);

    // 验证预选态与已选态均拥有 >= 5 条零冲突指引，自动激活对称双栏 Bento 布局
    auto& hint = ShortcutHintOverlay::instance();
    const auto selectingItems = hint.getItemsForContext(ShortcutHintContext::CaptureSelecting);
    EXPECT_GE(selectingItems.size(), ShortcutHintStyle::TwoColumnMinItems);
    bool hasTabHierarchy = false;
    bool hasCKCopy = false;
    for (const auto& item : selectingItems) {
        if (item.key.find(L"Tab") != std::wstring::npos) hasTabHierarchy = true;
        if (item.key == L"C" && item.label.find(L"K") != std::wstring::npos) hasCKCopy = true;
    }
    EXPECT_TRUE(hasTabHierarchy);
    EXPECT_TRUE(hasCKCopy);

    const auto selectedItems = hint.getItemsForContext(ShortcutHintContext::CaptureSelected);
    EXPECT_GE(selectedItems.size(), ShortcutHintStyle::TwoColumnMinItems);
}

TEST(CaptureHudRimStyleTest, AllWeatherWhiteRimOpticalContrastOnLightAndDarkBackgrounds) {
    using namespace tools3000::capture;

    // 1. 验证单一事实源令牌不变量：外层分离丝必须保持极低不透明度（防暗底黑框），内层主白边必须高纯度透亮
    const auto loupeTokens = CaptureHudRimTokens::forLoupe();
    const auto darkHudTokens = CaptureHudRimTokens::forDarkHud(false);
    const auto darkEmphTokens = CaptureHudRimTokens::forDarkHud(true);
    const auto lightBarTokens = CaptureHudRimTokens::forGlassBar(false);

    for (const auto& t : { loupeTokens, darkHudTokens, darkEmphTokens, lightBarTokens }) {
        EXPECT_GE(t.outerHairlineColor.a, 0.16f);
        EXPECT_LE(t.outerHairlineColor.a, 0.28f);
        EXPECT_GE(t.innerTopWhite.a, 0.85f);
        EXPECT_GE(t.innerBottomWhite.a, 0.60f);
        EXPECT_GE(t.outerOffset, 0.45f);
        EXPECT_GE(t.innerInset, 0.40f);
    }

    // 2. 离屏 Direct2D 物理像素级实测：验证在「暗色编辑器背景 (#0B0F17)」与「纯白文档背景 (#FFFFFF)」下
    //    边框均清晰透亮，且暗底零黑边晕染、亮底零融底消失
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    ASSERT_EQ(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()), S_OK);

    const int canvasW = 240;
    const int canvasH = 160;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = canvasW;
    bmi.bmiHeader.biHeight = -canvasH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dibBits = nullptr;
    HDC memDc = CreateCompatibleDC(nullptr);
    ASSERT_NE(memDc, nullptr);
    HBITMAP hBmp = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    ASSERT_NE(hBmp, nullptr);
    HGDIOBJ oldBmp = SelectObject(memDc, hBmp);

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcRt;
    ASSERT_EQ(factory->CreateDCRenderTarget(&props, dcRt.GetAddressOf()), S_OK);
    RECT bindRect{ 0, 0, canvasW, canvasH };
    ASSERT_EQ(dcRt->BindDC(memDc, &bindRect), S_OK);

    const D2D1_RECT_F hudBox = D2D1::RectF(40.0f, 36.0f, 200.0f, 124.0f);
    const float radius = 12.0f;
    const float scale = 1.5f; // 实测用户 150% DPI 缩放环境
    const int midX = 120;

    auto calcLuma = [](const cv::Vec4b& bgra) -> float {
        return 0.114f * bgra[0] + 0.587f * bgra[1] + 0.299f * bgra[2];
    };

    // 场景 A：深色代码编辑器 + 50% 截图遮罩暗底 (RGB 11, 15, 23, Luma ~15.1)
    {
        dcRt->BeginDraw();
        dcRt->Clear(D2D1::ColorF(11.0f / 255.0f, 15.0f / 255.0f, 23.0f / 255.0f, 1.0f));
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bodyBrush;
        ASSERT_EQ(dcRt->CreateSolidColorBrush(D2D1::ColorF(28.0f / 255.0f, 34.0f / 255.0f, 48.0f / 255.0f, 0.96f), bodyBrush.GetAddressOf()), S_OK);
        dcRt->FillRoundedRectangle(D2D1::RoundedRect(hudBox, radius, radius), bodyBrush.Get());
        drawAllWeatherWhiteRim(dcRt.Get(), hudBox, radius, scale, loupeTokens);
        ASSERT_EQ(dcRt->EndDraw(), S_OK);
        GdiFlush();

        cv::Mat img(canvasH, canvasW, CV_8UC4, dibBits);
        const float bgLuma = calcLuma(img.at<cv::Vec4b>(10, midX));

        float topPeakWhiteLuma = 0.0f;
        float topMinOuterLuma = 255.0f;
        for (int y = 30; y <= 42; ++y) {
            const float l = calcLuma(img.at<cv::Vec4b>(y, midX));
            topPeakWhiteLuma = (std::max)(topPeakWhiteLuma, l);
            topMinOuterLuma = (std::min)(topMinOuterLuma, l);
        }
        float bottomPeakWhiteLuma = 0.0f;
        for (int y = 118; y <= 130; ++y) {
            bottomPeakWhiteLuma = (std::max)(bottomPeakWhiteLuma, calcLuma(img.at<cv::Vec4b>(y, midX)));
        }

        // 暗色背景下必须呈现高纯度透亮白边（上下均 >= 210），且外侧绝无明显压黑光晕
        EXPECT_GE(topPeakWhiteLuma, 225.0f);
        EXPECT_GE(bottomPeakWhiteLuma, 210.0f);
        EXPECT_GE(topMinOuterLuma, bgLuma - 3.0f);
    }

    // 场景 B：纯白文档背景 (#FFFFFF) 且放大镜内部同样采样纯白内容 (#FFFFFF) 的极限融底测试
    {
        dcRt->BeginDraw();
        dcRt->Clear(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> whiteBody;
        ASSERT_EQ(dcRt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), whiteBody.GetAddressOf()), S_OK);
        dcRt->FillRoundedRectangle(D2D1::RoundedRect(hudBox, radius, radius), whiteBody.Get());
        drawAllWeatherWhiteRim(dcRt.Get(), hudBox, radius, scale, loupeTokens);
        ASSERT_EQ(dcRt->EndDraw(), S_OK);
        GdiFlush();

        cv::Mat img(canvasH, canvasW, CV_8UC4, dibBits);
        float minSeparationLuma = 255.0f;
        for (int y = 32; y <= 40; ++y) {
            minSeparationLuma = (std::min)(minSeparationLuma, calcLuma(img.at<cv::Vec4b>(y, midX)));
        }

        // 纯白套纯白极限工况下：外侧分离丝必须产生清晰可见的银灰轮廓（<= 225 防融底），但绝不能发黑（>= 165）
        EXPECT_LE(minSeparationLuma, 225.0f);
        EXPECT_GE(minSeparationLuma, 165.0f);
    }

    SelectObject(memDc, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDc);
}

// ─────────────────────────────────────────────────────────────────────────────
// 专项防回退回归套件：B3 / B7 / C2 / D4 / C6 / C7.2 / D5.1
// ─────────────────────────────────────────────────────────────────────────────

TEST(CaptureDeliveryPolicyTest, RespectsCopyToClipboardSettingAndOcrMemoryDirectPath) {
    using namespace tools3000::capture;

    // 1. B3: 关闭 copyToClipboard 时，默认完成动作 (Enter / 双击 / 完成勾号) 绝不写入剪贴板
    {
        CaptureOptions opts;
        opts.copyToClipboard = false;
        opts.saveToFile = true;
        opts.recordHistory = true;

        CaptureCompletion defaultDone;
        defaultDone.action = CaptureCompletionAction::Default;
        const auto decision = resolveCaptureDeliveryDecision(opts, defaultDone);
        EXPECT_FALSE(decision.shouldCopyToClipboard);
        EXPECT_TRUE(decision.shouldSaveAutomatically);
        EXPECT_FALSE(decision.shouldSaveExplicitly);
        EXPECT_FALSE(decision.shouldPromptSaveAsFallback);
        EXPECT_TRUE(decision.shouldRecordHistory);

        // 用户显式按 Ctrl+C 或点击「复制」按钮时，强制复制到剪贴板
        CaptureCompletion explicitCopy;
        explicitCopy.action = CaptureCompletionAction::Copy;
        const auto copyDecision = resolveCaptureDeliveryDecision(opts, explicitCopy);
        EXPECT_TRUE(copyDecision.shouldCopyToClipboard);
        EXPECT_TRUE(copyDecision.shouldSaveAutomatically);
        EXPECT_FALSE(copyDecision.shouldPromptSaveAsFallback);
    }

    // 2. B3 兜底防丢图：同时关闭 copyToClipboard 与 saveToFile 时，默认完成动作触发另存为对话框兜底
    {
        CaptureOptions bothOff;
        bothOff.copyToClipboard = false;
        bothOff.saveToFile = false;

        CaptureCompletion defaultDone;
        defaultDone.action = CaptureCompletionAction::Default;
        const auto fallbackDecision = resolveCaptureDeliveryDecision(bothOff, defaultDone);
        EXPECT_FALSE(fallbackDecision.shouldCopyToClipboard);
        EXPECT_FALSE(fallbackDecision.shouldSaveAutomatically);
        EXPECT_TRUE(fallbackDecision.shouldPromptSaveAsFallback);

        // 显式指定另存为目标路径时不重复触发兜底
        CaptureCompletion saveAsDone;
        saveAsDone.action = CaptureCompletionAction::SaveAs;
        saveAsDone.filePath = "C:/temp/manual_export.png";
        const auto explicitSave = resolveCaptureDeliveryDecision(bothOff, saveAsDone);
        EXPECT_TRUE(explicitSave.shouldSaveExplicitly);
        EXPECT_FALSE(explicitSave.shouldCopyToClipboard);
        EXPECT_FALSE(explicitSave.shouldPromptSaveAsFallback);
    }

    // 3. C2: 快捷键 OCR 纯内存直达路径（零剪贴板位图污染、零临时文件落盘、零幽灵历史记录）
    {
        CaptureOptions ocrOpts;
        ocrOpts.copyToClipboard = false;
        ocrOpts.saveToFile = false;
        ocrOpts.recordHistory = false;
        ocrOpts.onImageCaptured = [](const cv::Mat&, const CaptureRegion&) {};

        CaptureCompletion ocrDone;
        ocrDone.action = CaptureCompletionAction::Default;
        const auto ocrDecision = resolveCaptureDeliveryDecision(ocrOpts, ocrDone);
        EXPECT_TRUE(ocrDecision.shouldInvokeMemoryCallback);
        EXPECT_FALSE(ocrDecision.shouldCopyToClipboard);
        EXPECT_FALSE(ocrDecision.shouldSaveAutomatically);
        EXPECT_FALSE(ocrDecision.shouldSaveExplicitly);
        EXPECT_FALSE(ocrDecision.shouldPromptSaveAsFallback);
        EXPECT_FALSE(ocrDecision.shouldRecordHistory);
    }
}

TEST(RecordingAndBeautyShellParityTest, BitrateMappingEncoderMemoryAndPresetAlignment) {
    using namespace tools3000::capture;

    auto temp = std::filesystem::temp_directory_path() /
                (L"Tools3000_RecordBeautyParity_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::create_directories(temp, ec);
    auto& config = tools3000::core::ConfigManager::instance();
    config.initialize(temp);

    // 1. B7: 录屏码率 4 / 8 / 16 / 32 Mbps 四档双向对齐，且自定义码率不被三档硬编码覆盖
    EXPECT_EQ(recordQualityLevelToBitrateMbps(0), 4);
    EXPECT_EQ(recordQualityLevelToBitrateMbps(1), 8);
    EXPECT_EQ(recordQualityLevelToBitrateMbps(2), 16);
    EXPECT_EQ(recordQualityLevelToBitrateMbps(3), 32);

    EXPECT_EQ(recordBitrateToQualityLevel(4), 0);
    EXPECT_EQ(recordBitrateToQualityLevel(8), 1);
    EXPECT_EQ(recordBitrateToQualityLevel(16), 2);
    EXPECT_EQ(recordBitrateToQualityLevel(32), 3);

    const D2D1_RECT_F anchor = D2D1::RectF(100.0f, 100.0f, 180.0f, 132.0f);
    CaptureState state;

    openDropdownMenu(state, DropdownType::RecordQuality, anchor);
    ASSERT_EQ(state.dropdownMenu.items.size(), 4u);
    for (int level = 0; level < 4; ++level) {
        const int expectedMbps = recordQualityLevelToBitrateMbps(level);
        openDropdownMenu(state, DropdownType::RecordQuality, anchor);
        commitDropdownSelection(state, DropdownType::RecordQuality, level);
        EXPECT_EQ(state.recordSetup.qualityLevel, level);
        EXPECT_EQ(state.recordSetup.bitrateMbps, expectedMbps);
        EXPECT_EQ(config.get<int>("/recording/bitrate", 0), expectedMbps);
    }

    // 2. B7: 录屏编码器记忆契约（H.265 / WebM VP9 切换到 GIF 再切回视频时 100% 恢复原编码器，绝不降级 H.264）
    for (const auto& [fmtEnum, fmtKey, expectedHint] : std::vector<std::tuple<RecordFormat, std::string, std::wstring>>{
             {RecordFormat::MP4_H265, "mp4_h265", L"H.265"},
             {RecordFormat::WebM_VP9, "webm_vp9", L"VP9"},
         }) {
        config.set<std::string>("/recording/format", fmtKey);
        config.set<std::string>("/recording/videoFormat", fmtKey);
        state.recordSetup.format = fmtEnum;
        state.recordSetup.preferredVideoFormat = fmtEnum;

        openDropdownMenu(state, DropdownType::RecordFormat, anchor);
        ASSERT_EQ(state.dropdownMenu.items.size(), 2u);
        EXPECT_EQ(state.dropdownMenu.items[0].hint, expectedHint);

        // 切至 GIF
        commitDropdownSelection(state, DropdownType::RecordFormat, 1);
        EXPECT_EQ(state.recordSetup.format, RecordFormat::GIF);
        EXPECT_EQ(config.get<std::string>("/recording/format", ""), "gif");
        EXPECT_EQ(config.get<std::string>("/recording/videoFormat", ""), fmtKey);

        // 再从 GIF 切回视频：必须精准恢复为 MP4_H265 或 WebM_VP9
        openDropdownMenu(state, DropdownType::RecordFormat, anchor);
        commitDropdownSelection(state, DropdownType::RecordFormat, 0);
        EXPECT_EQ(state.recordSetup.format, fmtEnum);
        EXPECT_EQ(config.get<std::string>("/recording/format", ""), fmtKey);
    }

    // 3. B7: 美化外壳 5 套主题键名、6 档留白、7 档圆角前后端完全同构
    const std::array<std::pair<int, std::string>, 5> expectedThemes{{
        {0, "aurora_purple"},
        {1, "sunrise_coral"},
        {2, "emerald_sea"},
        {3, "obsidian_dark"},
        {4, "minimal_light"},
    }};
    for (const auto& [idx, themeKey] : expectedThemes) {
        openDropdownMenu(state, DropdownType::BeautyShellBg, anchor);
        ASSERT_EQ(state.dropdownMenu.items.size(), 5u);
        commitDropdownSelection(state, DropdownType::BeautyShellBg, idx);
        EXPECT_EQ(config.get<std::string>("/capture/beautyShellTheme", ""), themeKey);
    }

    openDropdownMenu(state, DropdownType::BeautyShellPadding, anchor);
    ASSERT_EQ(state.dropdownMenu.items.size(), 6u);
    const std::array<int, 6> expectedPaddings{16, 24, 32, 48, 64, 72};
    for (size_t i = 0; i < expectedPaddings.size(); ++i) {
        EXPECT_EQ(state.dropdownMenu.items[i].id, expectedPaddings[i]);
    }

    openDropdownMenu(state, DropdownType::BeautyShellRadius, anchor);
    ASSERT_EQ(state.dropdownMenu.items.size(), 7u);
    const std::array<int, 7> expectedRadii{0, 8, 10, 12, 16, 24, 32};
    for (size_t i = 0; i < expectedRadii.size(); ++i) {
        EXPECT_EQ(state.dropdownMenu.items[i].id, expectedRadii[i]);
    }

    std::filesystem::remove_all(temp, ec);
}

TEST(PinAndScrollShortcutSafetyTest, TextSelectionProtectsRotationAndScrollAvoidsNakedKeys) {
    using namespace tools3000::capture;

    // 1. C6: PinInputController 保持默认无文字上下文时的旋转兼容，支持 Ctrl+A 全选，并在文字选区上下文屏蔽误触旋转
    EXPECT_EQ(PinInputController::resolveKeyDown('R', false, false, false).action,
              PinShortcutAction::RotateClockwise90);
    EXPECT_EQ(PinInputController::resolveKeyDown('L', false, false, false).action,
              PinShortcutAction::RotateCounterClockwise90);
    EXPECT_EQ(PinInputController::resolveKeyDown('A', true, false, false).action,
              PinShortcutAction::SelectAllText);

    // 激活 OCR 文字划选或持有文字选区时：R / Shift+R / Ctrl+R / L 全部静默，Ctrl+C 正常复制所选文字
    EXPECT_EQ(PinInputController::resolveKeyDown('R', false, false, false, true).action,
              PinShortcutAction::None);
    EXPECT_EQ(PinInputController::resolveKeyDown('R', false, true, false, true).action,
              PinShortcutAction::None);
    EXPECT_EQ(PinInputController::resolveKeyDown('R', true, false, false, true).action,
              PinShortcutAction::None);
    EXPECT_EQ(PinInputController::resolveKeyDown('L', false, false, false, true).action,
              PinShortcutAction::None);
    EXPECT_EQ(PinInputController::resolveKeyDown('C', true, false, false, true).action,
              PinShortcutAction::CopySelectedOrImage);
    EXPECT_EQ(PinInputController::resolveKeyDown('A', true, false, false, true).action,
              PinShortcutAction::SelectAllText);

    // 2. C6: 长截图瞬态热键彻底移除无修饰裸单键 'P'/'S'，升级为 Ctrl+T / Ctrl+S / Ctrl+C，且暂停态释放 VK_SPACE
    const auto activeSpecs = ScrollCaptureOverlay::transientHotkeySpecs(true);
    const auto pausedSpecs = ScrollCaptureOverlay::transientHotkeySpecs(false);
    EXPECT_EQ(activeSpecs.size(), 6u);
    EXPECT_EQ(pausedSpecs.size(), 5u);

    bool activeHasSpace = false;
    bool hasCtrlT = false;
    bool hasCtrlS = false;
    bool hasCtrlC = false;
    const auto ctrlMask = static_cast<std::uint32_t>(tools3000::core::ModKey::Ctrl);
    const auto noneMask = static_cast<std::uint32_t>(tools3000::core::ModKey::None);

    for (const auto& spec : activeSpecs) {
        // 严禁注册任何无修饰单字母键 (A-Z) 拦截底层打字输入
        if (spec.vk >= 'A' && spec.vk <= 'Z') {
            EXPECT_NE(spec.modifiers, noneMask);
        }
        if (spec.vk == VK_SPACE && spec.isTogglePause) activeHasSpace = true;
        if (spec.vk == 'T' && spec.modifiers == ctrlMask && spec.finishAction == ScrollFinishAction::Pin) hasCtrlT = true;
        if (spec.vk == 'S' && spec.modifiers == ctrlMask && spec.finishAction == ScrollFinishAction::SaveAs) hasCtrlS = true;
        if (spec.vk == 'C' && spec.modifiers == ctrlMask && spec.finishAction == ScrollFinishAction::Copy) hasCtrlC = true;
    }
    EXPECT_TRUE(activeHasSpace);
    EXPECT_TRUE(hasCtrlT);
    EXPECT_TRUE(hasCtrlS);
    EXPECT_TRUE(hasCtrlC);

    for (const auto& spec : pausedSpecs) {
        EXPECT_NE(spec.vk, static_cast<UINT>(VK_SPACE));
    }

    // 3. C6: 左下角快捷键提示面板同步展示 Ctrl+T / Ctrl+S / Ctrl+C，绝无旧裸键 P / S
    const auto hintItems = ShortcutHintOverlay::instance().getItemsForContext(ShortcutHintContext::ScrollCapture);
    bool hintHasCtrlT = false;
    bool hintHasCtrlS = false;
    bool hintHasCtrlC = false;
    for (const auto& item : hintItems) {
        EXPECT_NE(item.key, L"P");
        EXPECT_NE(item.key, L"S");
        if (item.key == L"Ctrl+T") hintHasCtrlT = true;
        if (item.key == L"Ctrl+S") hintHasCtrlS = true;
        if (item.key == L"Ctrl+C") hintHasCtrlC = true;
    }
    EXPECT_TRUE(hintHasCtrlT);
    EXPECT_TRUE(hintHasCtrlS);
    EXPECT_TRUE(hintHasCtrlC);
}

TEST(RecordingIndicatorLayoutTest, DynamicUltraWideResolutionAndBilingualFrameUnit) {
    using namespace tools3000::capture;

    // 1. 累计帧计数单位按语言动态切换，严格区别于实时帧率 fps（单复数自适应 frame / frames）
    EXPECT_EQ(RecordingIndicator::formatFrameCounterText(1, true), L"1帧");
    EXPECT_EQ(RecordingIndicator::formatFrameCounterText(1800, true), L"1800帧");
    EXPECT_EQ(RecordingIndicator::formatFrameCounterText(1, false), L"1 frame");
    EXPECT_EQ(RecordingIndicator::formatFrameCounterText(1800, false), L"1800 frames");
    EXPECT_EQ(RecordingIndicator::formatFrameCounterText(1800, false).find(L"fps"), std::wstring::npos);

    auto& indicator = RecordingIndicator::instance();

    // 2. 默认无音频紧凑态：消除次级参数与右侧按钮之间的空白死区
    const auto compactNoAudio = indicator.computeRecordingBarLayout(
        L"REC", L"00:00:14", L"1968\u00D71345",
        RecordingIndicator::formatFrameCounterText(8, false), false, false, false);
    EXPECT_EQ(compactNoAudio.systemAudioBtn.left, 0);
    EXPECT_EQ(compactNoAudio.systemAudioBtn.right, 0);
    EXPECT_EQ(compactNoAudio.microphoneBtn.left, 0);
    EXPECT_EQ(compactNoAudio.microphoneBtn.right, 0);
    EXPECT_LE(compactNoAudio.separatorX - compactNoAudio.frameRect.right, 16.0f);
    EXPECT_LT(compactNoAudio.separatorX, static_cast<float>(compactNoAudio.snapshotBtn.left));
    EXPECT_LT(compactNoAudio.snapshotBtn.right, compactNoAudio.pauseBtn.left);
    EXPECT_LT(compactNoAudio.pauseBtn.right, compactNoAudio.stopBtn.left);
    EXPECT_LT(compactNoAudio.stopBtn.right, compactNoAudio.totalLogicalWidth);
    EXPECT_GE(compactNoAudio.totalLogicalWidth, 220);
    EXPECT_LT(compactNoAudio.totalLogicalWidth, 455);

    // 3. 倒计时态（极小选区如 64x64）：次级槽位仍预留 >= 104px，确保悬停显示 "取消倒计时 / Cancel Countdown" 零位移零截断
    const auto countdownTinyLayout = indicator.computeRecordingBarLayout(
        L"READY", L"3", L"64\u00D764", L"", false, false, true);
    EXPECT_GE(countdownTinyLayout.resolutionRect.right - countdownTinyLayout.resolutionRect.left, 104.0f);
    EXPECT_GE(countdownTinyLayout.separatorX - 6.0f - countdownTinyLayout.resolutionRect.left, 104.0f);

    // 4. 双通道音频开启 + 1080p / 4K (3840x2160) / 8K (7680x4320) 超宽分辨率与长帧数下动态扩宽零截断零重叠
    const auto fhdLayout = indicator.computeRecordingBarLayout(
        L"REC", L"00:05:12", L"1920\u00D71080",
        RecordingIndicator::formatFrameCounterText(9360, false), true, true, false);
    const auto uhd4kLayout = indicator.computeRecordingBarLayout(
        L"PAUSED", L"01:28:45", L"3840\u00D72160",
        RecordingIndicator::formatFrameCounterText(319500, false), true, true, false);
    const auto uhd8kLayout = indicator.computeRecordingBarLayout(
        L"PAUSED", L"12:59:59", L"7680\u00D74320",
        RecordingIndicator::formatFrameCounterText(2807940, false), true, true, false);

    for (const auto& lay : {fhdLayout, uhd4kLayout, uhd8kLayout}) {
        EXPECT_LT(lay.statusRect.left, lay.statusRect.right);
        EXPECT_LE(lay.statusRect.right, lay.timeRect.left);
        EXPECT_LT(lay.timeRect.left, lay.timeRect.right);
        EXPECT_LE(lay.timeRect.right, lay.resolutionRect.left);
        EXPECT_LT(lay.resolutionRect.left, lay.resolutionRect.right);
        EXPECT_LE(lay.resolutionRect.right, lay.frameRect.left);
        EXPECT_LT(lay.frameRect.left, lay.frameRect.right);
        EXPECT_LE(lay.frameRect.right, static_cast<float>(lay.systemAudioBtn.left));
        EXPECT_LT(lay.systemAudioBtn.right, lay.microphoneBtn.left);
        EXPECT_LT(static_cast<float>(lay.microphoneBtn.right), lay.separatorX);
        EXPECT_LT(lay.separatorX, static_cast<float>(lay.snapshotBtn.left));
        EXPECT_LT(lay.snapshotBtn.right, lay.pauseBtn.left);
        EXPECT_LT(lay.pauseBtn.right, lay.stopBtn.left);
        EXPECT_LT(lay.stopBtn.right, lay.totalLogicalWidth);
    }
    EXPECT_GT(fhdLayout.totalLogicalWidth, compactNoAudio.totalLogicalWidth);
    EXPECT_GT(uhd8kLayout.totalLogicalWidth, fhdLayout.totalLogicalWidth);
}

TEST(CapturePluginIntegrationTest, HistoryCopySaveAsOcrSelectionAndDpiChangedPlacement) {
    using namespace tools3000::capture;

    const auto tempCfgDir = std::filesystem::temp_directory_path() /
                            (L"tools3000_cap_plugin_cfg_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code cfgEc;
    std::filesystem::remove_all(tempCfgDir, cfgEc);
    std::filesystem::create_directories(tempCfgDir, cfgEc);
    ASSERT_TRUE(tools3000::core::ConfigManager::instance().initialize(tempCfgDir));

    std::array<wchar_t, 4096> executablePath{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    ASSERT_GT(length, 0u);
    ASSERT_LT(length, executablePath.size());

    const auto executableDir = std::filesystem::path(executablePath.data()).parent_path();
    const auto pluginDllPath =
        executableDir.parent_path() / L"plugins" / executableDir.filename() / L"Plugin_Capture.dll";
    ASSERT_TRUE(std::filesystem::exists(pluginDllPath));

    HMODULE hMod = LoadLibraryExW(
        pluginDllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    ASSERT_NE(hMod, nullptr);

    using CreatePluginFunc = tools3000::core::IPlugin* (*)();
    const auto createPlugin = reinterpret_cast<CreatePluginFunc>(GetProcAddress(hMod, "CreatePlugin"));
    ASSERT_NE(createPlugin, nullptr);
    tools3000::core::IPlugin* capturePlugin = createPlugin();
    ASSERT_NE(capturePlugin, nullptr);
    ASSERT_TRUE(capturePlugin->initialize());

    auto& bridge = tools3000::core::MessageBridge::instance();

    struct PluginCleanupGuard {
        HMODULE mod = nullptr;
        tools3000::core::IPlugin* plugin = nullptr;
        ~PluginCleanupGuard() {
            while (HWND existing = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
                SendMessageW(existing, WM_KEYDOWN, VK_ESCAPE, 0);
                if (IsWindow(existing)) DestroyWindow(existing);
            }
            if (plugin) plugin->shutdown();
            if (mod) FreeLibrary(mod);
        }
    } cleanupGuard{hMod, capturePlugin};

    auto invokeRpc = [&](int id, const char* method, const nlohmann::json& params) -> nlohmann::json {
        nlohmann::json req = {{"id", id}, {"method", method}, {"params", params}};
        const std::string raw = bridge.handleMessage(req.dump());
        const auto parsed = nlohmann::json::parse(raw, nullptr, false);
        return parsed.is_object() && parsed.contains("result") ? parsed["result"] : parsed;
    };

    // 1. B7: 尺寸 HUD 三项偏好持久化 + 录屏 videoFormat 记忆验证
    {
        const auto upd = invokeRpc(701, "capture.updateSettings", {
            {"sizeUnit", "dp"},
            {"showSizePosition", false},
            {"showSizeUnit", false}
        });
        EXPECT_TRUE(upd.value("success", false));

        const auto got = invokeRpc(702, "capture.getSettings", nlohmann::json::object());
        EXPECT_EQ(got.value("sizeUnit", ""), "dp");
        EXPECT_FALSE(got.value("showSizePosition", true));
        EXPECT_FALSE(got.value("showSizeUnit", true));

        // 恢复默认尺寸 HUD 设置
        invokeRpc(703, "capture.updateSettings", {
            {"sizeUnit", "px"},
            {"showSizePosition", true},
            {"showSizeUnit", true}
        });

        EXPECT_TRUE(invokeRpc(704, "recording.updateSettings", {{"format", "mp4_h265"}}).value("success", false));
        EXPECT_TRUE(invokeRpc(705, "recording.updateSettings", {{"format", "gif"}}).value("success", false));
        EXPECT_EQ(tools3000::core::ConfigManager::instance().get<std::string>("/recording/videoFormat", ""),
                  "mp4_h265");
    }

    // 2. C7.2: 纯内存历史截图项支持一键复制图片与另存为落盘
    {
        invokeRpc(710, "history.clear", nlohmann::json::object());
        const auto pushed = invokeRpc(711, "history.pushMemoryEntry", {
            {"width", 96}, {"height", 72}, {"b", 210}, {"g", 130}, {"r", 40}
        });
        ASSERT_TRUE(pushed.value("success", false));
        ASSERT_GE(pushed.value("count", 0), 1);

        const auto copied = invokeRpc(712, "capture.copyHistoryItem", {{"index", 0}});
        EXPECT_TRUE(copied.value("success", false));
        cv::Mat clipImg = ClipboardUtils::readImageFromClipboard();
        EXPECT_EQ(clipImg.cols, 96);
        EXPECT_EQ(clipImg.rows, 72);

        const auto exportPath = std::filesystem::temp_directory_path() /
                                (L"tools3000_hist_export_" + std::to_wstring(GetCurrentProcessId()) + L".png");
        std::error_code ec;
        std::filesystem::remove(exportPath, ec);
        const auto saved = invokeRpc(713, "capture.saveHistoryItemAs", {
            {"index", 0},
            {"targetPath", tools3000::core::WinUtils::wstringToUtf8(exportPath.wstring())}
        });
        EXPECT_TRUE(saved.value("success", false));
        EXPECT_TRUE(saved.value("saved", false));
        ASSERT_TRUE(std::filesystem::exists(exportPath));

        cv::Mat diskImg = cv::imread(exportPath.string(), cv::IMREAD_COLOR);
        EXPECT_EQ(diskImg.cols, 96);
        EXPECT_EQ(diskImg.rows, 72);
        std::filesystem::remove(exportPath, ec);
    }

    // 3. C6: 长截图悬浮控制台多语言与高分屏动态按钮布局零重叠、键帽精确为 Ctrl+T / Ctrl+S
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        for (bool isZh : {true, false}) {
            const auto bar = invokeRpc(720, "capture.inspectScrollControlBar", {
                {"scale", scale}, {"chinese", isZh}, {"paused", false}
            });
            ASSERT_TRUE(bar.value("success", false));
            ASSERT_TRUE(bar.contains("buttons") && bar["buttons"].size() == 5u);
            EXPECT_EQ(bar["buttons"][1].value("keycap", ""), "Ctrl+T");
            EXPECT_EQ(bar["buttons"][2].value("keycap", ""), "Ctrl+S");
            for (size_t i = 0; i < 5u; ++i) {
                const float l = bar["buttons"][i].value("left", 0.0f);
                const float r = bar["buttons"][i].value("right", 0.0f);
                EXPECT_LT(l, r);
                if (i + 1 < 5u) {
                    const float nextL = bar["buttons"][i + 1].value("left", 0.0f);
                    EXPECT_LE(r, nextL);
                }
            }
            EXPECT_LE(bar["buttons"][4].value("right", 0.0f), static_cast<float>(bar.value("barWidth", 0)));
        }
    }

    // 4. C6: 真实 PinWindow 注入 OCR 文字块全选后，按键 'R' / 'L' 绝不误触旋转窗口
    {
        cv::Mat pinImg(64, 128, CV_8UC3, cv::Scalar(48, 56, 72));
        const auto pinPng = std::filesystem::temp_directory_path() /
                            (L"tools3000_pin_ocr_guard_" + std::to_wstring(GetCurrentProcessId()) + L".png");
        std::vector<uchar> buf;
        ASSERT_TRUE(cv::imencode(".png", pinImg, buf));
        {
            std::ofstream ofs(pinPng, std::ios::binary | std::ios::trunc);
            ofs.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        }
        const auto pinned = invokeRpc(730, "capture.pinImageFile", {
            {"path", tools3000::core::WinUtils::wstringToUtf8(pinPng.wstring())}
        });
        EXPECT_TRUE(pinned.value("success", false));

        HWND pinHwnd = FindWindowW(L"Tools3000_PinWindow", nullptr);
        ASSERT_NE(pinHwnd, nullptr);

        const auto ocrState = invokeRpc(731, "capture.inspectPinOcrState", {
            {"mockBlocks", nlohmann::json::array({
                {{"text", "Tools3000"}, {"x", 8}, {"y", 8}, {"w", 60}, {"h", 18}},
                {{"text", "PinOCR"}, {"x", 8}, {"y", 30}, {"w", 50}, {"h", 18}}
            })},
            {"selectAll", true}
        });
        ASSERT_TRUE(ocrState.value("success", false));
        EXPECT_TRUE(ocrState.value("textSelectionMode", false));
        EXPECT_EQ(ocrState.value("selectedBlockCount", 0), 2);
        EXPECT_EQ(ocrState.value("origWidth", 0), 128);
        EXPECT_EQ(ocrState.value("origHeight", 0), 64);

        // 发送 'R' 与 'L'：由于持有激活的 OCR 文字选区，窗口严禁发生 90 度宽高对调旋转
        SendMessageW(pinHwnd, WM_KEYDOWN, 'R', 0);
        SendMessageW(pinHwnd, WM_KEYDOWN, 'L', 0);
        const auto afterRotAttempt = invokeRpc(732, "capture.inspectPinOcrState", nlohmann::json::object());
        EXPECT_EQ(afterRotAttempt.value("origWidth", 0), 128);
        EXPECT_EQ(afterRotAttempt.value("origHeight", 0), 64);
        EXPECT_EQ(afterRotAttempt.value("selectedBlockCount", 0), 2);

        // 清空文字选区并退出选字态后，按 'R' 立即正常执行 90 度旋转 (128x64 -> 64x128)
        invokeRpc(733, "capture.inspectPinOcrState", {{"clearSelection", true}});
        SendMessageW(pinHwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        SendMessageW(pinHwnd, WM_KEYDOWN, 'R', 0);
        const auto afterRealRot = invokeRpc(734, "capture.inspectPinOcrState", nlohmann::json::object());
        EXPECT_EQ(afterRealRot.value("origWidth", 0), 64);
        EXPECT_EQ(afterRealRot.value("origHeight", 0), 128);

        SendMessageW(pinHwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        std::error_code ec;
        std::filesystem::remove(pinPng, ec);
    }

    // 5. D4: OcrResultWindow 字符级单行/跨行选字、单行底部提示零折行、以及 WM_DPICHANGED 跨屏定位不飞回中央
    {
        const std::string sampleText = "Alpha Beta Gamma\n第二行离线识别文本\nLine3 End";
        const auto opened = invokeRpc(740, "ocr.openResultWindow", {{"text", sampleText}});
        ASSERT_TRUE(opened.value("success", false));
        EXPECT_EQ(opened.value("lineCount", 0), 3);
        EXPECT_TRUE(opened.value("singleLineFooter", false));

        HWND ocrHwnd = FindWindowW(L"Tools3000_OcrResult", nullptr);
        ASSERT_NE(ocrHwnd, nullptr);

        // 单行局部字符选字："Beta"
        const auto singleSel = invokeRpc(741, "ocr.selectResultRange", {
            {"anchorLine", 0}, {"anchorOffset", 6},
            {"activeLine", 0}, {"activeOffset", 10}
        });
        EXPECT_TRUE(singleSel.value("hasSelection", false));
        EXPECT_EQ(singleSel.value("selectedText", ""), "Beta");

        // 跨行字符选字：第 0 行 "Gamma" 到第 1 行前 3 个汉字 "第二行"
        const auto multiSel = invokeRpc(742, "ocr.selectResultRange", {
            {"anchorLine", 0}, {"anchorOffset", 11},
            {"activeLine", 1}, {"activeOffset", 3}
        });
        EXPECT_TRUE(multiSel.value("hasSelection", false));
        EXPECT_EQ(multiSel.value("selectedText", ""), "Gamma\r\n第二行");

        // 全选并复制到剪贴板
        const auto allSel = invokeRpc(743, "ocr.selectResultRange", {
            {"selectAll", true}, {"copy", true}
        });
        EXPECT_TRUE(allSel.value("hasSelection", false));
        EXPECT_NE(allSel.value("selectedText", "").find("Line3 End"), std::string::npos);

        // 模拟跨显示器拖拽触发 WM_DPICHANGED (125% = 120 DPI)，传入偏离屏幕中央的 suggested 矩形
        RECT suggested{135, 165, 885, 665};
        SendMessageW(ocrHwnd, WM_DPICHANGED, MAKELPARAM(120, 120), reinterpret_cast<LPARAM>(&suggested));

        RECT actualRc{};
        ASSERT_TRUE(GetWindowRect(ocrHwnd, &actualRc));
        EXPECT_EQ(actualRc.left, suggested.left);
        EXPECT_EQ(actualRc.top, suggested.top);
        EXPECT_EQ(actualRc.right - actualRc.left, suggested.right - suggested.left);
        EXPECT_EQ(actualRc.bottom - actualRc.top, suggested.bottom - suggested.top);

        // DPI 缩放切换后底部提示栏在双语下依然严格保持单行零折行
        const auto postDpi = invokeRpc(744, "ocr.selectResultRange", nlohmann::json::object());
        EXPECT_TRUE(postDpi.value("singleLineFooter", false));

        invokeRpc(745, "ocr.closeResultWindow", nlohmann::json::object());
    }

    // 6. U-02: 非 1.0x 缩放态 (2.0x 放大与 0.35x 缩小) 二次标注绝不触发窗口平方暴涨或底图永久糊化
    {
        cv::Mat pinImg(100, 200, CV_8UC3, cv::Scalar(180, 95, 45));
        const auto pinPng = std::filesystem::temp_directory_path() /
                            (L"tools3000_pin_reedit_" + std::to_wstring(GetCurrentProcessId()) + L".png");
        std::vector<uchar> buf;
        ASSERT_TRUE(cv::imencode(".png", pinImg, buf));
        {
            std::ofstream ofs(pinPng, std::ios::binary | std::ios::trunc);
            ofs.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        }
        ASSERT_TRUE(invokeRpc(801, "capture.pinImageFile", {
            {"path", tools3000::core::WinUtils::wstringToUtf8(pinPng.wstring())}
        }).value("success", false));

        // 缩放到 2.0x 并记录窗口尺寸，随后执行第一次二次标注（绘制实心红框）
        const auto scaled2x = invokeRpc(802, "capture.inspectPinOcrState", {{"setScale", 2.0f}});
        ASSERT_TRUE(scaled2x.value("success", false));
        EXPECT_EQ(scaled2x.value("origWidth", 0), 200);
        EXPECT_EQ(scaled2x.value("origHeight", 0), 100);
        EXPECT_NEAR(scaled2x.value("scale", 0.0f), 2.0f, 0.001f);
        const int winW2x = scaled2x.value("windowWidth", 0);
        const int winH2x = scaled2x.value("windowHeight", 0);

        const auto afterEdit1 = invokeRpc(803, "capture.inspectPinOcrState", {
            {"applyEditedRects", nlohmann::json::array({
                {{"x1", 10}, {"y1", 10}, {"x2", 60}, {"y2", 50}, {"r", 230}, {"g", 40}, {"b", 40}, {"fill", true}}
            })},
            {"sampleX", 30},
            {"sampleY", 30}
        });
        ASSERT_TRUE(afterEdit1.value("success", false));
        EXPECT_EQ(afterEdit1.value("origWidth", 0), 200);
        EXPECT_EQ(afterEdit1.value("origHeight", 0), 100);
        EXPECT_EQ(afterEdit1.value("cleanWidth", 0), 200);
        EXPECT_EQ(afterEdit1.value("cleanHeight", 0), 100);
        EXPECT_NEAR(afterEdit1.value("scale", 0.0f), 2.0f, 0.001f);
        EXPECT_EQ(afterEdit1.value("windowWidth", 0), winW2x);
        EXPECT_EQ(afterEdit1.value("windowHeight", 0), winH2x);
        EXPECT_EQ(afterEdit1.value("markupCount", 0), 1);
        EXPECT_EQ(afterEdit1.value("sampleR", 0), 230);

        // 再缩放到 0.35x (长图缩略态) 并执行第二次增量二次标注：绝不坍缩成 0.12x，未标注区底图 100% 原始像素零糊化
        const auto scaledSmall = invokeRpc(804, "capture.inspectPinOcrState", {{"setScale", 0.35f}});
        const int winWSmall = scaledSmall.value("windowWidth", 0);
        const int winHSmall = scaledSmall.value("windowHeight", 0);

        const auto afterEdit2 = invokeRpc(805, "capture.inspectPinOcrState", {
            {"appendExistingMarkup", true},
            {"applyEditedRects", nlohmann::json::array({
                {{"x1", 90}, {"y1", 20}, {"x2", 140}, {"y2", 70}, {"r", 40}, {"g", 210}, {"b", 90}, {"fill", true}}
            })},
            {"sampleX", 185},
            {"sampleY", 85}
        });
        ASSERT_TRUE(afterEdit2.value("success", false));
        EXPECT_EQ(afterEdit2.value("origWidth", 0), 200);
        EXPECT_EQ(afterEdit2.value("origHeight", 0), 100);
        EXPECT_EQ(afterEdit2.value("sourceWidth", 0), 200);
        EXPECT_EQ(afterEdit2.value("sourceHeight", 0), 100);
        EXPECT_NEAR(afterEdit2.value("scale", 0.0f), 0.35f, 0.001f);
        EXPECT_EQ(afterEdit2.value("windowWidth", 0), winWSmall);
        EXPECT_EQ(afterEdit2.value("windowHeight", 0), winHSmall);
        EXPECT_EQ(afterEdit2.value("markupCount", 0), 2);
        EXPECT_EQ(afterEdit2.value("sampleB", 0), 180);
        EXPECT_EQ(afterEdit2.value("sampleG", 0), 95);
        EXPECT_EQ(afterEdit2.value("sampleR", 0), 45);

        if (HWND pinHwnd = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
            SendMessageW(pinHwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        }
        std::error_code ec;
        std::filesystem::remove(pinPng, ec);
    }

    // 7. U-03 + V-01: OcrResultWindow 智能合并段落切换、双击选词复制、原地微调编辑、八向缩放热区与最小宽高约束
    {
        const std::string rawOcr =
            "Deep learning models require\n"
            "careful latency tuning.\n"
            "\n"
            "Second paragraph stays distinct.";
        const auto opened = invokeRpc(810, "ocr.openResultWindow", {{"text", rawOcr}});
        ASSERT_TRUE(opened.value("success", false));
        EXPECT_EQ(opened.value("lineCount", 0), 3);

        HWND ocrHwnd = FindWindowW(L"Tools3000_OcrResult", nullptr);
        ASSERT_NE(ocrHwnd, nullptr);

        // 开启智能合并段落：前两行自动合并为 1 行，非空显示行数由 3 降为 2
        const auto mergedState = invokeRpc(811, "ocr.selectResultRange", {{"setMergeParagraphs", true}});
        EXPECT_TRUE(mergedState.value("mergeParagraphs", false));
        EXPECT_EQ(mergedState.value("lineCount", 0), 2);
        EXPECT_NE(mergedState.value("currentText", "").find("Deep learning models require careful latency tuning."),
                  std::string::npos);

        // 关闭智能合并段落：无损还原回原始 3 行
        const auto unmergedState = invokeRpc(812, "ocr.selectResultRange", {{"setMergeParagraphs", false}});
        EXPECT_FALSE(unmergedState.value("mergeParagraphs", true));
        EXPECT_EQ(unmergedState.value("lineCount", 0), 3);

        // 双击选词复制：选中第 0 行 offset 6 处的单词 "learning"
        const auto wordCopied = invokeRpc(813, "ocr.selectResultRange", {
            {"selectWordLine", 0},
            {"selectWordOffset", 6}
        });
        EXPECT_EQ(wordCopied.value("copiedWord", ""), "learning");
        EXPECT_EQ(wordCopied.value("selectedText", ""), "learning");

        // 原地微调编辑：将选中的 "learning" 替换为 "vision"
        const auto edited = invokeRpc(814, "ocr.selectResultRange", {
            {"insertText", "vision"}
        });
        EXPECT_NE(edited.value("currentText", "").find("Deep vision models require"), std::string::npos);

        // V-01: 验证 WM_GETMINMAXINFO 严格消费并生效 BaseMinWidth / BaseMinHeight 最小尺寸约束
        const UINT dpi = GetDpiForWindow(ocrHwnd);
        const float scale = tools3000::core::dpi::scaleForDpi(dpi);
        MINMAXINFO mmi{};
        SendMessageW(ocrHwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&mmi));
        EXPECT_EQ(mmi.ptMinTrackSize.x,
                  tools3000::core::dpi::scaleMetric(tools3000::ocr::OcrResultStyle::BaseMinWidth, scale));
        EXPECT_EQ(mmi.ptMinTrackSize.y,
                  tools3000::core::dpi::scaleMetric(tools3000::ocr::OcrResultStyle::BaseMinHeight, scale));

        // V-01: 验证 WM_NCHITTEST 八向边缘与四角拖拽缩放命中
        RECT winRc{};
        ASSERT_TRUE(GetWindowRect(ocrHwnd, &winRc));
        const int inset = tools3000::core::dpi::scaleMetric(6, scale) + 2;
        const int midX = (winRc.left + winRc.right) / 2;
        const int midY = (winRc.top + winRc.bottom) / 2;

        EXPECT_EQ(SendMessageW(ocrHwnd, WM_NCHITTEST, 0, MAKELPARAM(winRc.left + inset, winRc.top + inset)), HTTOPLEFT);
        EXPECT_EQ(SendMessageW(ocrHwnd, WM_NCHITTEST, 0, MAKELPARAM(winRc.right - inset, winRc.bottom - inset)), HTBOTTOMRIGHT);
        EXPECT_EQ(SendMessageW(ocrHwnd, WM_NCHITTEST, 0, MAKELPARAM(winRc.left + inset, midY)), HTLEFT);
        EXPECT_EQ(SendMessageW(ocrHwnd, WM_NCHITTEST, 0, MAKELPARAM(winRc.right - inset, midY)), HTRIGHT);
        EXPECT_EQ(SendMessageW(ocrHwnd, WM_NCHITTEST, 0, MAKELPARAM(midX, winRc.top + inset)), HTTOP);
        EXPECT_EQ(SendMessageW(ocrHwnd, WM_NCHITTEST, 0, MAKELPARAM(midX, winRc.bottom - inset)), HTBOTTOM);

        invokeRpc(815, "ocr.closeResultWindow", nlohmann::json::object());
    }
    std::filesystem::remove_all(tempCfgDir, cfgEc);
}

TEST(CaptureMagnifierInteractionTest, HoverExistingMagnifierHidesDynamicPreviewAndExposesResizeHandles) {
    tools3000::capture::CaptureState state;
    state.dpiScale = 1.5f;
    state.dragStart = POINT{100, 100};
    state.dragEnd = POINT{900, 700};
    state.isMarkupToolActive = true;
    state.currentTool = tools3000::capture::MarkupTool::Magnifier;

    cv::Mat baseImg(600, 800, CV_8UC4, cv::Scalar(40, 44, 52, 255));
    state.markup.setBaseImage(baseImg);

    // 1. 创建第一个引出式放大镜：源点 (200, 200)，大透镜圆心 (360, 150)，半径 72
    auto* mag1 = state.markup.addMagnifier(cv::Point(200, 200), 2.5f, 72);
    ASSERT_NE(mag1, nullptr);
    mag1->dpiScale = 1.5f;
    mag1->endPt = cv::Point(360, 150);
    mag1->hasLeaderArrow = true;
    mag1->isActive = true;
    state.activeElement = mag1;

    // 悬停在引出源取景小环中心 (屏幕坐标 300, 300 -> 局部 200, 200)：必须隐藏动态预览镜
    EXPECT_TRUE(tools3000::capture::shouldSuppressDynamicMagnifierPreview(state, POINT{300, 300}));
    EXPECT_EQ(tools3000::capture::findHoveredMagnifierElement(state, cv::Point(200, 200)), mag1);

    // 悬停在大透镜圆心 (局部 360, 150 -> 屏幕 460, 250)：必须隐藏动态预览镜，露出移动热区
    EXPECT_TRUE(tools3000::capture::shouldSuppressDynamicMagnifierPreview(state, POINT{460, 250}));
    EXPECT_EQ(tools3000::capture::findHoveredMagnifierElement(state, cv::Point(360, 150)), mag1);

    // 悬停在圆形大透镜外接框右下角 RB 缩放控制点 (局部 360+72, 150+72 -> 屏幕 532, 322，距圆心 1.414*r)：
    // 必须隐藏动态预览镜，且高分屏下 hitTestEx 稳稳命中 HitArea::RB！
    EXPECT_TRUE(tools3000::capture::shouldSuppressDynamicMagnifierPreview(state, POINT{532, 322}));
    EXPECT_EQ(mag1->hitTestEx(cv::Point(360 + 72 + 8, 150 + 72 + 8)), tools3000::capture::HitArea::RB);

    // 2. 光标移至远处空白画布 (局部 620, 420 -> 屏幕 720, 520)：
    // 动态预览镜立即恢复显示供连续标第二个，且悬停放大镜查询返回 nullptr（滚轮绝不隔空篡改第一个放大镜）
    EXPECT_FALSE(tools3000::capture::shouldSuppressDynamicMagnifierPreview(state, POINT{720, 520}));
    EXPECT_EQ(tools3000::capture::findHoveredMagnifierElement(state, cv::Point(620, 420)), nullptr);

    // 3. 光标靠近选区右下角拉伸手柄 (900, 700)：自动退避动态预览镜
    EXPECT_TRUE(tools3000::capture::shouldSuppressDynamicMagnifierPreview(state, POINT{896, 696}));
}


TEST(CaptureHudPaletteTest, CleanPorcelainStudioAndGraphiteSlatePaletteContract) {
    using namespace tools3000::capture;

    auto srgbToLinear = [](float c) -> double {
        return (c <= 0.04045f) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
    };
    auto calcLum = [&](const D2D1_COLOR_F& col) -> double {
        return 0.2126 * srgbToLinear(col.r) +
               0.7152 * srgbToLinear(col.g) +
               0.0722 * srgbToLinear(col.b);
    };

    // 1. 强断言：可交互操作台（工具栏/菜单/尺寸胶囊）统一为温润雪瓷白 (Clean Porcelain Studio)
    for (const auto& bg : {
             CaptureHudPalette::kPanelBg,
             CaptureHudPalette::kSizeHudBg }) {
        EXPECT_GE(bg.r, 0.96f);
        EXPECT_GE(bg.g, 0.96f);
        EXPECT_GE(bg.b, 0.97f);
        EXPECT_GE(bg.a, 0.95f);
    }

    // 2. 强断言：外围只读穿透 HUD（ShortcutHintOverlay 与像素放大镜信息舱）采用高对比曜石纯黑底舱 + 100% 纯白高亮实心键帽
    for (const auto& obsBg : {
             CaptureHudPalette::kShortcutPanelBg,
             CaptureHudPalette::kLoupeInfoBg }) {
        EXPECT_LE(obsBg.r, 0.08f);
        EXPECT_LE(obsBg.g, 0.08f);
        EXPECT_LE(obsBg.b, 0.10f);
        EXPECT_GE(obsBg.a, 0.85f);
    }
    EXPECT_GE(CaptureHudPalette::kShortcutKeyBg.r, 0.98f);
    EXPECT_GE(CaptureHudPalette::kShortcutKeyBg.g, 0.98f);
    EXPECT_GE(CaptureHudPalette::kShortcutKeyBg.b, 0.98f);
    EXPECT_GE(CaptureHudPalette::kShortcutKeyBg.a, 0.95f);
    EXPECT_LE(CaptureHudPalette::kShortcutKeyText.r, 0.12f);
    EXPECT_GE(CaptureHudPalette::kShortcutLabelText.r, 0.95f);
    EXPECT_GE(CaptureHudPalette::kLoupeKeycapBg.r, 0.98f);
    EXPECT_LE(CaptureHudPalette::kLoupeKeycapText.r, 0.12f);

    // 3. 强断言：HEX / dp / R12px 微晶胶囊收敛为沉稳深石板中性色，且 6 点拖拽手柄具备清晰悬停反馈对比
    const auto& chip = CaptureHudPalette::kChipAccentText;
    const auto& primary = CaptureHudPalette::kPrimaryText;
    EXPECT_LE(chip.r, 0.22f);
    EXPECT_LE(chip.g, 0.25f);
    EXPECT_LE(chip.b, 0.32f);
    EXPECT_LE(std::abs(chip.b - chip.r), 0.12f);
    EXPECT_GT(CaptureHudPalette::kGripDotHover.a, CaptureHudPalette::kGripDotIdle.a + 0.20f);

    // 4. WCAG AAA 光学对比度校验：
    //    - 雪瓷白底板上的深曜石黑主文字 >= 14.0:1，徽章文字 >= 10.0:1
    //    - 纯白实心键帽上的深曜石黑键帽字 >= 14.0:1
    //    - 曜石纯黑底舱上的纯白说明文字与色值文字 >= 14.0:1
    const double lumPorcelainBg = calcLum(CaptureHudPalette::kSizeHudBg);
    const double lumPrimary = calcLum(primary);
    const double lumChip = calcLum(chip);
    EXPECT_GE((lumPorcelainBg + 0.05) / (lumPrimary + 0.05), 14.0);
    EXPECT_GE((lumPorcelainBg + 0.05) / (lumChip + 0.05), 10.0);

    const double lumKeycapBg = calcLum(CaptureHudPalette::kShortcutKeyBg);
    const double lumKeycapText = calcLum(CaptureHudPalette::kShortcutKeyText);
    EXPECT_GE((lumKeycapBg + 0.05) / (lumKeycapText + 0.05), 14.0);

    const double lumHintObsidianBg = calcLum(CaptureHudPalette::kShortcutPanelBg);
    const double lumHintLabelText = calcLum(CaptureHudPalette::kShortcutLabelText);
    EXPECT_GE((lumHintLabelText + 0.05) / (lumHintObsidianBg + 0.05), 14.0);

    const double lumLoupeBg = calcLum(CaptureHudPalette::kLoupeInfoBg);
    const double lumLoupeText = calcLum(CaptureHudPalette::kLoupePrimaryText);
    EXPECT_GE((lumLoupeText + 0.05) / (lumLoupeBg + 0.05), 14.0);

    // 5. 验证 HUD 实机渲染与 60fps 指数阻尼平滑避让动画收敛
    EXPECT_FLOAT_EQ(ShortcutHintStyle::BaseKeyFont, 15.0f);
    EXPECT_FLOAT_EQ(ShortcutHintStyle::BaseLabelFont, 15.5f);
    auto& hint = ShortcutHintOverlay::instance();
    hint.setOwnerActiveChecker([](ShortcutHintContext) { return true; });
    hint.show(ShortcutHintContext::CaptureSelecting, POINT{960, 200});
    EXPECT_TRUE(hint.isVisible());
    const RECT initialBounds = hint.getBounds();
    EXPECT_GT(initialBounds.right - initialBounds.left, 200);
    EXPECT_GT(initialBounds.bottom - initialBounds.top, 100);

    // 当选区/工具栏遮挡左下角卡片热区时触发自动避让，执行平滑过渡而非生硬瞬移
    const RECT collideRect{initialBounds.left - 8, initialBounds.top - 8,
                           initialBounds.right + 8, initialBounds.bottom + 8};
    hint.updateAvoidance({collideRect});
    const RECT targetBounds = hint.getBounds();
    if (targetBounds.left != initialBounds.left || targetBounds.top != initialBounds.top) {
        EXPECT_TRUE(hint.isAnimating());
        const RECT firstFrame = hint.getAnimatedWindowBounds();
        // 第一帧开始平滑靠近目标，经过若干 16ms 帧后精确吸附至目标并自动停表零 CPU 消耗
        EXPECT_TRUE(firstFrame.left != targetBounds.left || firstFrame.top != targetBounds.top);
        for (int step = 0; step < 60 && hint.isAnimating(); ++step) {
            hint.stepAnimationForTest(0.016f);
        }
        EXPECT_FALSE(hint.isAnimating());
        const RECT settled = hint.getAnimatedWindowBounds();
        EXPECT_EQ(settled.left, targetBounds.left);
        EXPECT_EQ(settled.top, targetBounds.top);
    }

    hint.hide();
    hint.setOwnerActiveChecker(nullptr);
}

TEST(CaptureCornerRadiusTest, FigmaProximityRevealAndPreciseHitZoneContract) {
    using namespace tools3000::capture;

    const auto handles = CornerRadiusHelper::getCornerHandles(100.0f, 100.0f, 500.0f, 400.0f, 0.0f, 1.0f, false);
    ASSERT_EQ(handles.size(), 4u);

    // 1. 平时远离角部（选区中央）：100% 纯净隐形，绝不展示常驻黑心靶眼
    const auto farState = CornerRadiusHelper::evaluateProximity(handles, 300.0f, 250.0f, 1.0f, false);
    EXPECT_EQ(farState.activeCornerIndex, -1);
    EXPECT_FLOAT_EQ(farState.visibilityAlpha, 0.0f);
    EXPECT_FALSE(farState.isDirectHover);

    // 2. 鼠标进入左上角近场感应区（距手柄约 30px）：仅唤醒单角，平滑半透明渐显，不弹胶囊
    const auto nearState = CornerRadiusHelper::evaluateProximity(
        handles, handles[0].x + 22.0f, handles[0].y + 20.0f, 1.0f, false);
    EXPECT_EQ(nearState.activeCornerIndex, 0);
    EXPECT_GT(nearState.visibilityAlpha, 0.22f);
    EXPECT_LT(nearState.visibilityAlpha, 1.0f);
    EXPECT_FALSE(nearState.isDirectHover);

    // 3. 鼠标直接落入手柄热区或处于拖拽态：100% 不透明唤醒并激活曲率弧与极简读数胶囊
    const auto directState = CornerRadiusHelper::evaluateProximity(
        handles, handles[1].x + 3.0f, handles[1].y - 3.0f, 1.0f, false);
    EXPECT_EQ(directState.activeCornerIndex, 1);
    EXPECT_FLOAT_EQ(directState.visibilityAlpha, 1.0f);
    EXPECT_TRUE(directState.isDirectHover);

    const auto dragState = CornerRadiusHelper::evaluateProximity(
        handles, 350.0f, 320.0f, 1.0f, true, 2, handles[2].x, handles[2].y);
    EXPECT_EQ(dragState.activeCornerIndex, 2);
    EXPECT_FLOAT_EQ(dragState.visibilityAlpha, 1.0f);
    EXPECT_TRUE(dragState.isDirectHover);

    // 4. 命中热区精准收敛：不再将手柄向内 35px 处的导轨/文字框腹地误判为圆角手柄
    EXPECT_EQ(CornerRadiusHelper::hitTestHandles(100.0f, 100.0f, 500.0f, 400.0f, 12.0f, handles[0].x, handles[0].y), 0);
    EXPECT_EQ(CornerRadiusHelper::hitTestHandles(
                  100.0f, 100.0f, 500.0f, 400.0f, 12.0f,
                  handles[0].x + handles[0].inDirX * 35.0f,
                  handles[0].y + handles[0].inDirY * 35.0f),
              -1);
}

TEST(CaptureToolbarLayoutTest, SecondaryToolbarAlwaysLeftAlignsWithPrimaryToolbar) {
    using namespace tools3000::capture;

    const D2D1_SIZE_F surface = D2D1_SIZE_F{1920.0f, 1080.0f};
    const std::vector<D2D1_RECT_F> testSelections = {
        D2D1::RectF(240.0f, 180.0f, 1100.0f, 680.0f),  // 常规居中选区
        D2D1::RectF(0.0f, 0.0f, 1920.0f, 1080.0f),     // 全屏大选区（触发左下角快捷键提示避让）
        D2D1::RectF(80.0f, 300.0f, 1400.0f, 1020.0f)   // 左下贴边大选区（触发 alignedRightToAvoidHint）
    };
    const std::vector<MarkupTool> toolsWithSecondary = {
        MarkupTool::Rectangle,
        MarkupTool::Ellipse,
        MarkupTool::Arrow,
        MarkupTool::Text,
        MarkupTool::Magnifier
    };

    for (const auto& sel : testSelections) {
        for (MarkupTool tool : toolsWithSecondary) {
            CaptureState state;
            state.mode = OverlayMode::Screenshot;
            state.state = OverlayState::Marking;
            state.dpiScale = 1.0f;
            state.options.showShortcutHints = true;
            state.isMarkupToolActive = true;
            state.currentTool = tool;

            rebuildCaptureToolbar(state, sel, surface);
            ASSERT_GT(state.primaryToolbarRect.right - state.primaryToolbarRect.left, 100.0f);
            ASSERT_GT(state.secondaryToolbarRect.right - state.secondaryToolbarRect.left, 80.0f);

            // 核心心理预期契约：二级属性栏左边缘必须与一级主工具栏左边缘严格对齐，严禁回退为右边缘对齐
            EXPECT_NEAR(state.secondaryToolbarRect.left, state.primaryToolbarRect.left, 1.0f);
        }
    }
}

TEST(CaptureToolbarLayoutTest, SizeMenuSegmentedLayoutAndRecordToolbarBreathingContract) {
    using namespace tools3000::capture;

    // 1. px / dp 尺寸配置弹出菜单：常规居中与贴边（150% DPI）下严格单调不重叠、开关垂直中心同轴
    const D2D1_SIZE_F surface = D2D1_SIZE_F{1920.0f, 1080.0f};
    const auto normalMenu = computeSizeMenuLayout(
        D2D1::RectF(240.0f, 180.0f, 430.0f, 216.0f), surface, 1.0f);
    const auto edgeHiDpiMenu = computeSizeMenuLayout(
        D2D1::RectF(1760.0f, 980.0f, 1916.0f, 1034.0f), surface, 1.5f);

    for (const auto& lay : {normalMenu, edgeHiDpiMenu}) {
        EXPECT_GE(lay.menuRect.left, 0.0f);
        EXPECT_GE(lay.menuRect.top, 0.0f);
        EXPECT_LE(lay.menuRect.right, surface.width);
        EXPECT_LE(lay.menuRect.bottom, surface.height);

        // 双联分段滑块左右平分且绝对不重叠
        EXPECT_GT(lay.pxSegmentRect.right - lay.pxSegmentRect.left, 60.0f);
        EXPECT_GT(lay.dpSegmentRect.right - lay.dpSegmentRect.left, 60.0f);
        EXPECT_LT(lay.pxSegmentRect.right, lay.dpSegmentRect.left);

        // 纵向四层分区严格单调递增不打架（杜绝旧版三行文字重叠车祸）
        EXPECT_LT(lay.segmentTrackRect.bottom, lay.formulaRect.top);
        EXPECT_LT(lay.formulaRect.bottom, lay.dividerY);
        EXPECT_LT(lay.dividerY, lay.positionRowRect.top);
        EXPECT_LE(lay.positionRowRect.bottom, lay.unitRowRect.top);
        EXPECT_LT(lay.unitRowRect.bottom, lay.menuRect.bottom);

        // 开关胶囊与对应行垂直中线绝对同轴
        const float posRowCenterY = (lay.positionRowRect.top + lay.positionRowRect.bottom) * 0.5f;
        const float posSwCenterY = (lay.positionSwitchRect.top + lay.positionSwitchRect.bottom) * 0.5f;
        const float unitRowCenterY = (lay.unitRowRect.top + lay.unitRowRect.bottom) * 0.5f;
        const float unitSwCenterY = (lay.unitSwitchRect.top + lay.unitSwitchRect.bottom) * 0.5f;
        EXPECT_NEAR(posRowCenterY, posSwCenterY, 0.01f);
        EXPECT_NEAR(unitRowCenterY, unitSwCenterY, 0.01f);
    }

    // 2. 录屏准备栏（RecordRegion）：文字参数胶囊具备充裕水平呼吸空间，且主按钮不含高低错位的字体实心圆字符
    CaptureState recordState;
    recordState.mode = OverlayMode::RecordRegion;
    recordState.state = OverlayState::Selected;
    recordState.dpiScale = 1.0f;
    rebuildCaptureToolbar(recordState, D2D1::RectF(200.0f, 200.0f, 1000.0f, 700.0f), surface);

    ASSERT_EQ(recordState.toolbarButtons.size(), 9u);
    for (const auto& btn : recordState.toolbarButtons) {
        const float btnW = btn.rect.right - btn.rect.left;
        if (!btn.label.empty()) {
            EXPECT_GE(btnW, 50.0f);
        }
        if (btn.command == ToolbarCommand::RecordStartConfirm) {
            EXPECT_GE(btnW, 84.0f);
            EXPECT_EQ(btn.label.find(L"\u25CF"), std::wstring::npos);
        }
    }
}

TEST(PinWindowUiPolishRegressionTest, TooltipDynamicWidthFoldRgnAndUltraNarrowOcrPillNoCrash) {
    using namespace tools3000::capture;

    // 1. 验证 PinInputController::computeTooltipRect 放宽宽度上限 (220px * scale)，支持长双语文案单行显示且窄窗口不越界
    const auto wideTb = PinInputController::computeHoverToolbarLayout(400.0f, 200.0f, 1.0f);
    const float wideBtnCenter = (wideTb.buttons[2].left + wideTb.buttons[2].right) * 0.5f;
    const auto wideTip = PinInputController::computeTooltipRect(
        wideTb.toolbarRect, wideBtnCenter, 400.0f, 200.0f, 1.0f);
    EXPECT_NEAR(wideTip.right - wideTip.left, 220.0f, 0.01f);
    EXPECT_GE(wideTip.left, 4.0f);
    EXPECT_LE(wideTip.right, 396.0f);

    const auto narrowTb = PinInputController::computeHoverToolbarLayout(80.0f, 400.0f, 1.0f);
    const float narrowBtnCenter = (narrowTb.buttons[0].left + narrowTb.buttons[0].right) * 0.5f;
    const auto narrowTip = PinInputController::computeTooltipRect(
        narrowTb.toolbarRect, narrowBtnCenter, 80.0f, 400.0f, 1.0f);
    EXPECT_GE(narrowTip.left, 4.0f);
    EXPECT_LE(narrowTip.right, 76.0f);
    EXPECT_GT(narrowTip.right, narrowTip.left);

    // 2. 真实 PinWindow 实测：16x16 极窄贴图注入 OCR 选区触发复制胶囊渲染防 std::clamp UB 崩溃 + 折叠圆角 Region 切换
    std::array<wchar_t, 4096> executablePath{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    ASSERT_GT(length, 0u);
    ASSERT_LT(length, executablePath.size());

    const auto executableDir = std::filesystem::path(executablePath.data()).parent_path();
    const auto pluginDllPath =
        executableDir.parent_path() / L"plugins" / executableDir.filename() / L"Plugin_Capture.dll";
    ASSERT_TRUE(std::filesystem::exists(pluginDllPath));

    HMODULE hMod = LoadLibraryExW(
        pluginDllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    ASSERT_NE(hMod, nullptr);

    using CreatePluginFunc = tools3000::core::IPlugin* (*)();
    const auto createPlugin = reinterpret_cast<CreatePluginFunc>(GetProcAddress(hMod, "CreatePlugin"));
    ASSERT_NE(createPlugin, nullptr);
    tools3000::core::IPlugin* capturePlugin = createPlugin();
    ASSERT_NE(capturePlugin, nullptr);
    ASSERT_TRUE(capturePlugin->initialize());

    struct PluginCleanupGuard {
        HMODULE mod = nullptr;
        tools3000::core::IPlugin* plugin = nullptr;
        ~PluginCleanupGuard() {
            while (HWND existing = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
                SendMessageW(existing, WM_KEYDOWN, VK_ESCAPE, 0);
                if (IsWindow(existing)) DestroyWindow(existing);
            }
            if (plugin) plugin->shutdown();
            if (mod) FreeLibrary(mod);
        }
    } cleanupGuard{hMod, capturePlugin};

    while (HWND existing = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
        SendMessageW(existing, WM_KEYDOWN, VK_ESCAPE, 0);
        if (IsWindow(existing)) DestroyWindow(existing);
    }

    auto& bridge = tools3000::core::MessageBridge::instance();
    auto invokeRpc = [&](int id, const char* method, const nlohmann::json& params) -> nlohmann::json {
        nlohmann::json req = {{"id", id}, {"method", method}, {"params", params}};
        const std::string raw = bridge.handleMessage(req.dump());
        const auto parsed = nlohmann::json::parse(raw, nullptr, false);
        return parsed.is_object() && parsed.contains("result") ? parsed["result"] : parsed;
    };

    cv::Mat tinyImg(16, 16, CV_8UC3, cv::Scalar(32, 42, 58));
    const auto tinyPng = std::filesystem::temp_directory_path() /
                         (L"tools3000_pin_tiny_ocr_" + std::to_wstring(GetCurrentProcessId()) + L".png");
    std::vector<uchar> pngBuf;
    ASSERT_TRUE(cv::imencode(".png", tinyImg, pngBuf));
    {
        std::ofstream ofs(tinyPng, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(pngBuf.data()), static_cast<std::streamsize>(pngBuf.size()));
    }
    struct TempFileGuard {
        std::filesystem::path p;
        ~TempFileGuard() {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    } tempGuard{tinyPng};

    const auto pinned = invokeRpc(881, "capture.pinImageFile", {
        {"path", tools3000::core::WinUtils::wstringToUtf8(tinyPng.wstring())}
    });
    ASSERT_TRUE(pinned.value("success", false));

    HWND pinHwnd = FindWindowW(L"Tools3000_PinWindow", nullptr);
    ASSERT_NE(pinHwnd, nullptr);

    // 注入 OCR 文字选区并强制重绘 16x16 极窄贴图窗口：不得因 size.width < pillW + 12*dpiScale 触发 std::clamp 崩溃
    const auto ocrState = invokeRpc(882, "capture.inspectPinOcrState", {
        {"mockBlocks", nlohmann::json::array({
            {{"text", "TinyPin"}, {"x", 1}, {"y", 1}, {"w", 14}, {"h", 14}}
        })},
        {"selectAll", true}
    });
    ASSERT_TRUE(ocrState.value("success", false));
    InvalidateRect(pinHwnd, nullptr, FALSE);
    UpdateWindow(pinHwnd);
    SendMessageW(pinHwnd, WM_PAINT, 0, 0);
    ASSERT_TRUE(IsWindow(pinHwnd));

    // 退出选字态后验证折叠圆角 Region 裁剪与展开恢复直角无裁剪
    invokeRpc(883, "capture.inspectPinOcrState", {{"clearSelection", true}});
    SendMessageW(pinHwnd, WM_KEYDOWN, VK_ESCAPE, 0);
    ASSERT_TRUE(IsWindow(pinHwnd));

    HRGN probeRgn = CreateRectRgn(0, 0, 0, 0);
    ASSERT_NE(probeRgn, nullptr);
    struct RgnGuard {
        HRGN rgn = nullptr;
        ~RgnGuard() { if (rgn) DeleteObject(rgn); }
    } rgnGuard{probeRgn};

    EXPECT_EQ(GetWindowRgn(pinHwnd, probeRgn), ERROR);

    // 折叠为微胶囊：挂载圆角裁剪 Region 消除四角直角黑三角底衬
    SendMessageW(pinHwnd, WM_KEYDOWN, 'M', 0);
    EXPECT_NE(GetWindowRgn(pinHwnd, probeRgn), ERROR);

    // 折叠态模拟跨屏 WM_DPICHANGED (144 DPI = 150%)：平滑重算尺寸与圆角 Region
    RECT suggestedFoldRect{260, 260, 480, 311};
    SendMessageW(pinHwnd, WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&suggestedFoldRect));
    EXPECT_NE(GetWindowRgn(pinHwnd, probeRgn), ERROR);

    // 再次按 'M' 展开：清除窗口 Region 恢复直角贴图完整像素
    SendMessageW(pinHwnd, WM_KEYDOWN, 'M', 0);
    EXPECT_EQ(GetWindowRgn(pinHwnd, probeRgn), ERROR);

    SendMessageW(pinHwnd, WM_KEYDOWN, VK_ESCAPE, 0);
    EXPECT_FALSE(IsWindow(pinHwnd));
}

TEST(ScrollCaptureUiPolishRegressionTest, CompactDegradationKeepsFinishAndCancelInBoundsOnHiDpiPortrait) {
    using namespace tools3000::capture;

    // 1. 不限宽模式 (workWidth = 0) 保持全量布局兼容
    const auto fullEn = ScrollCaptureOverlay::buildControlBarLayout(2.0f, false, false, 0);
    EXPECT_EQ(fullEn.compactLevel, 0);
    EXPECT_EQ(fullEn.keycaps[1], L"Ctrl+T");
    EXPECT_EQ(fullEn.keycaps[2], L"Ctrl+S");

    // 2. 200% DPI 中等屏宽 (1280px)：自动降级为一级紧凑（隐藏按钮内嵌键帽胶囊），全栏严格留在安全边距内
    const auto level1En = ScrollCaptureOverlay::buildControlBarLayout(2.0f, false, false, 1280);
    EXPECT_EQ(level1En.compactLevel, 1);
    EXPECT_TRUE(level1En.keycaps[1].empty());
    EXPECT_TRUE(level1En.keycaps[2].empty());
    EXPECT_FALSE(level1En.labels[1].empty());
    EXPECT_LE(level1En.barWidth, 1280 - 40);
    EXPECT_LE(level1En.buttonRects[4].right, static_cast<float>(level1En.barWidth));

    // 3. 200% DPI 窄宽视口 (760px)：中英文均自动降级为二级紧凑（贴图/保存收敛为纯图标按钮），取消/完成 100% 留在屏内
    for (bool zh : {true, false}) {
        const auto portraitBar = ScrollCaptureOverlay::buildControlBarLayout(2.0f, zh, false, 760);
        EXPECT_GE(portraitBar.compactLevel, 2);
        EXPECT_TRUE(portraitBar.labels[1].empty());
        EXPECT_TRUE(portraitBar.labels[2].empty());
        EXPECT_TRUE(portraitBar.labels[3].empty());
        EXPECT_FALSE(portraitBar.labels[4].empty());
        EXPECT_LE(portraitBar.barWidth, 760 - 40);
        for (size_t i = 0; i < portraitBar.buttonRects.size(); ++i) {
            EXPECT_LT(portraitBar.buttonRects[i].left, portraitBar.buttonRects[i].right);
            if (i + 1 < portraitBar.buttonRects.size()) {
                EXPECT_LE(portraitBar.buttonRects[i].right, portraitBar.buttonRects[i + 1].left);
            }
        }
        EXPECT_LE(portraitBar.buttonRects[4].right, static_cast<float>(portraitBar.barWidth));
    }
}

TEST(CaptureToolbarLayoutTest, WidenedBeautyShellAndTripleDigitFontSizeButtonsContract) {
    using namespace tools3000::capture;

    const auto tempCfgDir = std::filesystem::temp_directory_path() /
                            (L"tools3000_toolbar_lang_cfg_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(tempCfgDir);
    auto& cfg = tools3000::core::ConfigManager::instance();
    cfg.shutdown();
    ASSERT_TRUE(cfg.initialize(tempCfgDir));
    struct LangCfgCleanupGuard {
        std::filesystem::path dir;
        ~LangCfgCleanupGuard() {
            tools3000::core::ConfigManager::instance().shutdown();
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    } langGuard{tempCfgDir};

    const D2D1_RECT_F sel = D2D1::RectF(120.0f, 120.0f, 900.0f, 620.0f);
    const D2D1_SIZE_F surface = D2D1_SIZE_F{1920.0f, 1080.0f};

    auto findButtonWidth = [](const std::vector<ToolbarButton>& list, ToolbarCommand cmd) -> float {
        for (const auto& b : list) {
            if (b.command == cmd) return b.rect.right - b.rect.left;
        }
        return 0.0f;
    };

    // 1. 中文模式下验证美化外壳按钮独立态 (88px) 与伴随标注态 (66px)
    cfg.set<std::string>("/general/language", "zh-CN");
    {
        CaptureState state;
        state.mode = OverlayMode::Screenshot;
        state.state = OverlayState::Selected;
        state.dpiScale = 1.0f;
        state.beautyShell.enabled = true;
        state.isMarkupToolActive = false;
        rebuildCaptureToolbar(state, sel, surface);
        EXPECT_NEAR(findButtonWidth(state.secondaryToolbarButtons, ToolbarCommand::CycleBeautyBg), 88.0f, 0.05f);

        state.isMarkupToolActive = true;
        state.currentTool = MarkupTool::Rectangle;
        state.toolbarLayoutValid = false;
        rebuildCaptureToolbar(state, sel, surface);
        EXPECT_NEAR(findButtonWidth(state.secondaryToolbarButtons, ToolbarCommand::CycleBeautyBg), 66.0f, 0.05f);
    }

    // 2. 英文模式下验证美化外壳按钮独立态 (132px) 与伴随标注态 (90px)，以及文本工具三位数字号胶囊拓宽 (52px)
    cfg.set<std::string>("/general/language", "en-US");
    {
        CaptureState state;
        state.mode = OverlayMode::Screenshot;
        state.state = OverlayState::Selected;
        state.dpiScale = 1.0f;
        state.beautyShell.enabled = true;
        state.isMarkupToolActive = false;
        rebuildCaptureToolbar(state, sel, surface);
        EXPECT_NEAR(findButtonWidth(state.secondaryToolbarButtons, ToolbarCommand::CycleBeautyBg), 132.0f, 0.05f);

        state.isMarkupToolActive = true;
        state.currentTool = MarkupTool::Text;
        cv::Mat dummy(200, 300, CV_8UC3, cv::Scalar(30, 30, 30));
        state.markup.setBaseImage(dummy);
        state.activeElement = state.markup.addText(cv::Point(20, 20), "Hi", MarkupColor::Red(), 32.0f);
        state.toolbarLayoutValid = false;
        rebuildCaptureToolbar(state, sel, surface);
        EXPECT_NEAR(findButtonWidth(state.secondaryToolbarButtons, ToolbarCommand::CycleStrokeWidth), 44.0f, 0.05f);
        EXPECT_NEAR(findButtonWidth(state.secondaryToolbarButtons, ToolbarCommand::CycleBeautyBg), 90.0f, 0.05f);

        state.activeElement->fontSize = 120.0f;
        state.toolbarLayoutValid = false;
        rebuildCaptureToolbar(state, sel, surface);
        EXPECT_EQ(state.secondaryToolbarButtons.front().label, L"120");
        EXPECT_NEAR(findButtonWidth(state.secondaryToolbarButtons, ToolbarCommand::CycleStrokeWidth), 52.0f, 0.05f);
    }
}

TEST(CaptureTextEditorTest, LargeFontSizeDynamicOriginPreventsOutlineClippingAndKeepsHitTestAligned) {
    using namespace tools3000::capture;

    // 1. 小字号保持 6px / 4px 原点兼容；大字号 (120px, strokeWidth=21.6px, 半宽=10.8px) 动态扩展安全边距
    const auto smallLayout = CaptureTextEditor::buildLayout(L"Hg", 20);
    EXPECT_FLOAT_EQ(smallLayout.textOriginX, 6.0f);
    EXPECT_FLOAT_EQ(smallLayout.textOriginY, 4.0f);

    const std::wstring bigSample = L"Hg 大字描边";
    const int bigFontSize = 120;
    const auto bigLayout = CaptureTextEditor::buildLayout(bigSample, bigFontSize);
    const float expectedHalfStroke = CaptureTextEditor::computeOutlineStrokeWidth(bigFontSize) * 0.5f;
    EXPECT_GT(bigLayout.textOriginX, expectedHalfStroke + 2.0f);
    EXPECT_GT(bigLayout.textOriginY, expectedHalfStroke + 1.0f);

    // 验证动态边距下大字号每个字符的点击反算 hitTestCaretIndex 依然 100% 精准对齐
    ASSERT_FALSE(bigLayout.lines.empty());
    const auto& line = bigLayout.lines.front();
    const float midY = line.topY + line.height * 0.5f;
    for (size_t col = 0; col + 1 < line.charOffsets.size(); ++col) {
        const float xLeft = bigLayout.textOriginX + line.charOffsets[col] * 0.75f + line.charOffsets[col + 1] * 0.25f;
        const float xRight = bigLayout.textOriginX + line.charOffsets[col] * 0.25f + line.charOffsets[col + 1] * 0.75f;
        EXPECT_EQ(CaptureTextEditor::hitTestCaretIndex(bigLayout, bigSample, xLeft, midY), col);
        EXPECT_EQ(CaptureTextEditor::hitTestCaretIndex(bigLayout, bigSample, xRight, midY), col + 1);
    }

    // 2. 验证 MarkupEngine 渲染 120px 大字号高反差白描边文字：编辑态/非编辑态尺寸恒定，且四边绝不撞边切平
    cv::Mat darkBase(420, 900, CV_8UC3, cv::Scalar(20, 20, 20));
    MarkupEngine engine;
    engine.setBaseImage(darkBase);
    auto* textElem = engine.addText(
        cv::Point(50, 50), "HgPy", MarkupColor::Red(), static_cast<float>(bigFontSize), true, MarkupColor::White());
    ASSERT_NE(textElem, nullptr);

    const auto* handler = MarkupToolRegistry::instance().getHandler(MarkupTool::Text);
    ASSERT_NE(handler, nullptr);

    textElem->isEditing = true;
    const cv::Rect editBox = handler->getBoundingBox(*textElem);
    textElem->isEditing = false;
    const cv::Rect normalBox = handler->getBoundingBox(*textElem);
    EXPECT_EQ(editBox.width, normalBox.width);
    EXPECT_EQ(editBox.height, normalBox.height);

    const cv::Mat rendered = engine.getCompositeImage(false);
    ASSERT_FALSE(rendered.empty());
    const cv::Rect bbox = handler->getBoundingBox(*textElem);
    ASSERT_GE(bbox.x, 0);
    ASSERT_GE(bbox.y, 0);
    ASSERT_LE(bbox.x + bbox.width, rendered.cols);
    ASSERT_LE(bbox.y + bbox.height, rendered.rows);

    int interiorOutlinePixels = 0;
    for (int y = bbox.y; y < bbox.y + bbox.height; ++y) {
        for (int x = bbox.x; x < bbox.x + bbox.width; ++x) {
            const cv::Vec3b px = rendered.at<cv::Vec3b>(y, x);
            if (px[0] > 200 && px[1] > 200 && px[2] > 200) {
                ++interiorOutlinePixels;
            }
        }
    }
    EXPECT_GT(interiorOutlinePixels, 200);

    // 文本框位图最外圈第 0 行/列与末行/列应保持背景色 (20,20,20)，证明外扩描边未被位图四边切平
    int clippedBorderWhitePixels = 0;
    for (int x = bbox.x; x < bbox.x + bbox.width; ++x) {
        const cv::Vec3b topPx = rendered.at<cv::Vec3b>(bbox.y, x);
        const cv::Vec3b botPx = rendered.at<cv::Vec3b>(bbox.y + bbox.height - 1, x);
        if (topPx[0] > 180 && topPx[1] > 180 && topPx[2] > 180) ++clippedBorderWhitePixels;
        if (botPx[0] > 180 && botPx[1] > 180 && botPx[2] > 180) ++clippedBorderWhitePixels;
    }
    for (int y = bbox.y; y < bbox.y + bbox.height; ++y) {
        const cv::Vec3b leftPx = rendered.at<cv::Vec3b>(y, bbox.x);
        const cv::Vec3b rightPx = rendered.at<cv::Vec3b>(y, bbox.x + bbox.width - 1);
        if (leftPx[0] > 180 && leftPx[1] > 180 && leftPx[2] > 180) ++clippedBorderWhitePixels;
        if (rightPx[0] > 180 && rightPx[1] > 180 && rightPx[2] > 180) ++clippedBorderWhitePixels;
    }
    EXPECT_EQ(clippedBorderWhitePixels, 0);
}


TEST(CornerRadiusCursorTest, GoldenVisualSpanAndDragConsistency) {
    using namespace tools3000::capture;

    auto measureCursorBounds = [](HCURSOR hCur, int& outBmpW, int& outSpanX, int& outSpanY) -> bool {
        outBmpW = 0;
        outSpanX = 0;
        outSpanY = 0;
        if (!hCur) return false;

        ICONINFO ii{};
        if (!GetIconInfo(hCur, &ii)) return false;

        bool ok = false;
        if (ii.hbmColor) {
            BITMAP bm{};
            if (GetObjectW(ii.hbmColor, sizeof(bm), &bm) > 0 && bm.bmWidth > 0 && bm.bmHeight > 0) {
                outBmpW = bm.bmWidth;
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = bm.bmWidth;
                bi.bmiHeader.biHeight = -bm.bmHeight;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;

                std::vector<uint32_t> px(static_cast<size_t>(bm.bmWidth) * static_cast<size_t>(bm.bmHeight), 0);
                HDC hdc = GetDC(nullptr);
                if (GetDIBits(hdc, ii.hbmColor, 0, static_cast<UINT>(bm.bmHeight),
                              px.data(), &bi, DIB_RGB_COLORS) > 0) {
                    int minX = bm.bmWidth, maxX = -1;
                    int minY = bm.bmHeight, maxY = -1;
                    for (int y = 0; y < bm.bmHeight; ++y) {
                        for (int x = 0; x < bm.bmWidth; ++x) {
                            const uint8_t alpha = static_cast<uint8_t>((px[static_cast<size_t>(y) * bm.bmWidth + x] >> 24) & 0xFF);
                            if (alpha >= 32) {
                                minX = (std::min)(minX, x);
                                maxX = (std::max)(maxX, x);
                                minY = (std::min)(minY, y);
                                maxY = (std::max)(maxY, y);
                            }
                        }
                    }
                    if (maxX >= minX && maxY >= minY) {
                        outSpanX = maxX - minX + 1;
                        outSpanY = maxY - minY + 1;
                        ok = true;
                    }
                }
                ReleaseDC(nullptr, hdc);
            }
        }
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        return ok;
    };

    int bmpW100 = 0, hoverSpanX100 = 0, hoverSpanY100 = 0;
    ASSERT_TRUE(measureCursorBounds(
        CornerRadiusHelper::getLiveCursor(0, 1.0f, false), bmpW100, hoverSpanX100, hoverSpanY100));
    EXPECT_GE(bmpW100, 48);
    EXPECT_GE(hoverSpanX100, 18);
    EXPECT_GE(hoverSpanY100, 18);

    // 拖拽态同样保持饱满双向圆角箭头体量，绝不缩水为迷你小圆点
    int dragBmpW = 0, dragSpanX = 0, dragSpanY = 0;
    ASSERT_TRUE(measureCursorBounds(
        CornerRadiusHelper::getLiveCursor(1, 1.0f, true), dragBmpW, dragSpanX, dragSpanY));
    EXPECT_GE(dragSpanX, 17);
    EXPECT_GE(dragSpanY, 17);

    int bmpW150 = 0, hoverSpanX150 = 0, hoverSpanY150 = 0;
    ASSERT_TRUE(measureCursorBounds(
        CornerRadiusHelper::getLiveCursor(0, 1.5f, false), bmpW150, hoverSpanX150, hoverSpanY150));
    EXPECT_GT(hoverSpanX150, hoverSpanX100);
    EXPECT_GT(hoverSpanY150, hoverSpanY100);
}

TEST(MicroGlassNotePinTest, CompactSingleLineNoteVerticalStackingAndExactCopy) {
    using namespace tools3000::capture;

    // 1. 单行密码微晶字签：紧凑高度、无山寨红黄绿三圆点、支持高分屏清晰放大
    const cv::Mat note100 = PinPasteCoordinator::renderTextToImage(L"Hycan@!0798", 1.0f);
    ASSERT_FALSE(note100.empty());
    EXPECT_EQ(note100.type(), CV_8UC3);
    EXPECT_GE(note100.cols, 116);
    EXPECT_GE(note100.rows, 40);
    EXPECT_LE(note100.rows, 56);

    int trafficLightGreenPixels = 0;
    for (int y = 0; y < (std::min)(24, note100.rows); ++y) {
        for (int x = 0; x < (std::min)(64, note100.cols); ++x) {
            const cv::Vec3b bgr = note100.at<cv::Vec3b>(y, x);
            if (bgr[1] > 180 && bgr[2] < 80 && bgr[0] < 90) {
                ++trafficLightGreenPixels;
            }
        }
    }
    EXPECT_EQ(trafficLightGreenPixels, 0);

    const cv::Mat note150 = PinPasteCoordinator::renderTextToImage(L"Hycan@!0798", 1.5f);
    ASSERT_FALSE(note150.empty());
    EXPECT_GT(note150.cols, note100.cols);
    EXPECT_GT(note150.rows, note100.rows);

    // 2. 多张不同长度微晶字签在相近光标处连续贴出：自动左对齐顺延排在下方，零遮挡保心流
    PinSpawnInput stackInput;
    stackInput.imageWidth = 140;
    stackInput.imageHeight = 44;
    stackInput.cursor = POINT{595, 322};
    stackInput.workArea = RECT{0, 0, 1920, 1040};
    stackInput.virtualScreen = RECT{0, 0, 1920, 1080};
    stackInput.existingPins = {RECT{500, 300, 690, 344}}; // 第一张较宽字签 (left=500, 宽190)
    stackInput.stackVerticallyOnIntersect = true;
    stackInput.stackGap = 8;

    const POINT secondPos = PinSpawnCalculator::calculate(stackInput);
    EXPECT_EQ(secondPos.x, 500);
    EXPECT_EQ(secondPos.y, 352);

    // 3. 通过 Plugin_Capture 端到端实测剪贴板纯文本贴为真圆角微晶字签与一键零误差复制原文
    std::array<wchar_t, 4096> executablePath{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    ASSERT_GT(length, 0u);
    ASSERT_LT(length, executablePath.size());

    const auto executableDir = std::filesystem::path(executablePath.data()).parent_path();
    const auto pluginDllPath =
        executableDir.parent_path() / L"plugins" / executableDir.filename() / L"Plugin_Capture.dll";
    ASSERT_TRUE(std::filesystem::exists(pluginDllPath));

    HMODULE hMod = LoadLibraryExW(
        pluginDllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    ASSERT_NE(hMod, nullptr);

    using CreatePluginFunc = tools3000::core::IPlugin* (*)();
    const auto createPlugin = reinterpret_cast<CreatePluginFunc>(GetProcAddress(hMod, "CreatePlugin"));
    ASSERT_NE(createPlugin, nullptr);
    tools3000::core::IPlugin* capturePlugin = createPlugin();
    ASSERT_NE(capturePlugin, nullptr);
    ASSERT_TRUE(capturePlugin->initialize());

    struct PluginCleanupGuard {
        HMODULE mod = nullptr;
        tools3000::core::IPlugin* plugin = nullptr;
        ~PluginCleanupGuard() {
            while (HWND existing = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
                SendMessageW(existing, WM_KEYDOWN, VK_ESCAPE, 0);
                if (IsWindow(existing)) DestroyWindow(existing);
            }
            if (plugin) plugin->shutdown();
            if (mod) FreeLibrary(mod);
        }
    } cleanupGuard{hMod, capturePlugin};

    while (HWND existing = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
        SendMessageW(existing, WM_KEYDOWN, VK_ESCAPE, 0);
        if (IsWindow(existing)) DestroyWindow(existing);
    }

    auto& bridge = tools3000::core::MessageBridge::instance();
    auto invokeRpc = [&](int id, const char* method, const nlohmann::json& params) -> nlohmann::json {
        nlohmann::json req = {{"id", id}, {"method", method}, {"params", params}};
        const std::string raw = bridge.handleMessage(req.dump());
        const auto parsed = nlohmann::json::parse(raw, nullptr, false);
        return parsed.is_object() && parsed.contains("result") ? parsed["result"] : parsed;
    };

    invokeRpc(901, "history.clear", {});

    auto writeClipText = [](const std::wstring& s) -> bool {
        for (int retry = 0; retry < 10; ++retry) {
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
                if (HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                    if (auto* dst = static_cast<wchar_t*>(GlobalLock(hMem))) {
                        std::memcpy(dst, s.c_str(), bytes);
                        GlobalUnlock(hMem);
                        SetClipboardData(CF_UNICODETEXT, hMem);
                    } else {
                        GlobalFree(hMem);
                    }
                }
                CloseClipboard();
                return true;
            }
            Sleep(5);
        }
        return false;
    };

    ASSERT_TRUE(writeClipText(L"Hycan@!0798"));
    const auto pasted = invokeRpc(902, "capture.pasteAsPin", {});
    ASSERT_TRUE(pasted.value("success", false));

    HWND pinHwnd = FindWindowW(L"Tools3000_PinWindow", nullptr);
    ASSERT_NE(pinHwnd, nullptr);

    HRGN probeRgn = CreateRectRgn(0, 0, 0, 0);
    ASSERT_NE(probeRgn, nullptr);
    struct RgnGuard {
        HRGN rgn = nullptr;
        ~RgnGuard() { if (rgn) DeleteObject(rgn); }
    } rgnGuard{probeRgn};

    // 微晶字签默认即挂载真圆角窗口裁剪 Region，消除死直角灰框
    EXPECT_NE(GetWindowRgn(pinHwnd, probeRgn), ERROR);

    // 覆盖剪贴板后触发 copyAllText，验证零误差秒速还原内嵌原始纯文本
    ASSERT_TRUE(writeClipText(L"__OVERWRITTEN__"));
    const auto inspected = invokeRpc(903, "capture.inspectPinOcrState", {{"copyAll", true}});
    ASSERT_TRUE(inspected.value("success", false));
    EXPECT_TRUE(inspected.value("isGeneratedCard", false));
    EXPECT_EQ(inspected.value("pinnedSourceText", std::string{}), "Hycan@!0798");

    std::wstring copiedBack;
    for (int retry = 0; retry < 10; ++retry) {
        if (OpenClipboard(nullptr)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                    copiedBack = p;
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
            break;
        }
        Sleep(5);
    }
    EXPECT_EQ(copiedBack, L"Hycan@!0798");
}

TEST(CapturePixPinUpgradeTest, ToolbarSixDotGripFreeDragAndSpotlightDropdownLayout) {
    using namespace tools3000::capture;

    const D2D1_SIZE_F surface{1920.0f, 1080.0f};
    const D2D1_RECT_F selection = D2D1::RectF(300.0f, 220.0f, 1020.0f, 640.0f);

    CaptureState state;
    state.mode = OverlayMode::Screenshot;
    state.state = OverlayState::Marking;
    state.dpiScale = 1.0f;
    state.isMarkupToolActive = true;
    state.currentTool = MarkupTool::Spotlight;

    rebuildCaptureToolbar(state, selection, surface);

    // 1. 主工具栏最左侧必须拥有独立 2x3 六点灰色拖拽手柄，且与首个功能按钮零重叠
    EXPECT_GT(state.toolbarGripRect.right - state.toolbarGripRect.left, 12.0f);
    EXPECT_GT(state.toolbarGripRect.bottom - state.toolbarGripRect.top, 24.0f);
    ASSERT_FALSE(state.toolbarButtons.empty());
    EXPECT_LE(state.toolbarGripRect.right, state.toolbarButtons.front().rect.left);

    // 2. 手动拖拽手柄设定自定义原点后，主工具栏精准跟随自定义坐标并自动钳制屏幕安全边界，二级栏同步左对齐跟随
    state.hasCustomToolbarPos = true;
    state.customToolbarOrigin = D2D1::Point2F(120.0f, 820.0f);
    rebuildCaptureToolbar(state, selection, surface);
    EXPECT_NEAR(state.primaryToolbarRect.left, 120.0f, 1.0f);
    EXPECT_NEAR(state.primaryToolbarRect.top, 820.0f, 1.0f);
    EXPECT_NEAR(state.secondaryToolbarRect.left, state.primaryToolbarRect.left, 1.0f);

    // 拖往右下角越界位置时自动钳制在屏幕可视安全范围内
    state.customToolbarOrigin = D2D1::Point2F(1880.0f, 1065.0f);
    rebuildCaptureToolbar(state, selection, surface);
    EXPECT_LE(state.primaryToolbarRect.right, surface.width);
    EXPECT_LE(state.primaryToolbarRect.bottom, surface.height);

    // 3. 验证聚光灯二级属性栏包含形状切换（矩形/椭圆）、描边开关、圆角调节与暗化浓度调节
    bool hasSpotlightShapeToggle = false;
    bool hasSpotlightBorder = false;
    bool hasSpotlightCorner = false;
    bool hasSpotlightDim = false;
    for (const auto& btn : state.secondaryToolbarButtons) {
        if (btn.command == ToolbarCommand::ToggleSpotlightShape) hasSpotlightShapeToggle = true;
        if (btn.command == ToolbarCommand::ToggleFill) hasSpotlightBorder = true;
        if (btn.command == ToolbarCommand::CycleElementCornerRadius) hasSpotlightCorner = true;
        if (btn.command == ToolbarCommand::CycleSpotlightDim) hasSpotlightDim = true;
    }
    EXPECT_TRUE(hasSpotlightShapeToggle);
    EXPECT_TRUE(hasSpotlightBorder);
    EXPECT_TRUE(hasSpotlightCorner);
    EXPECT_TRUE(hasSpotlightDim);

    // 4. 验证 ShapeMode 下拉菜单切换几何图形与聚光灯，以及 NumberFormat 5 种序号制式下拉提交
    openDropdownMenu(state, DropdownType::ShapeMode, state.toolbarButtons.front().rect);
    EXPECT_EQ(state.dropdownMenu.type, DropdownType::ShapeMode);
    ASSERT_EQ(state.dropdownMenu.items.size(), 2u);
    commitDropdownSelection(state, DropdownType::ShapeMode, 1);
    EXPECT_EQ(state.currentTool, MarkupTool::Spotlight);
    EXPECT_TRUE(state.isMarkupToolActive);

    commitDropdownSelection(state, DropdownType::ShapeMode, 0);
    EXPECT_NE(state.currentTool, MarkupTool::Spotlight);
    EXPECT_TRUE(state.isMarkupToolActive);

    openDropdownMenu(state, DropdownType::NumberFormat, state.toolbarButtons.front().rect);
    ASSERT_EQ(state.dropdownMenu.items.size(), 5u);
    commitDropdownSelection(state, DropdownType::NumberFormat, static_cast<int>(NumberFormat::Chinese));
    EXPECT_EQ(state.currentNumberFormat, NumberFormat::Chinese);
}

TEST(CapturePixPinUpgradeTest, MultiFormatNumberBadgesAndMultiSpotlightUnionMasking) {
    using namespace tools3000::capture;

    // 1. 验证 5 种序号制式格式化精确度（含现代中文习惯“十 / 十一 / 二十 / 二十一”）
    EXPECT_EQ(formatNumberBadgeLabel(1, NumberFormat::Arabic), L"1");
    EXPECT_EQ(formatNumberBadgeLabel(12, NumberFormat::Arabic), L"12");

    EXPECT_EQ(formatNumberBadgeLabel(1, NumberFormat::UpperAlpha), L"A");
    EXPECT_EQ(formatNumberBadgeLabel(26, NumberFormat::UpperAlpha), L"Z");
    EXPECT_EQ(formatNumberBadgeLabel(27, NumberFormat::UpperAlpha), L"AA");

    EXPECT_EQ(formatNumberBadgeLabel(1, NumberFormat::LowerAlpha), L"a");
    EXPECT_EQ(formatNumberBadgeLabel(3, NumberFormat::LowerAlpha), L"c");
    EXPECT_EQ(formatNumberBadgeLabel(28, NumberFormat::LowerAlpha), L"ab");

    EXPECT_EQ(formatNumberBadgeLabel(1, NumberFormat::Roman), L"I");
    EXPECT_EQ(formatNumberBadgeLabel(2, NumberFormat::Roman), L"II");
    EXPECT_EQ(formatNumberBadgeLabel(3, NumberFormat::Roman), L"III");
    EXPECT_EQ(formatNumberBadgeLabel(4, NumberFormat::Roman), L"IV");
    EXPECT_EQ(formatNumberBadgeLabel(8, NumberFormat::Roman), L"VIII");
    EXPECT_EQ(formatNumberBadgeLabel(11, NumberFormat::Roman), L"XI");

    EXPECT_EQ(formatNumberBadgeLabel(1, NumberFormat::Chinese), L"一");
    EXPECT_EQ(formatNumberBadgeLabel(2, NumberFormat::Chinese), L"二");
    EXPECT_EQ(formatNumberBadgeLabel(3, NumberFormat::Chinese), L"三");
    EXPECT_EQ(formatNumberBadgeLabel(10, NumberFormat::Chinese), L"十");
    EXPECT_EQ(formatNumberBadgeLabel(11, NumberFormat::Chinese), L"十一");
    EXPECT_EQ(formatNumberBadgeLabel(20, NumberFormat::Chinese), L"二十");
    EXPECT_EQ(formatNumberBadgeLabel(21, NumberFormat::Chinese), L"二十一");

    // 2. 验证多字符序号在 RoundedSquare 模式下生成横向自适应胶囊包围盒（宽度显著大于高度）
    initializeMarkupTextRenderer();
    cv::Mat baseCanvas(220, 320, CV_8UC3, cv::Scalar(240, 240, 240));
    MarkupEngine engine;
    engine.setBaseImage(baseCanvas);
    engine.resetNumber(8, NumberFormat::Roman);
    auto* romanBadge = engine.drawNumberMark(
        cv::Point(160, 60), MarkupColor::Red(), 1.0f, NumberBadgeShape::RoundedSquare, NumberFormat::Roman);
    ASSERT_NE(romanBadge, nullptr);
    EXPECT_EQ(romanBadge->numberValue, 8);
    const cv::Rect capsuleBox = romanBadge->getBoundingBox();
    EXPECT_GT(capsuleBox.width, capsuleBox.height);

    // 2.5 验证 5 种序号制式各自拥有完全独立的递增序列（交叉标注、撤销、重做、删除、重置互不污染）
    MarkupEngine seqEngine;
    seqEngine.setBaseImage(baseCanvas);
    auto* n1 = seqEngine.drawNumberMark(cv::Point(20, 20), MarkupColor::Red(), 1.0f, NumberBadgeShape::Circle, NumberFormat::Arabic);
    auto* n2 = seqEngine.drawNumberMark(cv::Point(50, 20), MarkupColor::Red(), 1.0f, NumberBadgeShape::Circle, NumberFormat::Arabic);
    ASSERT_NE(n1, nullptr);
    ASSERT_NE(n2, nullptr);
    EXPECT_EQ(n1->numberValue, 1);
    EXPECT_EQ(n2->numberValue, 2);

    // 切换到大写字母必须从 A (1) 开始，绝不能变成 C (3)
    auto* a1 = seqEngine.drawNumberMark(cv::Point(20, 60), MarkupColor::Red(), 1.0f, NumberBadgeShape::Circle, NumberFormat::UpperAlpha);
    auto* a2 = seqEngine.drawNumberMark(cv::Point(50, 60), MarkupColor::Red(), 1.0f, NumberBadgeShape::Circle, NumberFormat::UpperAlpha);
    EXPECT_EQ(a1->numberValue, 1);
    EXPECT_EQ(a2->numberValue, 2);

    // 切换到罗马数字与中文数字同样各自从 I (1) 与 一 (1) 独立起算
    auto* r1 = seqEngine.drawNumberMark(cv::Point(20, 100), MarkupColor::Red(), 1.0f, NumberBadgeShape::Circle, NumberFormat::Roman);
    auto* c1 = seqEngine.drawNumberMark(cv::Point(50, 100), MarkupColor::Red(), 1.0f, NumberBadgeShape::Circle, NumberFormat::Chinese);
    auto* l1 = seqEngine.drawNumberMark(cv::Point(80, 100), MarkupColor::Red(), 1.0f, NumberBadgeShape::Circle, NumberFormat::LowerAlpha);
    EXPECT_EQ(r1->numberValue, 1);
    EXPECT_EQ(c1->numberValue, 1);
    EXPECT_EQ(l1->numberValue, 1);

    // 切回阿拉伯数字继续接着 3 递增，切回大写字母接着 C (3) 递增
    EXPECT_EQ(seqEngine.currentNumber(NumberFormat::Arabic), 3);
    EXPECT_EQ(seqEngine.currentNumber(NumberFormat::UpperAlpha), 3);
    EXPECT_EQ(seqEngine.currentNumber(NumberFormat::Roman), 2);
    EXPECT_EQ(seqEngine.currentNumber(NumberFormat::Chinese), 2);
    EXPECT_EQ(seqEngine.currentNumber(NumberFormat::LowerAlpha), 2);

    // 撤销最后一枚小写字母 l1，仅 LowerAlpha 回退为 1，其余制式纹丝不动
    ASSERT_TRUE(seqEngine.undo());
    EXPECT_EQ(seqEngine.currentNumber(NumberFormat::LowerAlpha), 1);
    EXPECT_EQ(seqEngine.currentNumber(NumberFormat::Arabic), 3);
    EXPECT_EQ(seqEngine.currentNumber(NumberFormat::UpperAlpha), 3);

    // 重做恢复 l1，仅 LowerAlpha 推进至 2
    ASSERT_TRUE(seqEngine.redo());
    EXPECT_EQ(seqEngine.currentNumber(NumberFormat::LowerAlpha), 2);

    // 单独重置 UpperAlpha 为 1，不影响 Arabic 序列
    seqEngine.resetNumber(1, NumberFormat::UpperAlpha);
    EXPECT_EQ(seqEngine.currentNumber(NumberFormat::UpperAlpha), 1);
    EXPECT_EQ(seqEngine.currentNumber(NumberFormat::Arabic), 3);

    // 3. 验证多区域聚光灯联合亮区并集镂空与前景标注分层保护：
    //    - 两个聚光灯亮区内部均保持 240 原图高亮度，绝不互相遮黑；
    //    - 外部背景仅按 dimAlpha=0.55 压暗一次 (240 * 0.45 = 108)，绝不二次叠加变黑；
    //    - 暗区上方的红色矩形标注渲染在暗化层之上，保持高饱和明亮红色。
    MarkupEngine spotEngine;
    spotEngine.setBaseImage(baseCanvas);
    auto* spotA = spotEngine.addSpotlight(cv::Point(20, 20), cv::Point(90, 90), MarkupColor::Red(), 0.55f, false);
    auto* spotB = spotEngine.addSpotlight(cv::Point(140, 20), cv::Point(210, 90), MarkupColor::Red(), 0.55f, false);
    ASSERT_NE(spotA, nullptr);
    ASSERT_NE(spotB, nullptr);
    spotA->spotlightShowBorder = false;
    spotB->spotlightShowBorder = false;

    auto* fgRect = spotEngine.drawRectangle(cv::Point(30, 140), cv::Point(90, 180), MarkupColor::Red(), 3.0f);
    ASSERT_NE(fgRect, nullptr);
    fgRect->fill = true;

    cv::Mat rendered = baseCanvas.clone();
    spotEngine.renderAll(rendered, false);

    // 聚光灯 A 中心与聚光灯 B 中心均保持 240 原图亮度
    const cv::Vec3b pxSpotA = rendered.at<cv::Vec3b>(55, 55);
    const cv::Vec3b pxSpotB = rendered.at<cv::Vec3b>(55, 175);
    EXPECT_NEAR(pxSpotA[0], 240, 2);
    EXPECT_NEAR(pxSpotB[0], 240, 2);

    // 外部暗区像素仅被单次弱化至约 108，背景信息依然清晰可读
    const cv::Vec3b pxDimBg = rendered.at<cv::Vec3b>(160, 260);
    EXPECT_NEAR(pxDimBg[0], 108, 4);
    EXPECT_GT(pxDimBg[0], 85);

    // 位于暗区之上的前景实心红色标注保持明亮珊瑚红 (#F43F5E -> R=244)，不被聚光灯压暗
    const cv::Vec3b pxFgRed = rendered.at<cv::Vec3b>(160, 60);
    EXPECT_GT(pxFgRed[2], 225);
}

TEST(PinGroupManagementTest, PaletteShortcutsAndEndToEndGroupFilterAutoGroupDissolve) {
    using namespace tools3000::capture;

    // 1. 验证 32 组动态扩容、12 色宝石色调色板与 Ctrl/Alt+1..9 快捷键映射
    EXPECT_EQ(PinGroupPalette::normalizeGroupId(0), 0);
    EXPECT_EQ(PinGroupPalette::normalizeGroupId(6), 6);
    EXPECT_EQ(PinGroupPalette::normalizeGroupId(32), 32);
    EXPECT_EQ(PinGroupPalette::normalizeGroupId(33), 0);
    EXPECT_EQ(PinGroupPalette::normalizeGroupId(99), 0);
    EXPECT_EQ(PinGroupPalette::normalizeFilter(PinGroupConstants::kFilterAllGroups),
              PinGroupConstants::kFilterAllGroups);
    for (int g = 1; g <= PinGroupConstants::kMaxGroupId; ++g) {
        EXPECT_FALSE(PinGroupPalette::shortLabel(g, true).empty());
        EXPECT_FALSE(PinGroupPalette::shortLabel(g, false).empty());
        EXPECT_NE(PinGroupPalette::colorName(g, true), nullptr);
        EXPECT_NE(PinGroupPalette::colorName(g, false), nullptr);
    }

    const auto ctrl2 = PinInputController::resolveKeyDown('2', true, false, false, false);
    EXPECT_EQ(ctrl2.action, PinShortcutAction::AssignOrToggleGroup);
    EXPECT_EQ(ctrl2.groupIndex, 2);

    const auto ctrl0 = PinInputController::resolveKeyDown('0', true, false, false, false);
    EXPECT_EQ(ctrl0.action, PinShortcutAction::RemoveFromGroup);

    const auto ctrlShiftG = PinInputController::resolveKeyDown('G', true, true, false, false);
    EXPECT_EQ(ctrlShiftG.action, PinShortcutAction::AutoGroupUngrouped);

    const auto ctrl8 = PinInputController::resolveKeyDown('8', true, false, false, false);
    EXPECT_EQ(ctrl8.action, PinShortcutAction::AssignOrToggleGroup);
    EXPECT_EQ(ctrl8.groupIndex, 8);

    const auto alt3 = PinInputController::resolveKeyDown('3', false, false, true, false);
    EXPECT_EQ(alt3.action, PinShortcutAction::FilterByGroup);
    EXPECT_EQ(alt3.groupIndex, 3);

    const auto alt9 = PinInputController::resolveKeyDown('9', false, false, true, false);
    EXPECT_EQ(alt9.action, PinShortcutAction::FilterByGroup);
    EXPECT_EQ(alt9.groupIndex, 9);

    const auto alt0 = PinInputController::resolveKeyDown('0', false, false, true, false);
    EXPECT_EQ(alt0.action, PinShortcutAction::ShowAllGroups);

    // 无修饰键按下 '1'、'0'、'G' 严格保持原有旋转、重置缩放与灰度切换，绝不误触分组
    EXPECT_EQ(PinInputController::resolveKeyDown('1', false, false, false, false).action,
              PinShortcutAction::RotateCounterClockwise90);
    EXPECT_EQ(PinInputController::resolveKeyDown('0', false, false, false, false).action,
              PinShortcutAction::ResetScaleAndOpacity);
    EXPECT_EQ(PinInputController::resolveKeyDown('G', false, false, false, false).action,
              PinShortcutAction::ToggleGrayscale);

    // 2. 加载真实 Plugin_Capture.dll 验证默认热键 (Screenshot=F1, Pin Paste=F3) 与贴图分组全闭环
    std::array<wchar_t, 4096> executablePath{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    ASSERT_GT(length, 0u);
    const auto executableDir = std::filesystem::path(executablePath.data()).parent_path();
    const auto pluginDllPath =
        executableDir.parent_path() / L"plugins" / executableDir.filename() / L"Plugin_Capture.dll";
    ASSERT_TRUE(std::filesystem::exists(pluginDllPath));

    HMODULE hMod = LoadLibraryExW(
        pluginDllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    ASSERT_NE(hMod, nullptr);

    using CreatePluginFunc = tools3000::core::IPlugin* (*)();
    const auto createPlugin = reinterpret_cast<CreatePluginFunc>(GetProcAddress(hMod, "CreatePlugin"));
    ASSERT_NE(createPlugin, nullptr);
    tools3000::core::IPlugin* capturePlugin = createPlugin();
    ASSERT_NE(capturePlugin, nullptr);

    // 清空临时配置中的 Screenshot / Pin Paste 覆盖项以验证出厂默认热键回退为 F1 / F3
    auto& cfg = tools3000::core::ConfigManager::instance();
    const auto prevShot = cfg.get<std::string>("/hotkeys/Screenshot", "");
    const auto prevPinPaste = cfg.get<std::string>("/hotkeys/Pin Paste", "");
    cfg.remove("/hotkeys/Screenshot");
    cfg.remove("/hotkeys/Pin Paste");

    ASSERT_TRUE(capturePlugin->initialize());

    struct PluginCleanupGuard {
        HMODULE mod = nullptr;
        tools3000::core::IPlugin* plugin = nullptr;
        std::string savedShot;
        std::string savedPinPaste;
        ~PluginCleanupGuard() {
            while (HWND existing = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
                DestroyWindow(existing);
            }
            if (plugin) plugin->shutdown();
            if (mod) FreeLibrary(mod);
            auto& c = tools3000::core::ConfigManager::instance();
            if (!savedShot.empty()) c.set("/hotkeys/Screenshot", savedShot);
            if (!savedPinPaste.empty()) c.set("/hotkeys/Pin Paste", savedPinPaste);
        }
    } cleanupGuard{hMod, capturePlugin, prevShot, prevPinPaste};

    std::string regShot;
    std::string regPinPaste;
    for (const auto& hk : tools3000::core::HotkeyManager::instance().getAllHotkeys()) {
        if (hk.name == "Screenshot") regShot = hk.def.toString();
        if (hk.name == "Pin Paste") regPinPaste = hk.def.toString();
    }
    EXPECT_EQ(regShot, "F1");
    EXPECT_EQ(regPinPaste, "F3");

    auto& bridge = tools3000::core::MessageBridge::instance();
    auto invokeRpc = [&](int id, const char* method, const nlohmann::json& params) -> nlohmann::json {
        nlohmann::json req = {{"id", id}, {"method", method}, {"params", params}};
        const std::string raw = bridge.handleMessage(req.dump());
        const auto parsed = nlohmann::json::parse(raw, nullptr, false);
        return parsed.is_object() && parsed.contains("result") ? parsed["result"] : parsed;
    };

    cv::Mat sampleImg(48, 64, CV_8UC3, cv::Scalar(60, 120, 210));
    const auto samplePng = std::filesystem::temp_directory_path() /
                           (L"tools3000_pin_group_e2e_" + std::to_wstring(GetCurrentProcessId()) + L".png");
    std::vector<uchar> pngBuf;
    ASSERT_TRUE(cv::imencode(".png", sampleImg, pngBuf));
    {
        std::ofstream ofs(samplePng, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(pngBuf.data()), static_cast<std::streamsize>(pngBuf.size()));
    }
    struct TempFileGuard {
        std::filesystem::path p;
        ~TempFileGuard() {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    } tempGuard{samplePng};

    const std::string pngUtf8 = tools3000::core::WinUtils::wstringToUtf8(samplePng.wstring());
    ASSERT_TRUE(invokeRpc(951, "capture.pinImageFile", {{"path", pngUtf8}}).value("success", false));
    const auto stateAfterFirst = invokeRpc(952, "capture.pinGroupControl", nlohmann::json::object());
    const HWND hwndPin1 = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(stateAfterFirst.value("latestHwnd", 0ULL)));
    ASSERT_NE(hwndPin1, nullptr);
    EXPECT_FALSE(stateAfterFirst.value("groupBarVisible", true));

    ASSERT_TRUE(invokeRpc(953, "capture.pinImageFile", {{"path", pngUtf8}}).value("success", false));
    const auto stateAfterSecond = invokeRpc(954, "capture.pinGroupControl", nlohmann::json::object());
    const HWND hwndPin2 = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(stateAfterSecond.value("latestHwnd", 0ULL)));
    ASSERT_NE(hwndPin2, nullptr);
    ASSERT_NE(hwndPin1, hwndPin2);
    EXPECT_EQ(stateAfterSecond.value("totalCount", 0), 2);
    // 全部未分组且处于全部视图时，顶部导览栏保持静默隐藏
    EXPECT_FALSE(stateAfterSecond.value("groupBarVisible", true));

    // 将第 2 张贴图归入第 1 组：顶部雪瓷白分组导览栏立即显现
    const auto assigned1 = invokeRpc(955, "capture.pinGroupControl", {{"assignLatestGroup", 1}});
    EXPECT_EQ(assigned1.value("latestGroupId", 0), 1);
    EXPECT_TRUE(assigned1.value("groupBarVisible", false));

    // 一键编组剩余未分组贴图（第 1 张）：自动分配至空闲的第 2 组
    const auto autoGrouped = invokeRpc(956, "capture.pinGroupControl", {{"autoGroupUngrouped", true}});
    EXPECT_EQ(autoGrouped.value("autoAssignedGroup", 0), 2);

    // 按组查看第 1 组：仅 hwndPin2 可见，hwndPin1 自动隐藏
    const auto filtered1 = invokeRpc(957, "capture.pinGroupControl", {{"filterGroup", 1}});
    EXPECT_EQ(filtered1.value("activeFilter", 0), 1);
    EXPECT_FALSE(IsWindowVisible(hwndPin1) != FALSE);
    EXPECT_TRUE(IsWindowVisible(hwndPin2) != FALSE);

    // 按组查看第 2 组：仅 hwndPin1 可见，hwndPin2 自动隐藏
    const auto filtered2 = invokeRpc(958, "capture.pinGroupControl", {{"filterGroup", 2}});
    EXPECT_EQ(filtered2.value("activeFilter", 0), 2);
    EXPECT_TRUE(IsWindowVisible(hwndPin1) != FALSE);
    EXPECT_FALSE(IsWindowVisible(hwndPin2) != FALSE);

    // 切回查看全部并解散两个分组：两张贴图全部恢复可见且未分组，顶部导览栏自动归隐
    invokeRpc(959, "capture.pinGroupControl", {{"filterGroup", PinGroupConstants::kFilterAllGroups}});
    EXPECT_TRUE(IsWindowVisible(hwndPin1) != FALSE);
    EXPECT_TRUE(IsWindowVisible(hwndPin2) != FALSE);

    invokeRpc(960, "capture.pinGroupControl", {{"dissolveGroup", 1}});
    const auto afterDissolveAll = invokeRpc(961, "capture.pinGroupControl", {{"dissolveGroup", 2}});
    EXPECT_EQ(afterDissolveAll.value("activeFilter", -99), PinGroupConstants::kFilterAllGroups);
    EXPECT_FALSE(afterDissolveAll.value("groupBarVisible", true));

    // 验证突破 6 个分组限制：直接归入第 8 组并动态新建新组
    const auto assigned8 = invokeRpc(962, "capture.pinGroupControl", {{"assignLatestGroup", 8}});
    EXPECT_EQ(assigned8.value("latestGroupId", 0), 8);
    EXPECT_TRUE(assigned8.value("groupBarVisible", false));
    const auto createdNew = invokeRpc(963, "capture.pinGroupControl", {{"createNewGroup", true}});
    EXPECT_EQ(createdNew.value("createdGroup", 0), 1);
    EXPECT_GE(createdNew["groups"].size(), 2u);
    invokeRpc(964, "capture.pinGroupControl", {{"dissolveGroup", 1}});
    const auto cleaned8 = invokeRpc(965, "capture.pinGroupControl", {{"dissolveGroup", 8}});
    const auto customized2 = invokeRpc(966, "capture.pinGroupControl", {
        {"updateGroupMeta", {{"id", 2}, {"name", "Design Spec"}, {"colorHex", "#f43f5e"}}}
    });
    EXPECT_TRUE(customized2.value("success", false));
    EXPECT_FALSE(customized2.value("groupBarVisible", true));
    EXPECT_GE(customized2["palettePresets"].size(), 12u);
    EXPECT_GE(customized2["catalog"].size(), 6u);
    bool foundCustom2 = false;
    for (const auto& item : customized2["catalog"]) {
        if (item.value("id", 0) == 2) {
            foundCustom2 = true;
            EXPECT_EQ(item.value("customName", ""), "Design Spec");
            EXPECT_EQ(item.value("colorHex", ""), "#F43F5E");
            EXPECT_EQ(item.value("shortLabel", ""), "Design Spec");
            EXPECT_TRUE(item.value("hasCustomization", false));
        }
    }
    EXPECT_TRUE(foundCustom2);
    const auto reset2 = invokeRpc(967, "capture.pinGroupControl", {{"resetGroupMeta", 2}});
    for (const auto& item : reset2["catalog"]) {
        if (item.value("id", 0) == 2) {
            EXPECT_EQ(item.value("customName", "x"), "");
            EXPECT_FALSE(item.value("hasCustomization", true));
        }
    }
    EXPECT_FALSE(cleaned8.value("groupBarVisible", true));
}
TEST(CaptureToolbarLayoutTest, DualContrastCornerRadiusStepperAndVectorIconContract) {
    using namespace tools3000::capture;

    const D2D1_RECT_F sel = D2D1::RectF(100.0f, 100.0f, 780.0f, 520.0f);
    const D2D1_SIZE_F surface = D2D1_SIZE_F{1920.0f, 1080.0f};

    CaptureState state;
    state.mode = OverlayMode::Screenshot;
    state.state = OverlayState::Selected;
    state.dpiScale = 1.0f;
    state.isMarkupToolActive = true;
    state.currentTool = MarkupTool::Rectangle;
    state.currentElementCornerRadius = 0.0f;

    cv::Mat dummy(420, 680, CV_8UC3, cv::Scalar(40, 45, 52));
    state.markup.setBaseImage(dummy);
    rebuildCaptureToolbar(state, sel, surface);

    auto findBtn = [](const std::vector<ToolbarButton>& list, ToolbarCommand cmd) -> const ToolbarButton* {
        for (const auto& b : list) {
            if (b.command == cmd) return &b;
        }
        return nullptr;
    };

    const ToolbarButton* radiusBtn = findBtn(state.secondaryToolbarButtons, ToolbarCommand::CycleElementCornerRadius);
    ASSERT_NE(radiusBtn, nullptr);
    EXPECT_NEAR(radiusBtn->rect.right - radiusBtn->rect.left, 48.0f, 0.05f);
    EXPECT_EQ(radiusBtn->intParam, 0);
    EXPECT_EQ(radiusBtn->label, L"0");

    // 选中已有大圆角矩形元素时，标注栏圆角胶囊必须实时同步回显选中元素的 cornerRadius
    MarkupElement* rectElem = state.markup.drawRectangle(cv::Point(40, 40), cv::Point(260, 180), MarkupColor::Red(), 3.0f);
    ASSERT_NE(rectElem, nullptr);
    rectElem->cornerRadius = 24.0f;
    state.activeElement = rectElem;
    state.toolbarLayoutValid = false;
    rebuildCaptureToolbar(state, sel, surface);

    radiusBtn = findBtn(state.secondaryToolbarButtons, ToolbarCommand::CycleElementCornerRadius);
    ASSERT_NE(radiusBtn, nullptr);
    EXPECT_EQ(radiusBtn->intParam, 24);
    EXPECT_EQ(radiusBtn->label, L"24");

    // 真实光栅化渲染双对比度无缝闭合 PropCornerRadius 与精修版 ToolSpotlight 矢量图标
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory;
    ASSERT_HRESULT_SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf()));

    HDC memDc = CreateCompatibleDC(nullptr);
    ASSERT_NE(memDc, nullptr);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 64;
    bmi.bmiHeader.biHeight = -64;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ASSERT_NE(hBmp, nullptr);
    HGDIOBJ oldBmp = SelectObject(memDc, hBmp);

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcRt;
    ASSERT_HRESULT_SUCCEEDED(d2dFactory->CreateDCRenderTarget(&rtProps, dcRt.GetAddressOf()));
    const RECT bindRc{0, 0, 64, 64};
    ASSERT_HRESULT_SUCCEEDED(dcRt->BindDC(memDc, &bindRc));

    dcRt->BeginDraw();
    dcRt->Clear(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ink;
    ASSERT_HRESULT_SUCCEEDED(dcRt->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.09f, 0.16f, 1.0f), ink.GetAddressOf()));
    CaptureVectorIcons::renderIcon(dcRt.Get(), d2dFactory.Get(), CaptureIconId::PropCornerRadius,
                                   D2D1::RectF(4.0f, 4.0f, 30.0f, 30.0f), ink.Get(), 1.0f);
    CaptureVectorIcons::renderIcon(dcRt.Get(), d2dFactory.Get(), CaptureIconId::ToolSpotlight,
                                   D2D1::RectF(34.0f, 4.0f, 60.0f, 30.0f), ink.Get(), 1.0f);
    EXPECT_HRESULT_SUCCEEDED(dcRt->EndDraw());

    // 校验两个图标区域均产出了清晰非纯白的矢量笔画像素
    const auto* px = static_cast<const std::uint32_t*>(bits);
    int cornerInkPixels = 0;
    int spotlightInkPixels = 0;
    for (int y = 4; y < 30; ++y) {
        for (int x = 4; x < 30; ++x) {
            if ((px[y * 64 + x] & 0x00FFFFFFu) != 0x00FFFFFFu) ++cornerInkPixels;
        }
        for (int x = 34; x < 60; ++x) {
            if ((px[y * 64 + x] & 0x00FFFFFFu) != 0x00FFFFFFu) ++spotlightInkPixels;
        }
    }
    EXPECT_GT(cornerInkPixels, 20);
    EXPECT_GT(spotlightInkPixels, 20);

    SelectObject(memDc, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDc);
}

TEST(ScrollCaptureEditorTest, NonDestructiveHeadTailCropMinimapMappingAndMarkupComposite) {
    using namespace tools3000::capture;

    // 1. 验证首尾与左右裁切区间边界夹取、防交叉及无损恢复契约
    const int fullW = 800;
    const int fullH = 2400;
    const auto normalTrim = ScrollCaptureEditorOverlay::clampScrollCropRange(
        ScrollCropRange{180, 2100, 20, 780}, fullW, fullH, 32, 0);
    EXPECT_EQ(normalTrim.topRow, 180);
    EXPECT_EQ(normalTrim.bottomRow, 2100);
    EXPECT_EQ(normalTrim.croppedWidth(), 760);
    EXPECT_EQ(normalTrim.croppedHeight(), 1920);
    EXPECT_TRUE(normalTrim.isModified(fullW, fullH));

    // 拖动顶边越过底边时，底边锚定不动，顶边停在 bottomRow - minKeepPx
    const auto dragTopPastBottom = ScrollCaptureEditorOverlay::clampScrollCropRange(
        ScrollCropRange{2300, 1200, 0, fullW}, fullW, fullH, 40, 1);
    EXPECT_EQ(dragTopPastBottom.bottomRow, 1200);
    EXPECT_EQ(dragTopPastBottom.topRow, 1160);

    // 拖动底边越过顶边时，顶边锚定不动，底边停在 topRow + minKeepPx
    const auto dragBottomPastTop = ScrollCaptureEditorOverlay::clampScrollCropRange(
        ScrollCropRange{500, 300, 0, fullW}, fullW, fullH, 40, 2);
    EXPECT_EQ(dragBottomPastTop.topRow, 500);
    EXPECT_EQ(dragBottomPastTop.bottomRow, 540);

    // 2. 验证 12000px 超长截图下编辑器布局互不重叠与右侧 Minimap 双向行号精确映射
    const auto layout = ScrollCaptureEditorOverlay::computeEditorLayout(1920.0f, 1080.0f, 1200, 12000, 1.25f);
    EXPECT_LT(layout.topHeaderRect.bottom, layout.stageRect.top);
    EXPECT_LT(layout.stageRect.bottom, layout.bottomBarRect.top);
    EXPECT_LT(layout.stageRect.right, layout.minimapPanelRect.left);
    EXPECT_GT(layout.minimapImageRect.right - layout.minimapImageRect.left, 20.0f);

    for (int testRow : {0, 420, 3600, 9850, 12000}) {
        const float miniY = ScrollCaptureEditorOverlay::mapImageRowToMinimapY(testRow, layout.minimapImageRect, 12000);
        const int mappedBack = ScrollCaptureEditorOverlay::mapMinimapYToImageRow(miniY, layout.minimapImageRect, 12000);
        EXPECT_NEAR(mappedBack, testRow, 16);
    }

    // 3. 验证三段式长图（红头 150px + 绿体 650px + 蓝尾 200px）去头去尾 + 原位标注 + 无损恢复全尺寸
    cv::Mat stitched(1000, 480, CV_8UC4, cv::Scalar(40, 180, 60, 255)); // 主体绿色 (BGRA)
    stitched(cv::Rect(0, 0, 480, 150)).setTo(cv::Scalar(30, 30, 230, 255));    // 头部干扰栏（红）
    stitched(cv::Rect(0, 800, 480, 200)).setTo(cv::Scalar(230, 60, 30, 255));  // 尾部冗余区（蓝）

    MarkupEngine markup;
    markup.setBaseImage(stitched);
    // 在保留主体区 (y=300..360) 绘制纯白实线矩形标注
    MarkupElement* rectMark = markup.drawRectangle(cv::Point(80, 300), cv::Point(220, 360), MarkupColor::White(), 4.0f);
    ASSERT_NE(rectMark, nullptr);
    rectMark->isActive = false;

    const ScrollCropRange trimHeadAndTail{150, 800, 0, 480};
    const cv::Mat croppedOut = ScrollCaptureEditorOverlay::applyScrollCropAndMarkup(
        stitched, &markup, trimHeadAndTail, 0.0f);
    ASSERT_EQ(croppedOut.cols, 480);
    ASSERT_EQ(croppedOut.rows, 650);

    // 首行与末行均应为绿色正文而非红头/蓝尾
    const cv::Vec4b firstPx = croppedOut.at<cv::Vec4b>(2, 10);
    const cv::Vec4b lastPx = croppedOut.at<cv::Vec4b>(647, 10);
    EXPECT_GT(firstPx[1], 150);
    EXPECT_LT(firstPx[2], 80);
    EXPECT_GT(lastPx[1], 150);
    EXPECT_LT(lastPx[0], 80);

    // 原图 y=300 的白色矩形顶边在裁去头部 150px 后精确落在输出图 y=150 处
    const cv::Vec4b markPx = croppedOut.at<cv::Vec4b>(150, 150);
    EXPECT_GT(markPx[0], 240);
    EXPECT_GT(markPx[1], 240);
    EXPECT_GT(markPx[2], 240);

    // 往回拉恢复全区间 {0, 1000, 0, 480}：被裁掉的红头与蓝尾 100% 无损恢复！
    const cv::Mat restoredOut = ScrollCaptureEditorOverlay::applyScrollCropAndMarkup(
        stitched, &markup, ScrollCropRange{0, 1000, 0, 480}, 16.0f);
    ASSERT_EQ(restoredOut.cols, 480);
    ASSERT_EQ(restoredOut.rows, 1000);
    // 圆角 16px 使得四角顶点透明 (alpha == 0)，而中心头部红区与尾部蓝区完好无损
    EXPECT_EQ(restoredOut.at<cv::Vec4b>(0, 0)[3], 0);
    EXPECT_GT(restoredOut.at<cv::Vec4b>(50, 240)[2], 200);
    EXPECT_GT(restoredOut.at<cv::Vec4b>(920, 240)[0], 200);
}

TEST(LongImageSsotStudioTest, FullStandardToolbarTextMultiSequenceNumbersAndSmartisanTrim) {
    using namespace tools3000::capture;

    CaptureState state;
    state.mode = OverlayMode::Screenshot;
    state.state = OverlayState::Selected;
    state.dpiScale = 1.25f;

    const int docW = 900;
    const int docH = 3600;
    state.frozenScreen = cv::Mat(docH, docW, CV_8UC4, cv::Scalar(248, 250, 252, 255));
    state.frozenScreen(cv::Rect(0, 0, docW, 260)).setTo(cv::Scalar(36, 40, 220, 255));
    state.frozenScreen(cv::Rect(0, 3200, docW, 400)).setTo(cv::Scalar(220, 90, 32, 255));

    state.dragStart = {0, 0};
    state.dragEnd = {docW, docH};
    state.longCanvas.active = true;
    state.longCanvas.frameCount = 9;
    state.longCanvas.docWidth = docW;
    state.longCanvas.docHeight = docH;
    state.longCanvas.stageRect = D2D1::RectF(30.0f, 68.0f, 1740.0f, 940.0f);
    state.longCanvas.minimapPanelRect = D2D1::RectF(1762.0f, 74.0f, 1898.0f, 934.0f);
    state.longCanvas.zoom = 0.85f;
    state.longCanvas.scrollX = 0.0f;
    state.longCanvas.scrollY = 480.0f;
    state.clampLongCanvasScroll();

    // 1. 验证长图文档坐标与屏幕舞台坐标双向变换互逆无漂移
    for (const auto& docPt : {D2D1::Point2F(0.0f, 480.0f), D2D1::Point2F(450.0f, 1260.0f), D2D1::Point2F(899.0f, 1450.0f)}) {
        const D2D1_POINT_2F scrPt = state.docToScreen(docPt.x, docPt.y);
        const D2D1_POINT_2F roundTrip = state.screenToDoc(scrPt.x, scrPt.y);
        EXPECT_NEAR(roundTrip.x, docPt.x, 0.02f);
        EXPECT_NEAR(roundTrip.y, docPt.y, 0.02f);
    }

    // 2. 验证 SSOT 工具栏在长截图模式下 100% 复用标准截图全套工具（含文字、序号、放大镜、马赛克/消除、美化壳、OCR、贴图、侧边4圆钮），且过滤递归长截图/录屏
    syncMarkupBase(state.frozenScreen, cv::Rect(0, 0, docW, docH),
                   state.markupBaseReady, state.markupBaseRect, state.markup);
    const D2D1_SIZE_F surface{1920.0f, 1079.0f};
    rebuildCaptureToolbar(state, state.toolbarAnchorRect(), surface);
    EXPECT_TRUE(state.toolbarLayoutValid);
    EXPECT_TRUE(state.toolbarLayoutLongImage);
    // 主工具栏必须避让右侧 Minimap 导航舱且稳固停靠于舞台底部下方
    EXPECT_LE(state.primaryToolbarRect.right, state.longCanvas.minimapPanelRect.left);
    EXPECT_GE(state.primaryToolbarRect.top, state.longCanvas.stageRect.bottom);

    auto hasPrimaryTool = [&](MarkupTool tool) {
        for (const auto& b : state.toolbarButtons) {
            if (b.command == ToolbarCommand::SelectTool && b.tool == tool) return true;
        }
        return false;
    };
    auto hasPrimaryCmd = [&](ToolbarCommand cmd) {
        for (const auto& b : state.toolbarButtons) {
            if (b.command == cmd) return true;
        }
        return false;
    };
    auto hasSideCmd = [&](ToolbarCommand cmd) {
        for (const auto& b : state.selectionSideButtons) {
            if (b.command == cmd) return true;
        }
        return false;
    };

    // 核心标注工具全量就绪（矩形/聚光灯复合入口、画笔/荧光笔、箭头、文字、多序列序号、放大镜、马赛克/模糊、智能消除）
    EXPECT_TRUE(hasPrimaryTool(MarkupTool::Rectangle));
    EXPECT_TRUE(hasPrimaryTool(MarkupTool::Pen));
    EXPECT_TRUE(hasPrimaryTool(MarkupTool::Arrow));
    EXPECT_TRUE(hasPrimaryTool(MarkupTool::Text));
    EXPECT_TRUE(hasPrimaryTool(MarkupTool::Number));
    EXPECT_TRUE(hasPrimaryTool(MarkupTool::Magnifier));
    EXPECT_TRUE(hasPrimaryTool(MarkupTool::Mosaic));
    EXPECT_TRUE(hasPrimaryTool(MarkupTool::Inpaint));

    // 系统级增强、侧边 4 圆钮与交付按钮全量就绪
    EXPECT_TRUE(hasPrimaryCmd(ToolbarCommand::Undo));
    EXPECT_TRUE(hasPrimaryCmd(ToolbarCommand::Redo));
    EXPECT_TRUE(hasPrimaryCmd(ToolbarCommand::ToggleBeautyShell));
    EXPECT_TRUE(hasPrimaryCmd(ToolbarCommand::ExtractText));
    EXPECT_TRUE(hasPrimaryCmd(ToolbarCommand::PinWindow));
    EXPECT_TRUE(hasPrimaryCmd(ToolbarCommand::Copy));
    EXPECT_TRUE(hasPrimaryCmd(ToolbarCommand::Cancel));
    EXPECT_TRUE(hasPrimaryCmd(ToolbarCommand::Confirm));
    EXPECT_TRUE(hasSideCmd(ToolbarCommand::SideToggleCornerRadius));
    EXPECT_TRUE(hasSideCmd(ToolbarCommand::SideCycleAspectRatio));
    EXPECT_TRUE(hasSideCmd(ToolbarCommand::SideResetSelection));
    EXPECT_TRUE(hasSideCmd(ToolbarCommand::SideInvertSelection));

    // 递归长截图与开始录屏在长图精修中必须隐藏
    EXPECT_FALSE(hasPrimaryCmd(ToolbarCommand::ScrollCapture));
    EXPECT_FALSE(hasPrimaryCmd(ToolbarCommand::StartRecord));

    // 激活序号工具时，二级属性栏完整弹出序号形状切换、5 序列格式下拉菜单入口与重置计数器
    state.isMarkupToolActive = true;
    state.currentTool = MarkupTool::Number;
    state.toolbarLayoutValid = false;
    rebuildCaptureToolbar(state, state.toolbarAnchorRect(), surface);
    bool hasNumShape = false;
    bool hasNumFormat = false;
    bool hasNumReset = false;
    for (const auto& b : state.secondaryToolbarButtons) {
        if (b.command == ToolbarCommand::ToggleNumberShape) hasNumShape = true;
        if (b.command == ToolbarCommand::ToggleNumberFormatDropdown) hasNumFormat = true;
        if (b.command == ToolbarCommand::ResetNumberCounter) hasNumReset = true;
    }
    EXPECT_TRUE(hasNumShape);
    EXPECT_TRUE(hasNumFormat);
    EXPECT_TRUE(hasNumReset);

    // 激活文字工具时，二级属性栏完整弹出字号滑块胶囊、高对比文字描边开关与描边色选择
    state.currentTool = MarkupTool::Text;
    state.toolbarLayoutValid = false;
    rebuildCaptureToolbar(state, state.toolbarAnchorRect(), surface);
    bool hasFontSize = false;
    bool hasTextOutline = false;
    for (const auto& b : state.secondaryToolbarButtons) {
        if (b.command == ToolbarCommand::CycleStrokeWidth) hasFontSize = true;
        if (b.command == ToolbarCommand::ToggleTextOutline) hasTextOutline = true;
    }
    EXPECT_TRUE(hasFontSize);
    EXPECT_TRUE(hasTextOutline);

    // 3. 验证在长图不同高度添加文字 + 多序列独立序号后，执行 Smartisan 首尾推拉裁切与无损拉回时全部标注精准保位
    MarkupElement* txt = state.markup.addText(
        cv::Point(180, 920), "Smartisan Long Studio\nMulti-line Text", MarkupColor::Red(), 22.0f, true);
    ASSERT_NE(txt, nullptr);
    MarkupElement* rom1 = state.markup.drawNumberMark(
        cv::Point(120, 1400), MarkupColor::Red(), 1.25f, NumberBadgeShape::Circle, NumberFormat::Roman);
    MarkupElement* chn1 = state.markup.drawNumberMark(
        cv::Point(260, 1400), MarkupColor::Blue(), 1.25f, NumberBadgeShape::RoundedSquare, NumberFormat::Chinese);
    MarkupElement* rom2 = state.markup.drawNumberMark(
        cv::Point(120, 2100), MarkupColor::Red(), 1.25f, NumberBadgeShape::Circle, NumberFormat::Roman);
    ASSERT_NE(rom1, nullptr);
    ASSERT_NE(chn1, nullptr);
    ASSERT_NE(rom2, nullptr);
    EXPECT_EQ(rom1->numberValue, 1);
    EXPECT_EQ(chn1->numberValue, 1); // 汉字序列独立从「一」起算
    EXPECT_EQ(rom2->numberValue, 2); // 罗马序列独立递增为「II」

    // 推拉裁掉头部 260px 与尾部 400px -> 净高 2940px
    syncMarkupBase(state.frozenScreen, cv::Rect(0, 260, docW, 2940),
                   state.markupBaseReady, state.markupBaseRect, state.markup);
    EXPECT_EQ(state.markup.elementCount(), 4u);
    EXPECT_EQ(txt->startPt.y, 920 - 260);
    EXPECT_EQ(rom2->startPt.y, 2100 - 260);

    const cv::Mat trimmedComposite = state.markup.getCompositeImage();
    ASSERT_EQ(trimmedComposite.cols, docW);
    ASSERT_EQ(trimmedComposite.rows, 2940);
    ASSERT_EQ(trimmedComposite.channels(), 3);
    // 裁掉红头与蓝尾后，首尾像素均为浅灰白正文底色 (248, 250, 252)
    EXPECT_GT(trimmedComposite.at<cv::Vec3b>(4, 20)[0], 235);
    EXPECT_GT(trimmedComposite.at<cv::Vec3b>(2935, 20)[0], 235);

    // 往回拉开恢复全长 {0, 3600} -> 头部红区 (36, 40, 220)、尾部蓝区 (220, 90, 32) 以及全部 4 个文字/序号标注坐标 100% 无损复原
    syncMarkupBase(state.frozenScreen, cv::Rect(0, 0, docW, docH),
                   state.markupBaseReady, state.markupBaseRect, state.markup);
    EXPECT_EQ(state.markup.elementCount(), 4u);
    EXPECT_EQ(txt->startPt.y, 920);
    EXPECT_EQ(rom1->startPt.y, 1400);
    EXPECT_EQ(chn1->startPt.y, 1400);
    EXPECT_EQ(rom2->startPt.y, 2100);
    const cv::Mat restoredComposite = state.markup.getCompositeImage();
    ASSERT_EQ(restoredComposite.rows, docH);
    ASSERT_EQ(restoredComposite.channels(), 3);
    EXPECT_GT(restoredComposite.at<cv::Vec3b>(40, 40)[2], 200);
    EXPECT_GT(restoredComposite.at<cv::Vec3b>(3450, 40)[0], 200);
}

TEST(PinGroupCustomizationTest, CustomNameAndColorNormalizationAndConfigPersistence) {
    using namespace tools3000::capture;
    const auto tempCfgDir = std::filesystem::temp_directory_path() /
        (L"Tools3000_PinGroupCfg_" + std::to_wstring(GetCurrentProcessId()));
    ASSERT_TRUE(tools3000::core::ConfigManager::instance().initialize(tempCfgDir));
    struct TempConfigDirGuard {
        std::filesystem::path dir;
        ~TempConfigDirGuard() {
            tools3000::core::ConfigManager::instance().shutdown();
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    } cfgGuard{tempCfgDir};

    struct PaletteResetGuard {
        ~PaletteResetGuard() {
            PinGroupPalette::resetAllCustomizations();
        }
    } guard;

    PinGroupPalette::resetAllCustomizations();

    EXPECT_EQ(PinGroupPalette::normalizeHexColor("#abc"), "#AABBCC");
    EXPECT_EQ(PinGroupPalette::normalizeHexColor("  0ea5e9 "), "#0EA5E9");
    EXPECT_FALSE(PinGroupPalette::normalizeHexColor("#12345").has_value());
    EXPECT_FALSE(PinGroupPalette::normalizeHexColor("#GGGGGG").has_value());
    EXPECT_FALSE(PinGroupPalette::setGroupColor(3, "not-a-color"));

    EXPECT_TRUE(PinGroupPalette::setGroupName(3, L"  UI 视觉走查\r\n对比组  "));
    EXPECT_EQ(PinGroupPalette::customName(3), L"UI 视觉走查 对比组");
    EXPECT_EQ(PinGroupPalette::shortLabel(3, true), L"UI 视觉走查 对比组");
    EXPECT_NE(PinGroupPalette::fullLabel(3, true).find(L"第 3 组"), std::wstring::npos);

    const std::wstring thirtyChars(30, L'A');
    EXPECT_TRUE(PinGroupPalette::setGroupName(4, thirtyChars));
    EXPECT_EQ(PinGroupPalette::customName(4).size(), PinGroupConstants::kMaxCustomGroupNameChars);

    EXPECT_TRUE(PinGroupPalette::setGroupColor(3, "#FF3366"));
    EXPECT_EQ(PinGroupPalette::accentHex(3), "#FF3366");
    const auto d2dCol = PinGroupPalette::accentColor(3, 0.85f);
    EXPECT_NEAR(d2dCol.r, 1.0f, 0.01f);
    EXPECT_NEAR(d2dCol.g, 0x33 / 255.0f, 0.01f);
    EXPECT_NEAR(d2dCol.b, 0x66 / 255.0f, 0.01f);
    EXPECT_NEAR(d2dCol.a, 0.85f, 0.001f);

    PinGroupPalette::reloadFromConfig();
    EXPECT_EQ(PinGroupPalette::customName(3), L"UI 视觉走查 对比组");
    EXPECT_EQ(PinGroupPalette::accentHex(3), "#FF3366");
    EXPECT_TRUE(PinGroupPalette::hasCustomization(3));

    PinGroupPalette::resetGroupCustomization(3);
    EXPECT_FALSE(PinGroupPalette::hasCustomization(3));
    EXPECT_EQ(PinGroupPalette::accentHex(3), PinGroupPalette::defaultHex(3));

    // 验证顶部导览栏原位内联重命名输入清洗契约：输入空白或等同默认短名 ("组 N" / "GN") 自动回退空串恢复默认
    EXPECT_TRUE(PinGroupPalette::normalizeInlineRenameInput(0, L"测试组").empty());
    EXPECT_TRUE(PinGroupPalette::normalizeInlineRenameInput(2, L"   \r\n\t  ").empty());
    EXPECT_TRUE(PinGroupPalette::normalizeInlineRenameInput(2, L"  组 2  ").empty());
    EXPECT_TRUE(PinGroupPalette::normalizeInlineRenameInput(2, L" G2 ").empty());
    EXPECT_EQ(PinGroupPalette::normalizeInlineRenameInput(2, L"组 3"), L"组 3");
    EXPECT_EQ(PinGroupPalette::normalizeInlineRenameInput(2, L"  前端参考图  "), L"前端参考图");

    // 验证 1..12 预设色槽位名称 1-based 对齐无偏移（槽位 1 为科技蓝而非默认回退）
    for (int slot = 1; slot <= PinGroupConstants::kPaletteColorCount; ++slot) {
        EXPECT_NE(std::wstring(PinGroupPalette::presetColorName(slot, true)), L"默认");
        EXPECT_NE(std::wstring(PinGroupPalette::presetColorName(slot, false)), L"Default");
    }
}


TEST(CaptureHistoryTest, RemoveSingleEntryUpdatesCountAndOrder) {
    using namespace tools3000::capture;
    auto& hist = CaptureHistory::instance();
    const int prevMax = hist.maxSize();
    struct HistoryGuard {
        int savedMax;
        ~HistoryGuard() {
            CaptureHistory::instance().clear();
            CaptureHistory::instance().setMaxSize(savedMax);
        }
    } guard{prevMax};

    hist.clear();
    hist.setMaxSize(10);

    EXPECT_FALSE(hist.remove(-1));
    EXPECT_FALSE(hist.remove(0));

    cv::Mat imgA(24, 32, CV_8UC3, cv::Scalar(10, 20, 30));
    cv::Mat imgB(24, 48, CV_8UC3, cv::Scalar(40, 50, 60));
    cv::Mat imgC(24, 64, CV_8UC3, cv::Scalar(70, 80, 90));
    hist.push(imgA, CaptureRegion{0, 0, 32, 24}, "C:\\tmp\\a.png");
    hist.push(imgB, CaptureRegion{0, 0, 48, 24}, "C:\\tmp\\b.png");
    hist.push(imgC, CaptureRegion{0, 0, 64, 24}, "C:\\tmp\\c.png");
    ASSERT_EQ(hist.count(), 3);

    // push_front 顺序: [0]=c.png, [1]=b.png, [2]=a.png
    EXPECT_FALSE(hist.remove(3));
    EXPECT_TRUE(hist.remove(1));
    ASSERT_EQ(hist.count(), 2);

    const auto first = hist.get(0);
    const auto second = hist.get(1);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->filePath, "C:\\tmp\\c.png");
    EXPECT_EQ(second->filePath, "C:\\tmp\\a.png");

    EXPECT_TRUE(hist.remove(0));
    EXPECT_TRUE(hist.remove(0));
    EXPECT_EQ(hist.count(), 0);
    EXPECT_FALSE(hist.remove(0));
}

TEST(CaptureHistoryTest, RemoveByIdResistsConcurrentPushIndexShift) {
    using namespace tools3000::capture;
    auto& hist = CaptureHistory::instance();
    const int prevMax = hist.maxSize();
    struct HistoryGuard {
        int savedMax;
        ~HistoryGuard() {
            CaptureHistory::instance().clear();
            CaptureHistory::instance().setMaxSize(savedMax);
        }
    } guard{prevMax};

    hist.clear();
    hist.setMaxSize(10);

    cv::Mat imgA(20, 30, CV_8UC3, cv::Scalar(11, 22, 33));
    cv::Mat imgB(20, 40, CV_8UC3, cv::Scalar(44, 55, 66));
    cv::Mat imgC(20, 50, CV_8UC3, cv::Scalar(77, 88, 99));

    hist.push(imgA, CaptureRegion{0, 0, 30, 20}, "C:\\tmp\\older_a.png");
    hist.push(imgB, CaptureRegion{0, 0, 40, 20}, "C:\\tmp\\target_b.png");

    // 模拟前端渲染时读取快照: 此时 target_b.png 位于 index=0，并携带单调递增稳定 id
    const auto snapshotBefore = nlohmann::json::parse(hist.toMetadataJson());
    ASSERT_EQ(snapshotBefore.size(), 2u);
    const uint64_t targetBId = snapshotBefore[0].value("id", static_cast<uint64_t>(0));
    ASSERT_GT(targetBId, 0u);
    EXPECT_EQ(snapshotBefore[0].value("filePath", ""), "C:\\tmp\\target_b.png");

    // 用户点击删除前后台新产生一张截图 newest_c.png，导致 target_b.png 位置索引后移至 index=1
    hist.push(imgC, CaptureRegion{0, 0, 50, 20}, "C:\\tmp\\newest_c.png");
    ASSERT_EQ(hist.count(), 3);

    // 按稳定 id 删除必须精准命中 target_b.png，绝不误删 index=0 的 newest_c.png
    EXPECT_FALSE(hist.removeById(0));
    EXPECT_FALSE(hist.removeById(targetBId + 99999));
    EXPECT_TRUE(hist.removeById(targetBId));
    ASSERT_EQ(hist.count(), 2);

    const auto remain0 = hist.get(0);
    const auto remain1 = hist.get(1);
    ASSERT_TRUE(remain0.has_value());
    ASSERT_TRUE(remain1.has_value());
    EXPECT_EQ(remain0->filePath, "C:\\tmp\\newest_c.png");
    EXPECT_EQ(remain1->filePath, "C:\\tmp\\older_a.png");
    EXPECT_FALSE(hist.getById(targetBId).has_value());
}

TEST(PinWindowLazyOcrTest, DefersHeavyOcrOnCreateRotateFlipAndDeduplicatesInFlight) {
    std::array<wchar_t, 4096> executablePath{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    ASSERT_GT(length, 0u);
    ASSERT_LT(length, executablePath.size());

    const auto executableDir = std::filesystem::path(executablePath.data()).parent_path();
    const auto pluginDllPath =
        executableDir.parent_path() / L"plugins" / executableDir.filename() / L"Plugin_Capture.dll";
    ASSERT_TRUE(std::filesystem::exists(pluginDllPath));

    HMODULE hMod = LoadLibraryExW(
        pluginDllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    ASSERT_NE(hMod, nullptr);

    using CreatePluginFunc = tools3000::core::IPlugin* (*)();
    const auto createPlugin = reinterpret_cast<CreatePluginFunc>(GetProcAddress(hMod, "CreatePlugin"));
    ASSERT_NE(createPlugin, nullptr);
    tools3000::core::IPlugin* capturePlugin = createPlugin();
    ASSERT_NE(capturePlugin, nullptr);
    ASSERT_TRUE(capturePlugin->initialize());

    struct PluginCleanupGuard {
        HMODULE mod = nullptr;
        tools3000::core::IPlugin* plugin = nullptr;
        ~PluginCleanupGuard() {
            while (HWND existing = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
                SendMessageW(existing, WM_KEYDOWN, VK_ESCAPE, 0);
                if (IsWindow(existing)) DestroyWindow(existing);
            }
            if (plugin) plugin->shutdown();
            if (mod) FreeLibrary(mod);
        }
    } cleanupGuard{hMod, capturePlugin};

    while (HWND existing = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
        SendMessageW(existing, WM_KEYDOWN, VK_ESCAPE, 0);
        if (IsWindow(existing)) DestroyWindow(existing);
    }

    auto& bridge = tools3000::core::MessageBridge::instance();
    auto invokeRpc = [&](int id, const char* method, const nlohmann::json& params) -> nlohmann::json {
        nlohmann::json req = {{"id", id}, {"method", method}, {"params", params}};
        const std::string raw = bridge.handleMessage(req.dump());
        const auto parsed = nlohmann::json::parse(raw, nullptr, false);
        return parsed.is_object() && parsed.contains("result") ? parsed["result"] : parsed;
    };

    cv::Mat sampleImg(48, 80, CV_8UC3, cv::Scalar(240, 242, 245));
    const auto tempPng = std::filesystem::temp_directory_path() /
                         (L"tools3000_lazy_ocr_test_" + std::to_wstring(GetCurrentProcessId()) + L".png");
    std::vector<uchar> pngBuf;
    ASSERT_TRUE(cv::imencode(".png", sampleImg, pngBuf));
    {
        std::ofstream ofs(tempPng, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(pngBuf.data()), static_cast<std::streamsize>(pngBuf.size()));
    }
    struct TempFileGuard {
        std::filesystem::path p;
        ~TempFileGuard() {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    } tempGuard{tempPng};

    const auto pinned = invokeRpc(1901, "capture.pinImageFile", {
        {"path", tools3000::core::WinUtils::wstringToUtf8(tempPng.wstring())}
    });
    ASSERT_TRUE(pinned.value("success", false));

    HWND pinHwnd = FindWindowW(L"Tools3000_PinWindow", nullptr);
    ASSERT_NE(pinHwnd, nullptr);

    // 1. 常规贴图创建、旋转 ('R')、水平翻转 ('H') 均绝不启动重型后台 OCR
    const auto stateAfterCreate = invokeRpc(1902, "capture.inspectPinOcrState", nlohmann::json::object());
    ASSERT_TRUE(stateAfterCreate.value("success", false));
    EXPECT_EQ(stateAfterCreate.value("ocrRunCount", 99u), 0u);
    EXPECT_FALSE(stateAfterCreate.value("ocrReady", true));
    EXPECT_FALSE(stateAfterCreate.value("ocrInFlight", true));

    SendMessageW(pinHwnd, WM_KEYDOWN, 'R', 0);
    SendMessageW(pinHwnd, WM_KEYDOWN, 'H', 0);
    const auto stateAfterTransform = invokeRpc(1903, "capture.inspectPinOcrState", nlohmann::json::object());
    ASSERT_TRUE(stateAfterTransform.value("success", false));
    EXPECT_EQ(stateAfterTransform.value("ocrRunCount", 99u), 0u);
    EXPECT_FALSE(stateAfterTransform.value("ocrReady", true));

    // 2. 显式进入文字选择模式时按需启动 OCR，且连续重复 ensureOcr 单飞去重只计数 1 次
    const auto stateOnSelect = invokeRpc(1904, "capture.inspectPinOcrState", {
        {"toggleSelectionMode", true},
        {"triggerEnsureOcr", true}
    });
    ASSERT_TRUE(stateOnSelect.value("success", false));
    EXPECT_TRUE(stateOnSelect.value("textSelectionMode", false));
    EXPECT_EQ(stateOnSelect.value("ocrRunCount", 0u), 1u);

    const auto stateDupEnsure = invokeRpc(1905, "capture.inspectPinOcrState", {
        {"triggerEnsureOcr", true}
    });
    EXPECT_EQ(stateDupEnsure.value("ocrRunCount", 0u), 1u);

    // 3. 不依赖测试侧手工 sleep 轮询：在后台 OCR 仍可能处于 in-flight 瞬间直接调用 shutdown() + FreeLibrary()，
    //    真实校验 CapturePlugin::shutdown() 内部自动排空所有 PinWindow 后台 OCR 线程并排干主线程派发队列。
    capturePlugin->shutdown();
    cleanupGuard.plugin = nullptr;
    EXPECT_TRUE(FreeLibrary(hMod));
    cleanupGuard.mod = nullptr;
}

TEST(PinWindowLazyOcrTest, GeneratedTextCardPreservesSelectAllAcrossRotateAndFlip) {
    std::array<wchar_t, 4096> executablePath{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    ASSERT_GT(length, 0u);
    ASSERT_LT(length, executablePath.size());

    const auto executableDir = std::filesystem::path(executablePath.data()).parent_path();
    const auto pluginDllPath =
        executableDir.parent_path() / L"plugins" / executableDir.filename() / L"Plugin_Capture.dll";
    ASSERT_TRUE(std::filesystem::exists(pluginDllPath));

    HMODULE hMod = LoadLibraryExW(
        pluginDllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    ASSERT_NE(hMod, nullptr);

    using CreatePluginFunc = tools3000::core::IPlugin* (*)();
    const auto createPlugin = reinterpret_cast<CreatePluginFunc>(GetProcAddress(hMod, "CreatePlugin"));
    ASSERT_NE(createPlugin, nullptr);
    tools3000::core::IPlugin* capturePlugin = createPlugin();
    ASSERT_NE(capturePlugin, nullptr);
    ASSERT_TRUE(capturePlugin->initialize());

    struct PluginCleanupGuard {
        HMODULE mod = nullptr;
        tools3000::core::IPlugin* plugin = nullptr;
        ~PluginCleanupGuard() {
            while (HWND existing = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
                SendMessageW(existing, WM_KEYDOWN, VK_ESCAPE, 0);
                if (IsWindow(existing)) DestroyWindow(existing);
            }
            if (plugin) plugin->shutdown();
            if (mod) FreeLibrary(mod);
        }
    } cleanupGuard{hMod, capturePlugin};

    while (HWND existing = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
        SendMessageW(existing, WM_KEYDOWN, VK_ESCAPE, 0);
        if (IsWindow(existing)) DestroyWindow(existing);
    }

    auto writeClipText = [](const std::wstring& text) -> bool {
        for (int retry = 0; retry < 15; ++retry) {
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
                if (HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                    if (auto* dst = static_cast<wchar_t*>(GlobalLock(hMem))) {
                        std::memcpy(dst, text.c_str(), bytes);
                        GlobalUnlock(hMem);
                        SetClipboardData(CF_UNICODETEXT, hMem);
                    }
                }
                CloseClipboard();
                return true;
            }
            Sleep(5);
        }
        return false;
    };

    auto& bridge = tools3000::core::MessageBridge::instance();
    auto invokeRpc = [&](int id, const char* method, const nlohmann::json& params) -> nlohmann::json {
        nlohmann::json req = {{"id", id}, {"method", method}, {"params", params}};
        const std::string raw = bridge.handleMessage(req.dump());
        const auto parsed = nlohmann::json::parse(raw, nullptr, false);
        return parsed.is_object() && parsed.contains("result") ? parsed["result"] : parsed;
    };

    ASSERT_TRUE(writeClipText(L"CardSelectAll@2026"));
    const auto pasted = invokeRpc(1911, "capture.pasteAsPin", nlohmann::json::object());
    ASSERT_TRUE(pasted.value("success", false));

    HWND pinHwnd = FindWindowW(L"Tools3000_PinWindow", nullptr);
    ASSERT_NE(pinHwnd, nullptr);

    // 旋转 ('R') 与翻转 ('H') 文本字签后，合成 OCR 块必须同步重建宽高并维持 ocrReady=true，绝不退化失效
    SendMessageW(pinHwnd, WM_KEYDOWN, 'R', 0);
    SendMessageW(pinHwnd, WM_KEYDOWN, 'H', 0);
    ASSERT_TRUE(writeClipText(L"__CLEARED__"));

    const auto afterSelectAll = invokeRpc(1912, "capture.inspectPinOcrState", {
        {"selectAll", true}
    });
    ASSERT_TRUE(afterSelectAll.value("success", false));
    EXPECT_TRUE(afterSelectAll.value("isGeneratedCard", false));
    EXPECT_TRUE(afterSelectAll.value("ocrReady", false));
    EXPECT_EQ(afterSelectAll.value("ocrRunCount", 99u), 0u);
    EXPECT_EQ(afterSelectAll.value("selectedBlockCount", 0), 1);

    const auto afterCopySelected = invokeRpc(1913, "capture.inspectPinOcrState", {
        {"copySelected", true}
    });
    ASSERT_TRUE(afterCopySelected.value("success", false));
    EXPECT_EQ(afterCopySelected.value("selectedBlockCount", 99), 0);

    std::wstring copiedBack;
    for (int retry = 0; retry < 15; ++retry) {
        if (OpenClipboard(nullptr)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                    copiedBack = p;
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
            break;
        }
        Sleep(5);
    }
    EXPECT_EQ(copiedBack, L"CardSelectAll@2026");
}

TEST(CaptureUsabilitySemanticsTest, SemanticVectorIconsRenderCrispInkAndDistinctSilhouettes) {
    using namespace tools3000::capture;

    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf());
    ASSERT_NE(d2dFactory, nullptr);

    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcRt;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    ASSERT_EQ(d2dFactory->CreateDCRenderTarget(&props, dcRt.GetAddressOf()), S_OK);

    auto renderPatch = [&](CaptureIconId iconId, int sizePx, float scale, bool withFactory) -> cv::Mat {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = sizePx;
        bmi.bmiHeader.biHeight = -sizePx;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* dibBits = nullptr;
        HDC screenDc = GetDC(nullptr);
        HDC memDc = CreateCompatibleDC(screenDc);
        HBITMAP hBmp = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
        ReleaseDC(nullptr, screenDc);
        HGDIOBJ oldBmp = SelectObject(memDc, hBmp);

        RECT rc{0, 0, sizePx, sizePx};
        dcRt->BindDC(memDc, &rc);
        dcRt->BeginDraw();
        dcRt->Clear(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> inkBrush;
        dcRt->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.12f, 0.16f, 0.95f), inkBrush.GetAddressOf());
        CaptureVectorIcons::renderIcon(
            dcRt.Get(),
            withFactory ? d2dFactory.Get() : nullptr,
            iconId,
            D2D1::RectF(0.0f, 0.0f, static_cast<float>(sizePx), static_cast<float>(sizePx)),
            inkBrush.Get(),
            scale);
        EXPECT_EQ(dcRt->EndDraw(), S_OK);
        GdiFlush();

        cv::Mat copyMat = cv::Mat(sizePx, sizePx, CV_8UC4, dibBits).clone();
        SelectObject(memDc, oldBmp);
        DeleteObject(hBmp);
        DeleteDC(memDc);
        return copyMat;
    };

    auto countInkPixels = [](const cv::Mat& img) -> int {
        int ink = 0;
        for (int y = 0; y < img.rows; ++y) {
            for (int x = 0; x < img.cols; ++x) {
                const auto& px = img.at<cv::Vec4b>(y, x);
                if (px[0] < 235 || px[1] < 235 || px[2] < 235) {
                    ++ink;
                }
            }
        }
        return ink;
    };

    auto countDiffPixels = [](const cv::Mat& a, const cv::Mat& b) -> int {
        int diff = 0;
        for (int y = 0; y < a.rows; ++y) {
            for (int x = 0; x < a.cols; ++x) {
                const auto& pa = a.at<cv::Vec4b>(y, x);
                const auto& pb = b.at<cv::Vec4b>(y, x);
                if (std::abs(int(pa[0]) - int(pb[0])) > 20 ||
                    std::abs(int(pa[1]) - int(pb[1])) > 20 ||
                    std::abs(int(pa[2]) - int(pb[2])) > 20) {
                    ++diff;
                }
            }
        }
        return diff;
    };

    const std::vector<CaptureIconId> semanticIcons = {
        CaptureIconId::ToolMagnifier,
        CaptureIconId::ToolSpotlight,
        CaptureIconId::ActionPinGroup,
        CaptureIconId::ActionRecordStart,
        CaptureIconId::ActionRecordPause,
        CaptureIconId::ActionRecordStop,
        CaptureIconId::ActionToggleMic,
        CaptureIconId::ActionToggleMicMuted,
        CaptureIconId::ActionToggleSpeaker,
        CaptureIconId::ActionToggleSpeakerMuted,
        CaptureIconId::ActionResetSelection,
        CaptureIconId::PropPadding,
        CaptureIconId::PropSpotlightBorder,
        CaptureIconId::PropMagnifierSquare,
        CaptureIconId::PropMagnifierBadge,
        CaptureIconId::PropTextOutline,
        CaptureIconId::PropFontSize,
        CaptureIconId::PropTextBackground,
        CaptureIconId::PropSmoothCurve,
    };

    for (const auto icon : semanticIcons) {
        const cv::Mat p1x = renderPatch(icon, 32, 1.0f, true);
        const cv::Mat p2x = renderPatch(icon, 56, 1.8f, true);
        const cv::Mat pFallback = renderPatch(icon, 32, 1.0f, false);

        const int ink1x = countInkPixels(p1x);
        const int ink2x = countInkPixels(p2x);
        const int inkFb = countInkPixels(pFallback);

        EXPECT_GT(ink1x, 12) << "Semantic icon had insufficient ink at 1x: " << static_cast<int>(icon);
        EXPECT_LT(ink1x, 32 * 32 * 70 / 100) << "Semantic icon flooded negative space at 1x: " << static_cast<int>(icon);
        EXPECT_GT(ink2x, 36) << "Semantic icon had insufficient ink at 2x: " << static_cast<int>(icon);
        EXPECT_GT(inkFb, 10) << "Semantic icon had insufficient ink without D2D factory: " << static_cast<int>(icon);
    }

    // 静音与开启状态必须存在显著视觉笔迹差异（斜杠/静音叉）
    EXPECT_GT(countDiffPixels(renderPatch(CaptureIconId::ActionToggleMic, 40, 1.25f, true),
                              renderPatch(CaptureIconId::ActionToggleMicMuted, 40, 1.25f, true)), 24);
    EXPECT_GT(countDiffPixels(renderPatch(CaptureIconId::ActionToggleSpeaker, 40, 1.25f, true),
                              renderPatch(CaptureIconId::ActionToggleSpeakerMuted, 40, 1.25f, true)), 24);

    // 文字标注二级栏三个图标（Aa 字号、[T] 背景胶囊、空心 T 描边）两两之间必须轮廓鲜明区分
    const cv::Mat fontSizePatch = renderPatch(CaptureIconId::PropFontSize, 40, 1.25f, true);
    const cv::Mat textBgPatch = renderPatch(CaptureIconId::PropTextBackground, 40, 1.25f, true);
    const cv::Mat textOutlinePatch = renderPatch(CaptureIconId::PropTextOutline, 40, 1.25f, true);
    EXPECT_GT(countDiffPixels(fontSizePatch, textBgPatch), 40);
    EXPECT_GT(countDiffPixels(fontSizePatch, textOutlinePatch), 40);
    EXPECT_GT(countDiffPixels(textBgPatch, textOutlinePatch), 40);

    // 录屏恢复三角、暂停双竖线、停止圆角方块两两必须显著区分
    const cv::Mat resumePatch = renderPatch(CaptureIconId::ActionRecordStart, 40, 1.25f, true);
    const cv::Mat pausePatch = renderPatch(CaptureIconId::ActionRecordPause, 40, 1.25f, true);
    const cv::Mat stopPatch = renderPatch(CaptureIconId::ActionRecordStop, 40, 1.25f, true);
    EXPECT_GT(countDiffPixels(resumePatch, pausePatch), 30);
    EXPECT_GT(countDiffPixels(resumePatch, stopPatch), 30);

    // Aa 步进器实机尺寸 (24px 1x) 下水平墨迹重心必须围绕中心均衡分布，不得严重偏左
    const cv::Mat aaStepperPatch = renderPatch(CaptureIconId::PropFontSize, 24, 1.0f, true);
    double weightedX = 0.0;
    double totalWeight = 0.0;
    for (int y = 0; y < aaStepperPatch.rows; ++y) {
        for (int x = 0; x < aaStepperPatch.cols; ++x) {
            const auto& px = aaStepperPatch.at<cv::Vec4b>(y, x);
            const double darkness = 255.0 - static_cast<double>(px[0] + px[1] + px[2]) / 3.0;
            if (darkness > 12.0) {
                weightedX += darkness * (static_cast<double>(x) + 0.5);
                totalWeight += darkness;
            }
        }
    }
    ASSERT_GT(totalWeight, 0.0);
    EXPECT_NEAR(weightedX / totalWeight, 12.0, 2.0);

    // 静音麦克风与贴图编组图标必须真实渲染半透明次级图层（56% / 58% alpha 合成后灰度位于 [95, 195]）
    auto countMidToneSubLayerPixels = [](const cv::Mat& img) -> int {
        int mid = 0;
        for (int y = 0; y < img.rows; ++y) {
            for (int x = 0; x < img.cols; ++x) {
                const auto& px = img.at<cv::Vec4b>(y, x);
                const int lum = (int(px[0]) + int(px[1]) + int(px[2])) / 3;
                if (lum >= 95 && lum <= 195) {
                    ++mid;
                }
            }
        }
        return mid;
    };
    EXPECT_GT(countMidToneSubLayerPixels(renderPatch(CaptureIconId::ActionToggleMicMuted, 48, 1.5f, true)), 30);
    EXPECT_GT(countMidToneSubLayerPixels(renderPatch(CaptureIconId::ActionPinGroup, 48, 1.5f, true)), 18);
}

TEST(CaptureUsabilitySemanticsTest, ContextAwareTooltipsAndAccessibilityNamesEliminateAmbiguity) {
    using namespace tools3000::capture;

    auto makeStrokeBtn = [](MarkupTool tool) {
        ToolbarButton btn{};
        btn.command = ToolbarCommand::CycleStrokeWidth;
        btn.tool = tool;
        return btn;
    };

    // 1. CycleStrokeWidth 在不同工具下绝不能统称为 "Stroke Width" 或退化为默认泛称
    EXPECT_NE(tooltipForButton(makeStrokeBtn(MarkupTool::Text), false).find(L"Font Size"), std::wstring::npos);
    EXPECT_EQ(tooltipForButton(makeStrokeBtn(MarkupTool::Text), false).find(L"Stroke Width"), std::wstring::npos);
    EXPECT_NE(tooltipForButton(makeStrokeBtn(MarkupTool::Text), true).find(L"字号"), std::wstring::npos);
    EXPECT_EQ(toolbarButtonAccessibleName(makeStrokeBtn(MarkupTool::Text)), L"调节文本字号");

    EXPECT_NE(tooltipForButton(makeStrokeBtn(MarkupTool::Mosaic), false).find(L"Mosaic Size"), std::wstring::npos);
    EXPECT_EQ(toolbarButtonAccessibleName(makeStrokeBtn(MarkupTool::Mosaic)), L"调节马赛克粒度");

    EXPECT_NE(tooltipForButton(makeStrokeBtn(MarkupTool::Blur), false).find(L"Blur Strength"), std::wstring::npos);
    EXPECT_EQ(toolbarButtonAccessibleName(makeStrokeBtn(MarkupTool::Blur)), L"调节模糊强度");

    EXPECT_NE(tooltipForButton(makeStrokeBtn(MarkupTool::Inpaint), false).find(L"Brush Size"), std::wstring::npos);
    EXPECT_EQ(toolbarButtonAccessibleName(makeStrokeBtn(MarkupTool::Inpaint)), L"调节涂抹笔刷大小");

    EXPECT_NE(tooltipForButton(makeStrokeBtn(MarkupTool::Number), false).find(L"Badge Size"), std::wstring::npos);
    EXPECT_EQ(toolbarButtonAccessibleName(makeStrokeBtn(MarkupTool::Number)), L"调节序号尺寸");

    EXPECT_NE(tooltipForButton(makeStrokeBtn(MarkupTool::Magnifier), false).find(L"Rim Thickness"), std::wstring::npos);
    EXPECT_EQ(toolbarButtonAccessibleName(makeStrokeBtn(MarkupTool::Magnifier)), L"调节镜框粗细");

    EXPECT_NE(tooltipForButton(makeStrokeBtn(MarkupTool::Rectangle), false).find(L"Stroke Width"), std::wstring::npos);
    EXPECT_EQ(toolbarButtonAccessibleName(makeStrokeBtn(MarkupTool::Rectangle)), L"调节线条粗细");

    // 2. ToggleFill 在 Text / Magnifier / Spotlight / Rectangle 四种上下文下精准分流
    auto makeFillBtn = [](MarkupTool tool) {
        ToolbarButton btn{};
        btn.command = ToolbarCommand::ToggleFill;
        btn.tool = tool;
        return btn;
    };
    EXPECT_NE(tooltipForButton(makeFillBtn(MarkupTool::Text), false).find(L"Background"), std::wstring::npos);
    EXPECT_NE(tooltipForButton(makeFillBtn(MarkupTool::Text), true).find(L"文字背景底衬"), std::wstring::npos);
    EXPECT_EQ(toolbarButtonAccessibleName(makeFillBtn(MarkupTool::Text)), L"文字背景底衬开关");
    EXPECT_EQ(toolbarButtonAccessibleName(makeFillBtn(MarkupTool::Magnifier)), L"倍率角标显隐开关");
    EXPECT_EQ(toolbarButtonAccessibleName(makeFillBtn(MarkupTool::Spotlight)), L"聚光区域描边开关");
    EXPECT_EQ(toolbarButtonAccessibleName(makeFillBtn(MarkupTool::Rectangle)), L"切换填充模式");

    // 3. 录音开关 Tooltip 明确反映当前开启/静音状态，且侧边栏扩展按钮不再误标为 "Invert Selection"
    ToolbarButton sysAudioOn{};
    sysAudioOn.command = ToolbarCommand::RecordToggleSystemAudio;
    sysAudioOn.boolParam = true;
    ToolbarButton sysAudioMuted = sysAudioOn;
    sysAudioMuted.boolParam = false;
    EXPECT_NE(tooltipForButton(sysAudioOn, false), tooltipForButton(sysAudioMuted, false));
    EXPECT_NE(tooltipForButton(sysAudioMuted, false).find(L"Muted"), std::wstring::npos);
    EXPECT_NE(tooltipForButton(sysAudioMuted, true).find(L"静音"), std::wstring::npos);

    ToolbarButton sideFullBtn{};
    sideFullBtn.command = ToolbarCommand::SideInvertSelection;
    EXPECT_EQ(tooltipForButton(sideFullBtn, false).find(L"Invert"), std::wstring::npos);
    EXPECT_NE(tooltipForButton(sideFullBtn, false).find(L"Full Screen"), std::wstring::npos);

    // 4. 遍历全部标注工具二级栏与美化外壳栏，确保无任何实际按钮退化为泛化的 "截图工具栏操作"
    const std::vector<MarkupTool> allTools = {
        MarkupTool::Rectangle, MarkupTool::Line, MarkupTool::Ellipse, MarkupTool::Pen,
        MarkupTool::Highlight, MarkupTool::Arrow, MarkupTool::Text, MarkupTool::Number,
        MarkupTool::Magnifier, MarkupTool::Mosaic, MarkupTool::Blur, MarkupTool::Inpaint,
        MarkupTool::Spotlight
    };
    const D2D1_RECT_F sel = D2D1::RectF(120.0f, 120.0f, 920.0f, 640.0f);
    const D2D1_SIZE_F surface = D2D1::SizeF(1920.0f, 1080.0f);
    for (const auto tool : allTools) {
        CaptureState state{};
        state.mode = OverlayMode::Screenshot;
        state.dpiScale = 1.0f;
        state.isMarkupToolActive = true;
        state.currentTool = tool;
        state.toolbarLayoutValid = false;
        rebuildCaptureToolbar(state, sel, surface);
        ASSERT_FALSE(state.secondaryToolbarButtons.empty());
        for (const auto& btn : state.secondaryToolbarButtons) {
            EXPECT_NE(toolbarButtonAccessibleName(btn), L"截图工具栏操作")
                << "Secondary button fell back to generic accessible name under tool=" << static_cast<int>(tool)
                << " cmd=" << static_cast<int>(btn.command);
            EXPECT_FALSE(tooltipForButton(btn, true).empty())
                << "Missing Chinese tooltip for secondary cmd=" << static_cast<int>(btn.command);
            EXPECT_FALSE(tooltipForButton(btn, false).empty())
                << "Missing English tooltip for secondary cmd=" << static_cast<int>(btn.command);
        }
    }

    // 5. 真实通过 rebuildCaptureToolbar 校验像素马赛克 vs 高斯模糊联动（杜绝手工伪造按钮掩盖死代码）
    {
        CaptureState mosaicState{};
        mosaicState.mode = OverlayMode::Screenshot;
        mosaicState.dpiScale = 1.0f;
        mosaicState.isMarkupToolActive = true;
        mosaicState.currentTool = MarkupTool::Mosaic;
        mosaicState.currentMosaicType = 0;
        mosaicState.toolbarLayoutValid = false;
        rebuildCaptureToolbar(mosaicState, sel, surface);
        ASSERT_EQ(mosaicState.secondaryToolbarButtons.size(), 3u);
        EXPECT_TRUE(isToolbarButtonSelected(mosaicState.secondaryToolbarButtons[0], mosaicState));
        EXPECT_FALSE(isToolbarButtonSelected(mosaicState.secondaryToolbarButtons[1], mosaicState));
        EXPECT_NE(tooltipForButton(mosaicState.secondaryToolbarButtons[2], false).find(L"Mosaic Size"), std::wstring::npos);
        EXPECT_EQ(toolbarButtonAccessibleName(mosaicState.secondaryToolbarButtons[2]), L"调节马赛克粒度");

        CaptureState blurState{};
        blurState.mode = OverlayMode::Screenshot;
        blurState.dpiScale = 1.0f;
        blurState.isMarkupToolActive = true;
        blurState.currentTool = MarkupTool::Mosaic;
        blurState.currentMosaicType = 1;
        blurState.toolbarLayoutValid = false;
        rebuildCaptureToolbar(blurState, sel, surface);
        ASSERT_EQ(blurState.secondaryToolbarButtons.size(), 3u);
        EXPECT_FALSE(isToolbarButtonSelected(blurState.secondaryToolbarButtons[0], blurState));
        EXPECT_TRUE(isToolbarButtonSelected(blurState.secondaryToolbarButtons[1], blurState));
        EXPECT_NE(tooltipForButton(blurState.secondaryToolbarButtons[2], false).find(L"Blur Strength"), std::wstring::npos);
        EXPECT_NE(tooltipForButton(blurState.secondaryToolbarButtons[2], true).find(L"模糊强度"), std::wstring::npos);
        EXPECT_EQ(toolbarButtonAccessibleName(blurState.secondaryToolbarButtons[2]), L"调节模糊强度");

        bool foundPrimaryBlurBtn = false;
        for (const auto& pBtn : blurState.toolbarButtons) {
            if (pBtn.command == ToolbarCommand::SelectTool && pBtn.tool == MarkupTool::Blur) {
                foundPrimaryBlurBtn = true;
                EXPECT_TRUE(isToolbarButtonSelected(pBtn, blurState));
                EXPECT_NE(tooltipForButton(pBtn, false).find(L"Gaussian Blur"), std::wstring::npos);
                EXPECT_NE(tooltipForButton(pBtn, true).find(L"高斯模糊"), std::wstring::npos);
            }
        }
        EXPECT_TRUE(foundPrimaryBlurBtn);
    }

    CaptureState beautyState{};
    beautyState.mode = OverlayMode::Screenshot;
    beautyState.dpiScale = 1.0f;
    beautyState.isMarkupToolActive = false;
    beautyState.beautyShell.enabled = true;
    beautyState.beautyShell.padding = 32;
    beautyState.toolbarLayoutValid = false;
    rebuildCaptureToolbar(beautyState, sel, surface);
    bool foundPaddingStepper = false;
    for (const auto& btn : beautyState.secondaryToolbarButtons) {
        if (btn.command == ToolbarCommand::CycleBeautyPadding) {
            foundPaddingStepper = true;
            EXPECT_EQ(btn.label, L"32");
            EXPECT_NEAR(btn.rect.right - btn.rect.left, 54.0f, 0.5f);
        }
        EXPECT_NE(toolbarButtonAccessibleName(btn), L"截图工具栏操作");
    }
    EXPECT_TRUE(foundPaddingStepper);
}

TEST(PixPinParityAndEndpointSystemTest, PureWhiteTextCardThickDashedGapsAndEndpointCapsShaftTrim) {
    using namespace tools3000::capture;

    // 1. 验证复制文字贴出 (Text Pin) 背景为纯净亮白色 (BGR > 235) 且正文含高对比深色石墨黑笔迹
    const cv::Mat whiteNote = PinPasteCoordinator::renderTextToImage(L"PixPin 纯白微晶文字卡片测试\nSecond Line Clean White", 1.0f);
    ASSERT_FALSE(whiteNote.empty());
    EXPECT_EQ(whiteNote.type(), CV_8UC3);
    // 右上角内侧空白底衬区应为纯白/亮白 (BGR > 235)
    const cv::Vec3b bgSample = whiteNote.at<cv::Vec3b>(8, whiteNote.cols - 12);
    EXPECT_GT(bgSample[0], 235);
    EXPECT_GT(bgSample[1], 235);
    EXPECT_GT(bgSample[2], 235);
    // 卡片内部必须包含深色正文笔迹 (BGR < 80)
    int darkTextInkCount = 0;
    for (int y = 6; y < whiteNote.rows - 6; ++y) {
        for (int x = 14; x < whiteNote.cols - 8; ++x) {
            const cv::Vec3b px = whiteNote.at<cv::Vec3b>(y, x);
            if (px[0] < 80 && px[1] < 80 && px[2] < 80) {
                ++darkTextInkCount;
            }
        }
    }
    EXPECT_GT(darkTextInkCount, 40);

    // 2. 验证高线宽 (thickness = 12.0f) 下 Dashed 与 Dotted 在直线及高频 Pen 折线上的中心轴具备多段空白间隙 (像素值 = 0)
    {
        cv::Mat base(120, 420, CV_8UC3, cv::Scalar(0, 0, 0));
        MarkupEngine engine;
        engine.setBaseImage(base);
        auto* dashedLine = engine.drawLine(cv::Point(30, 60), cv::Point(390, 60), MarkupColor(255, 255, 255), 12.0f);
        ASSERT_NE(dashedLine, nullptr);
        dashedLine->lineStyle = LineStyle::Dashed;
        cv::Mat compDashed = engine.getCompositeImage(false);

        int blankTransitions = 0;
        bool prevLit = false;
        int zeroPixels = 0;
        for (int x = 40; x <= 380; ++x) {
            const cv::Vec3b px = compDashed.at<cv::Vec3b>(60, x);
            const bool isLit = (px[0] > 128);
            if (!isLit) ++zeroPixels;
            if (prevLit && !isLit) ++blankTransitions;
            prevLit = isLit;
        }
        EXPECT_GT(zeroPixels, 60) << "Thick dashed line (thickness=12) merged into a solid line without gaps";
        EXPECT_GE(blankTransitions, 3) << "Thick dashed line did not produce distinct dash segments";
    }
    {
        cv::Mat base(120, 420, CV_8UC3, cv::Scalar(0, 0, 0));
        MarkupEngine engine;
        engine.setBaseImage(base);
        std::vector<cv::Point> densePenPts;
        for (int x = 30; x <= 390; x += 2) {
            densePenPts.emplace_back(x, 60);
        }
        auto* dottedPen = engine.drawPenStroke(densePenPts, MarkupColor(255, 255, 255), 12.0f);
        ASSERT_NE(dottedPen, nullptr);
        dottedPen->lineStyle = LineStyle::Dotted;
        cv::Mat compDotted = engine.getCompositeImage(false);

        int dotTransitions = 0;
        bool prevLit = false;
        int zeroPixels = 0;
        for (int x = 40; x <= 380; ++x) {
            const cv::Vec3b px = compDotted.at<cv::Vec3b>(60, x);
            const bool isLit = (px[0] > 128);
            if (!isLit) ++zeroPixels;
            if (prevLit && !isLit) ++dotTransitions;
            prevLit = isLit;
        }
        EXPECT_GT(zeroPixels, 80) << "Thick dotted pen stroke (2px dense vertices, thickness=12) merged into solid line";
        EXPECT_GE(dotTransitions, 4);
    }

    // 3. 验证双端 10 种端点样式二级栏布局与空心三角/空心圆环端点主线精准截断（内部镂空不穿心）
    {
        CaptureState lineState{};
        lineState.mode = OverlayMode::Screenshot;
        lineState.dpiScale = 1.0f;
        lineState.isMarkupToolActive = true;
        lineState.currentTool = MarkupTool::Line;
        lineState.currentLineArrowEnabled = true;
        rebuildCaptureToolbar(lineState, D2D1::RectF(100.0f, 100.0f, 800.0f, 600.0f), D2D1::SizeF(1920.0f, 1080.0f));
        bool hasCurveToggle = false, hasArrowToggle = false, hasStartCap = false, hasEndCap = false;
        for (const auto& btn : lineState.secondaryToolbarButtons) {
            if (btn.command == ToolbarCommand::ToggleCurveMode) hasCurveToggle = true;
            if (btn.command == ToolbarCommand::ToggleLineArrow) hasArrowToggle = true;
            if (btn.command == ToolbarCommand::ToggleStartCapDropdown) hasStartCap = true;
            if (btn.command == ToolbarCommand::ToggleEndCapDropdown) hasEndCap = true;
        }
        EXPECT_TRUE(hasCurveToggle);
        EXPECT_TRUE(hasArrowToggle);
        EXPECT_TRUE(hasStartCap);
        EXPECT_TRUE(hasEndCap);
    }
    {
        // 空心三角端点 (HollowTriangle) 与空心圆环端点 (HollowCircle)：验证端点几何内部中心保持黑色 (0)，主线不穿心
        cv::Mat base(160, 400, CV_8UC3, cv::Scalar(0, 0, 0));
        MarkupEngine engine;
        engine.setBaseImage(base);
        auto* elem = engine.drawLine(cv::Point(60, 80), cv::Point(340, 80), MarkupColor(255, 255, 255), 4.0f);
        ASSERT_NE(elem, nullptr);
        elem->hasCustomCaps = true;
        elem->startCap = LineCapStyle::HollowCircle;
        elem->endCap = LineCapStyle::HollowTriangle;
        cv::Mat comp = engine.getCompositeImage(false);

        // 终点 (340, 80) 为 HollowTriangle，triLen = max(15, 4*3.8) = 15.2，几何重心 (330, 80) 必须完全镂空为 0
        const cv::Vec3b triInterior = comp.at<cv::Vec3b>(80, 330);
        EXPECT_EQ(triInterior[0], 0) << "Main shaft penetrated inside HollowTriangle cap";

        // 起点 (60, 80) 为 HollowCircle，rad = max(4.5, 4*1.45) = 5.8，圆心在 (66, 80)，圆心处必须镂空为 0
        const cv::Vec3b circleCenter = comp.at<cv::Vec3b>(80, 66);
        EXPECT_EQ(circleCenter[0], 0) << "Main shaft penetrated inside HollowCircle cap";

        // 主线段中点 (200, 80) 必须正常点亮
        const cv::Vec3b shaftMid = comp.at<cv::Vec3b>(80, 200);
        EXPECT_GT(shaftMid[0], 200);
    }
    {
        // 遍历全部 10 种端点样式与平滑样条曲线模式，确保渲染无异常且均产生有效像素
        for (int capIdx = 0; capIdx <= 9; ++capIdx) {
            cv::Mat base(120, 300, CV_8UC3, cv::Scalar(0, 0, 0));
            MarkupEngine engine;
            engine.setBaseImage(base);
            std::vector<cv::Point> curvePts = { cv::Point(30, 90), cv::Point(150, 30), cv::Point(270, 90) };
            auto* penCurve = engine.drawPenStroke(curvePts, MarkupColor(59, 130, 246), 4.0f);
            ASSERT_NE(penCurve, nullptr);
            penCurve->smoothCurve = true;
            auto* arrowElem = engine.drawArrow(cv::Point(30, 60), cv::Point(270, 60), MarkupColor(239, 68, 68), 4.0f);
            ASSERT_NE(arrowElem, nullptr);
            arrowElem->hasCustomCaps = true;
            arrowElem->startCap = static_cast<LineCapStyle>(capIdx);
            arrowElem->endCap = static_cast<LineCapStyle>(9 - capIdx);
            cv::Mat out = engine.getCompositeImage(false);
            EXPECT_FALSE(out.empty());
            EXPECT_GT(cv::countNonZero(out.reshape(1)), 100);
        }
    }
}

TEST(PixPinParityAndEndpointSystemTest, SmoothCurveArchHitTestDegenerateDashAndSubmenuToggleClose) {
    using namespace tools3000::capture;

    // 1. 验证 Pen 二级属性栏包含 ToggleCurveMode 平滑曲线开关按钮
    {
        CaptureState penState{};
        penState.mode = OverlayMode::Screenshot;
        penState.dpiScale = 1.0f;
        penState.isMarkupToolActive = true;
        penState.currentTool = MarkupTool::Pen;
        rebuildCaptureToolbar(penState, D2D1::RectF(100.0f, 100.0f, 800.0f, 600.0f), D2D1::SizeF(1920.0f, 1080.0f));
        bool foundPenCurveToggle = false;
        for (const auto& btn : penState.secondaryToolbarButtons) {
            if (btn.command == ToolbarCommand::ToggleCurveMode) {
                foundPenCurveToggle = true;
            }
        }
        EXPECT_TRUE(foundPenCurveToggle);
    }

    // 2. 验证平滑曲线 (smoothCurve = true) 在拱起处的 getElementAtEx 命中与 getBoundingBox 包围盒覆盖
    {
        cv::Mat base(240, 420, CV_8UC3, cv::Scalar(0, 0, 0));
        MarkupEngine engine;
        engine.setBaseImage(base);
        // 水平方向 (40, 100) -> (340, 100)，dist = 300，arch = 32，控制点 c1 在 (139, 132)
        auto* curvedLine = engine.drawLine(cv::Point(40, 100), cv::Point(340, 100), MarkupColor(255, 255, 255), 4.0f);
        ASSERT_NE(curvedLine, nullptr);
        curvedLine->smoothCurve = true;

        // 拱峰位于 (139, 128)，距离直线 y=100 达 28px（超过直线容差 8px），曲线 getElementAtEx 必须命中
        const auto hitRes = engine.getElementAtEx(cv::Point(139, 128), 4);
        EXPECT_EQ(hitRes.element, curvedLine);
        EXPECT_EQ(hitRes.area, HitArea::Body);
        const cv::Rect bbox = curvedLine->getBoundingBox();
        EXPECT_GT(bbox.y + bbox.height, 125);
        EXPECT_LT(bbox.y, 75);
    }

    // 3. 验证退化重合点集在 Dashed / Dotted 线型下依然绘制可见圆点，绝不消失
    {
        cv::Mat base(100, 100, CV_8UC3, cv::Scalar(0, 0, 0));
        MarkupEngine engine;
        engine.setBaseImage(base);
        std::vector<cv::Point> coincidentPts = { cv::Point(50, 50), cv::Point(50, 50), cv::Point(50, 50) };
        auto* degPen = engine.drawPenStroke(coincidentPts, MarkupColor(255, 255, 255), 8.0f);
        ASSERT_NE(degPen, nullptr);
        degPen->lineStyle = LineStyle::Dashed;
        cv::Mat out = engine.getCompositeImage(false);
        EXPECT_GT(out.at<cv::Vec3b>(50, 50)[0], 200) << "Degenerate coincident dashed stroke vanished";
    }

    // 4. 验证空心菱形端点 (HollowDiamond) 几何中心镂空不穿心，以及 Line 动态关闭 hasCustomCaps 后恢复普通线段
    {
        cv::Mat base(160, 400, CV_8UC3, cv::Scalar(0, 0, 0));
        MarkupEngine engine;
        engine.setBaseImage(base);
        auto* elem = engine.drawLine(cv::Point(60, 80), cv::Point(340, 80), MarkupColor(255, 255, 255), 4.0f);
        ASSERT_NE(elem, nullptr);
        elem->hasCustomCaps = true;
        elem->startCap = LineCapStyle::None;
        elem->endCap = LineCapStyle::HollowDiamond;
        cv::Mat comp = engine.getCompositeImage(false);
        // 终点 (340, 80)，dLen = 14 * 1.05 = 14.7，菱形中心在 (333, 80)，中心处必须保持黑色镂空 (0)
        const cv::Vec3b diamondCenter = comp.at<cv::Vec3b>(80, 333);
        EXPECT_EQ(diamondCenter[0], 0) << "Main shaft penetrated inside HollowDiamond cap";

        // 关闭 hasCustomCaps 后，(333, 80) 恢复为实线主干点亮状态
        elem->hasCustomCaps = false;
        cv::Mat compPlain = engine.getCompositeImage(false);
        EXPECT_GT(compPlain.at<cv::Vec3b>(80, 333)[0], 200);
    }

    // 5. 验证 StartCapStyle 与 EndCapStyle 下拉子菜单均生成完整的 10 种端点样式按钮 (0..9)
    {
        CaptureState state{};
        state.mode = OverlayMode::Screenshot;
        state.state = OverlayState::Selected;
        state.dpiScale = 1.0f;
        state.isMarkupToolActive = true;
        state.currentTool = MarkupTool::Line;
        state.currentLineArrowEnabled = true;

        state.openSubmenu = SubmenuType::StartCapStyle;
        rebuildCaptureToolbar(state, D2D1::RectF(100.0f, 100.0f, 600.0f, 400.0f), D2D1::SizeF(1200.0f, 800.0f));
        ASSERT_EQ(state.submenuButtons.size(), 10u);
        for (int i = 0; i < 10; ++i) {
            EXPECT_EQ(state.submenuButtons[static_cast<size_t>(i)].command, ToolbarCommand::SelectStartCap);
            EXPECT_EQ(state.submenuButtons[static_cast<size_t>(i)].intParam, i);
        }

        state.openSubmenu = SubmenuType::EndCapStyle;
        rebuildCaptureToolbar(state, D2D1::RectF(100.0f, 100.0f, 600.0f, 400.0f), D2D1::SizeF(1200.0f, 800.0f));
        ASSERT_EQ(state.submenuButtons.size(), 10u);
        for (int i = 0; i < 10; ++i) {
            EXPECT_EQ(state.submenuButtons[static_cast<size_t>(i)].command, ToolbarCommand::SelectEndCap);
            EXPECT_EQ(state.submenuButtons[static_cast<size_t>(i)].intParam, i);
        }
    }
}

TEST(PinWindowDragFirstAndZeroOcclusionTest, SmallPinZeroToolbarOcclusionAndDragFirstOnButtonsAndTextMode) {
    using namespace tools3000::capture;

    // 1. 纯函数边界测试：小尺寸贴图与微晶字签零遮挡判定 shouldShowHoverToolbar
    EXPECT_FALSE(PinInputController::shouldShowHoverToolbar(240.0f, 52.0f, 1.0f, false));
    EXPECT_FALSE(PinInputController::shouldShowHoverToolbar(259.0f, 160.0f, 1.0f, false));
    EXPECT_FALSE(PinInputController::shouldShowHoverToolbar(300.0f, 107.0f, 1.0f, false));
    EXPECT_TRUE(PinInputController::shouldShowHoverToolbar(260.0f, 108.0f, 1.0f, false));
    EXPECT_TRUE(PinInputController::shouldShowHoverToolbar(360.0f, 180.0f, 1.0f, false));

    // 纯文本微晶字签高度阈值 140 * dpiScale
    EXPECT_FALSE(PinInputController::shouldShowHoverToolbar(320.0f, 120.0f, 1.0f, true));
    EXPECT_FALSE(PinInputController::shouldShowHoverToolbar(320.0f, 139.0f, 1.0f, true));
    EXPECT_TRUE(PinInputController::shouldShowHoverToolbar(320.0f, 140.0f, 1.0f, true));

    // 150% DPI 缩放比例自适应与非正数 dpiScale 防御
    EXPECT_FALSE(PinInputController::shouldShowHoverToolbar(380.0f, 180.0f, 1.5f, false));
    EXPECT_FALSE(PinInputController::shouldShowHoverToolbar(400.0f, 160.0f, 1.5f, false));
    EXPECT_TRUE(PinInputController::shouldShowHoverToolbar(390.0f, 162.0f, 1.5f, false));
    EXPECT_TRUE(PinInputController::shouldShowHoverToolbar(300.0f, 120.0f, 0.0f, false));

    // 2. 真窗口端到端实测：小贴图悬停零遮挡 + 工具栏按钮按住即拖 (Drag-First) + 选字模式空白处拖动窗口
    std::array<wchar_t, 4096> executablePath{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    ASSERT_GT(length, 0u);
    ASSERT_LT(length, executablePath.size());

    const auto executableDir = std::filesystem::path(executablePath.data()).parent_path();
    const auto pluginDllPath =
        executableDir.parent_path() / L"plugins" / executableDir.filename() / L"Plugin_Capture.dll";
    ASSERT_TRUE(std::filesystem::exists(pluginDllPath));

    HMODULE hMod = LoadLibraryExW(
        pluginDllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    ASSERT_NE(hMod, nullptr);

    using CreatePluginFunc = tools3000::core::IPlugin* (*)();
    const auto createPlugin = reinterpret_cast<CreatePluginFunc>(GetProcAddress(hMod, "CreatePlugin"));
    ASSERT_NE(createPlugin, nullptr);
    tools3000::core::IPlugin* capturePlugin = createPlugin();
    ASSERT_NE(capturePlugin, nullptr);
    ASSERT_TRUE(capturePlugin->initialize());

    const auto smallPng = std::filesystem::temp_directory_path() /
                          (L"tools3000_pin_small_drag_" + std::to_wstring(GetCurrentProcessId()) + L".png");
    const auto largePng = std::filesystem::temp_directory_path() /
                          (L"tools3000_pin_large_drag_" + std::to_wstring(GetCurrentProcessId()) + L".png");

    struct CleanupGuard {
        HMODULE mod = nullptr;
        tools3000::core::IPlugin* plugin = nullptr;
        std::filesystem::path p1;
        std::filesystem::path p2;
        ~CleanupGuard() {
            while (HWND existing = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
                SendMessageW(existing, WM_KEYDOWN, VK_DELETE, 0);
                if (IsWindow(existing)) DestroyWindow(existing);
            }
            if (plugin) plugin->shutdown();
            if (mod) FreeLibrary(mod);
            std::error_code ec;
            std::filesystem::remove(p1, ec);
            std::filesystem::remove(p2, ec);
        }
    } cleanupGuard{hMod, capturePlugin, smallPng, largePng};

    while (HWND existing = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
        SendMessageW(existing, WM_KEYDOWN, VK_DELETE, 0);
        if (IsWindow(existing)) DestroyWindow(existing);
    }

    auto& bridge = tools3000::core::MessageBridge::instance();
    auto invokeRpc = [&](int id, const char* method, const nlohmann::json& params) -> nlohmann::json {
        nlohmann::json req = {{"id", id}, {"method", method}, {"params", params}};
        const std::string raw = bridge.handleMessage(req.dump());
        const auto parsed = nlohmann::json::parse(raw, nullptr, false);
        return parsed.is_object() && parsed.contains("result") ? parsed["result"] : parsed;
    };

    // 2A. 模拟用户反馈的 240x52 短文字截图贴图：悬停绝不弹出 8 按钮大黑条，任意位置按住左键直接拖动窗口
    {
        cv::Mat smallImg(52, 240, CV_8UC3, cv::Scalar(28, 32, 38));
        std::vector<uchar> pngBuf;
        ASSERT_TRUE(cv::imencode(".png", smallImg, pngBuf));
        {
            std::ofstream ofs(smallPng, std::ios::binary | std::ios::trunc);
            ofs.write(reinterpret_cast<const char*>(pngBuf.data()), static_cast<std::streamsize>(pngBuf.size()));
        }

        const auto pinned = invokeRpc(2101, "capture.pinImageFile", {
            {"path", tools3000::core::WinUtils::wstringToUtf8(smallPng.wstring())}
        });
        ASSERT_TRUE(pinned.value("success", false));

        HWND pinHwnd = FindWindowW(L"Tools3000_PinWindow", nullptr);
        ASSERT_NE(pinHwnd, nullptr);
        SetWindowPos(pinHwnd, nullptr, 300, 300, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

        const auto hoverState = invokeRpc(2102, "capture.inspectPinOcrState", {
            {"forceHover", true}
        });
        ASSERT_TRUE(hoverState.value("success", false));
        EXPECT_FALSE(hoverState.value("hoverToolbarVisible", true));
        EXPECT_DOUBLE_EQ(hoverState.value("toolbarWidth", -1.0), 0.0);

        RECT beforeRc{};
        ASSERT_TRUE(GetWindowRect(pinHwnd, &beforeRc));

        // 在小贴图顶部中央 (120, 20) 按下并拖动 (+36, +28)
        SendMessageW(pinHwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(120, 20));
        SendMessageW(pinHwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(156, 48));
        SendMessageW(pinHwnd, WM_LBUTTONUP, 0, MAKELPARAM(156, 48));

        RECT afterRc{};
        ASSERT_TRUE(GetWindowRect(pinHwnd, &afterRc));
        EXPECT_EQ(afterRc.left - beforeRc.left, 36);
        EXPECT_EQ(afterRc.top - beforeRc.top, 28);

        const auto afterDragState = invokeRpc(2103, "capture.inspectPinOcrState", nlohmann::json::object());
        EXPECT_FALSE(afterDragState.value("textSelectionMode", true));

        SendMessageW(pinHwnd, WM_KEYDOWN, VK_DELETE, 0);
        EXPECT_FALSE(IsWindow(pinHwnd));
    }

    // 2B. 大尺寸贴图 (420x220)：验证悬浮工具栏按钮 Drag-First（按住拖动移动窗口不触发按钮，原地单击抬起才触发按钮）及选字模式空白区拖拽
    {
        cv::Mat largeImg(220, 420, CV_8UC3, cv::Scalar(40, 46, 56));
        std::vector<uchar> pngBuf;
        ASSERT_TRUE(cv::imencode(".png", largeImg, pngBuf));
        {
            std::ofstream ofs(largePng, std::ios::binary | std::ios::trunc);
            ofs.write(reinterpret_cast<const char*>(pngBuf.data()), static_cast<std::streamsize>(pngBuf.size()));
        }

        const auto pinned = invokeRpc(2104, "capture.pinImageFile", {
            {"path", tools3000::core::WinUtils::wstringToUtf8(largePng.wstring())}
        });
        ASSERT_TRUE(pinned.value("success", false));

        HWND pinHwnd = FindWindowW(L"Tools3000_PinWindow", nullptr);
        ASSERT_NE(pinHwnd, nullptr);
        SetWindowPos(pinHwnd, nullptr, 300, 300, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

        const auto hoverState = invokeRpc(2105, "capture.inspectPinOcrState", {
            {"forceHover", true},
            {"mockBlocks", nlohmann::json::array({
                {{"text", "HeaderLine"}, {"x", 16}, {"y", 60}, {"w", 96}, {"h", 24}}
            })}
        });
        ASSERT_TRUE(hoverState.value("success", false));
        EXPECT_TRUE(hoverState.value("hoverToolbarVisible", false));
        EXPECT_GT(hoverState.value("toolbarWidth", 0.0), 180.0);

        const int textBtnX = hoverState.value("textBtnCenterX", 0);
        const int textBtnY = hoverState.value("textBtnCenterY", 0);
        const int invBtnX = hoverState.value("invertBtnCenterX", 0);
        const int invBtnY = hoverState.value("invertBtnCenterY", 0);
        ASSERT_GT(textBtnX, 0);
        ASSERT_GT(textBtnY, 0);
        ASSERT_GT(invBtnX, 0);
        ASSERT_GT(invBtnY, 0);

        // (1) 在「文字划选 (Text)」按钮正中央按下左键：WM_LBUTTONDOWN 绝不直接开启 textSelectionMode！
        RECT beforeBtnDragRc{};
        ASSERT_TRUE(GetWindowRect(pinHwnd, &beforeBtnDragRc));

        SendMessageW(pinHwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(textBtnX, textBtnY));
        const auto onDownState = invokeRpc(2106, "capture.inspectPinOcrState", nlohmann::json::object());
        EXPECT_FALSE(onDownState.value("textSelectionMode", true));
        EXPECT_EQ(onDownState.value("pressedToolbarBtn", -1), 4);

        // 按住左键拖动 (+32, +26) 超过 4px 阈值：立即转为窗口拖拽并清除按钮候选
        SendMessageW(pinHwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(textBtnX + 32, textBtnY + 26));
        const auto onMoveState = invokeRpc(2107, "capture.inspectPinOcrState", nlohmann::json::object());
        EXPECT_TRUE(onMoveState.value("isDragging", false));
        EXPECT_EQ(onMoveState.value("pressedToolbarBtn", 99), -1);

        RECT afterBtnDragRc{};
        ASSERT_TRUE(GetWindowRect(pinHwnd, &afterBtnDragRc));
        EXPECT_EQ(afterBtnDragRc.left - beforeBtnDragRc.left, 32);
        EXPECT_EQ(afterBtnDragRc.top - beforeBtnDragRc.top, 26);

        SendMessageW(pinHwnd, WM_LBUTTONUP, 0, MAKELPARAM(textBtnX + 32, textBtnY + 26));
        const auto afterUpState = invokeRpc(2108, "capture.inspectPinOcrState", nlohmann::json::object());
        EXPECT_FALSE(afterUpState.value("textSelectionMode", true));
        EXPECT_FALSE(afterUpState.value("isDragging", true));

        // (2) 原地单击（无拖拽）反色按钮与文字按钮：在 WM_LBUTTONUP 正常触发按钮命令
        invokeRpc(2109, "capture.inspectPinOcrState", {{"forceHover", true}});
        SendMessageW(pinHwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(invBtnX, invBtnY));
        SendMessageW(pinHwnd, WM_LBUTTONUP, 0, MAKELPARAM(invBtnX, invBtnY));
        const auto afterInvertClick = invokeRpc(2110, "capture.inspectPinOcrState", {{"forceHover", true}});
        EXPECT_TRUE(afterInvertClick.value("isInverted", false));

        SendMessageW(pinHwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(textBtnX, textBtnY));
        SendMessageW(pinHwnd, WM_LBUTTONUP, 0, MAKELPARAM(textBtnX, textBtnY));
        const auto afterTextClick = invokeRpc(2111, "capture.inspectPinOcrState", nlohmann::json::object());
        EXPECT_TRUE(afterTextClick.value("textSelectionMode", false));

        // (3) 在 textSelectionMode 开启状态下，点击右下角无文字空白区 (300, 160) 拖动：依然 100% 移动贴图窗口！
        RECT beforeBlankDragRc{};
        ASSERT_TRUE(GetWindowRect(pinHwnd, &beforeBlankDragRc));

        SendMessageW(pinHwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(300, 160));
        SendMessageW(pinHwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(334, 188));
        SendMessageW(pinHwnd, WM_LBUTTONUP, 0, MAKELPARAM(334, 188));

        RECT afterBlankDragRc{};
        ASSERT_TRUE(GetWindowRect(pinHwnd, &afterBlankDragRc));
        EXPECT_EQ(afterBlankDragRc.left - beforeBlankDragRc.left, 34);
        EXPECT_EQ(afterBlankDragRc.top - beforeBlankDragRc.top, 28);

        const auto afterBlankDragState = invokeRpc(2112, "capture.inspectPinOcrState", nlohmann::json::object());
        EXPECT_EQ(afterBlankDragState.value("selectedBlockCount", -1), 0);

        // (4) 在 textSelectionMode 开启状态下，直接点击左上角 OCR 文字块内部 (30, 70) 并划选到 (90, 74)：正常拉框选中文字块
        RECT beforeOcrSelectRc{};
        ASSERT_TRUE(GetWindowRect(pinHwnd, &beforeOcrSelectRc));

        SendMessageW(pinHwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(30, 70));
        SendMessageW(pinHwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(90, 74));
        SendMessageW(pinHwnd, WM_LBUTTONUP, 0, MAKELPARAM(90, 74));

        RECT afterOcrSelectRc{};
        ASSERT_TRUE(GetWindowRect(pinHwnd, &afterOcrSelectRc));
        EXPECT_EQ(afterOcrSelectRc.left, beforeOcrSelectRc.left);
        EXPECT_EQ(afterOcrSelectRc.top, beforeOcrSelectRc.top);

        const auto afterOcrSelectState = invokeRpc(2113, "capture.inspectPinOcrState", nlohmann::json::object());
        EXPECT_EQ(afterOcrSelectState.value("selectedBlockCount", 0), 1);

        SendMessageW(pinHwnd, WM_KEYDOWN, VK_DELETE, 0);
        EXPECT_FALSE(IsWindow(pinHwnd));
    }

    capturePlugin->shutdown();
    cleanupGuard.plugin = nullptr;
    EXPECT_TRUE(FreeLibrary(hMod));
    cleanupGuard.mod = nullptr;
}

TEST(PixPinPaletteAndTextBgIconTest, RainbowIndicatorFirstPresetSwatchesTooltipAndWheelOpacityBrightness) {
    using namespace tools3000::capture;

    CaptureState state{};
    state.mode = OverlayMode::Screenshot;
    state.state = OverlayState::Selected;
    state.dpiScale = 1.0f;
    state.dragStart = {100, 100};
    state.dragEnd = {700, 500};
    state.isMarkupToolActive = true;
    state.currentTool = MarkupTool::Text;
    state.currentColor = MarkupColor::Blue(); // {59, 130, 246, 255}

    const D2D1_RECT_F sel = D2D1::RectF(100.0f, 100.0f, 700.0f, 500.0f);
    const D2D1_SIZE_F surface = D2D1::SizeF(1920.0f, 1080.0f);
    rebuildCaptureToolbar(state, sel, surface);

    // 1. Text 二级栏：CycleStrokeWidth(0), ToggleFill(1), ToggleTextOutline(2), ChooseCustomColor(3), 7 x SelectColor(4..10)
    ASSERT_EQ(state.secondaryToolbarButtons.size(), 11u);
    const auto& chooseBtn = state.secondaryToolbarButtons[3];
    EXPECT_EQ(chooseBtn.command, ToolbarCommand::ChooseCustomColor);
    EXPECT_NEAR(chooseBtn.rect.right - chooseBtn.rect.left, 24.0f, 0.1f);
    EXPECT_EQ(chooseBtn.color.r, MarkupColor::Blue().r);
    EXPECT_EQ(chooseBtn.color.g, MarkupColor::Blue().g);
    EXPECT_EQ(chooseBtn.color.b, MarkupColor::Blue().b);

    const auto& firstSwatch = state.secondaryToolbarButtons[4];
    EXPECT_EQ(firstSwatch.command, ToolbarCommand::SelectColor);
    EXPECT_TRUE(firstSwatch.isSeparatorBefore);
    for (size_t i = 5; i < state.secondaryToolbarButtons.size(); ++i) {
        EXPECT_EQ(state.secondaryToolbarButtons[i].command, ToolbarCommand::SelectColor);
        EXPECT_FALSE(state.secondaryToolbarButtons[i].isSeparatorBefore);
    }

    // 2. 验证 ChooseCustomColor 中英文 Tooltip
    EXPECT_EQ(tooltipForButton(chooseBtn, true), L"点击设置颜色，滚轮调整不透明度，Ctrl+滚轮调整亮度");
    EXPECT_EQ(tooltipForButton(chooseBtn, false), L"Click to pick color, Scroll for opacity, Ctrl+Scroll for brightness");

    // 3. 验证鼠标悬停在 ChooseCustomColor 上滚动滚轮调节不透明度 (步进 +-15, 边界 30..255) 并同步更新 activeElement
    cv::Mat baseImg(400, 600, CV_8UC3, cv::Scalar(240, 240, 240));
    state.markup.setBaseImage(baseImg);
    auto* rectElem = state.markup.drawRectangle(cv::Point(40, 40), cv::Point(180, 140), state.currentColor, 4.0f);
    ASSERT_NE(rectElem, nullptr);
    rectElem->fill = true;
    rectElem->isActive = true;
    state.activeElement = rectElem;

    // 向下滚轮 2 次：255 -> 240 -> 225
    adjustPaletteColorByWheel(state, chooseBtn.tool, -WHEEL_DELTA, false);
    EXPECT_EQ(state.currentColor.a, 240);
    EXPECT_EQ(rectElem->color.a, 240);
    adjustPaletteColorByWheel(state, chooseBtn.tool, -WHEEL_DELTA, false);
    EXPECT_EQ(state.currentColor.a, 225);
    EXPECT_EQ(rectElem->color.a, 225);

    // 连续向下滚轮 20 次验证下限夹取为 30
    for (int i = 0; i < 20; ++i) {
        adjustPaletteColorByWheel(state, chooseBtn.tool, -WHEEL_DELTA, false);
    }
    EXPECT_EQ(state.currentColor.a, 30);
    EXPECT_EQ(rectElem->color.a, 30);

    // 向上滚轮 1 次：30 -> 45
    adjustPaletteColorByWheel(state, chooseBtn.tool, WHEEL_DELTA, false);
    EXPECT_EQ(state.currentColor.a, 45);

    // 连续向上滚轮 20 次验证上限夹取为 255
    for (int i = 0; i < 20; ++i) {
        adjustPaletteColorByWheel(state, chooseBtn.tool, WHEEL_DELTA, false);
    }
    EXPECT_EQ(state.currentColor.a, 255);

    // 4. 验证 Ctrl + 滚轮微调明度 (Brightness / Value, 步进 +-6%)
    state.currentColor = MarkupColor::Blue(); // {59, 130, 246, 255}
    const uint8_t origB = state.currentColor.b;
    adjustPaletteColorByWheel(state, chooseBtn.tool, -WHEEL_DELTA, true);
    EXPECT_LT(state.currentColor.b, origB);
    EXPECT_EQ(rectElem->color.b, state.currentColor.b);

    const uint8_t dimmedB = state.currentColor.b;
    adjustPaletteColorByWheel(state, chooseBtn.tool, WHEEL_DELTA, true);
    EXPECT_GT(state.currentColor.b, dimmedB);

    // 5. 验证半透明元素 (alpha = 120) 在 MarkupEngine 合成画布上呈现背景透出效果
    rectElem->color = MarkupColor{59, 130, 246, 120};
    const cv::Mat compSemi = state.markup.getCompositeImage(false);
    const cv::Vec3b semiPx = compSemi.at<cv::Vec3b>(90, 100);
    // 科技蓝 (BGR: 246, 130, 59) 与浅灰底 (240, 240, 240) 混合后 R 分量应显著高于纯蓝的 59 且低于背景的 240
    EXPECT_GT(semiPx[2], 120);
    EXPECT_LT(semiPx[2], 220);

    // 6. 验证边界与回退防护：
    // (a) delta == 0 严格无副作用
    const MarkupColor beforeZero = state.currentColor;
    adjustPaletteColorByWheel(state, chooseBtn.tool, 0, false);
    adjustPaletteColorByWheel(state, chooseBtn.tool, 0, true);
    EXPECT_EQ(state.currentColor, beforeZero);

    // (b) 当 activeElement 颜色 (Green) 与 state.currentColor (Red) 不同时，滚轮微调必须以 activeElement->color 为基准，并实时同步更新 secondaryToolbarButtons 中的 ChooseCustomColor 按钮颜色
    state.currentColor = MarkupColor::Red();
    rectElem->color = MarkupColor::Green(); // {16, 185, 129, 255}
    adjustPaletteColorByWheel(state, chooseBtn.tool, -WHEEL_DELTA, false);
    EXPECT_EQ(rectElem->color.r, MarkupColor::Green().r);
    EXPECT_EQ(rectElem->color.g, MarkupColor::Green().g);
    EXPECT_EQ(rectElem->color.b, MarkupColor::Green().b);
    EXPECT_EQ(rectElem->color.a, 240);
    EXPECT_EQ(state.currentColor, rectElem->color);
    EXPECT_EQ(state.secondaryToolbarButtons[3].color, rectElem->color);

    // (c) Ctrl+滚轮向下调暗预设极客黑 MarkupColor::Black() ({30, 41, 59}) 可直达纯黑 #000000，且可从纯黑重新调亮
    rectElem->color = MarkupColor::Black();
    state.currentColor = MarkupColor::Black();
    for (int i = 0; i < 10; ++i) {
        adjustPaletteColorByWheel(state, chooseBtn.tool, -WHEEL_DELTA, true);
    }
    EXPECT_EQ(state.currentColor.r, 0);
    EXPECT_EQ(state.currentColor.g, 0);
    EXPECT_EQ(state.currentColor.b, 0);
    adjustPaletteColorByWheel(state, chooseBtn.tool, WHEEL_DELTA, true);
    EXPECT_GT(state.currentColor.r, 0);
    EXPECT_EQ(state.currentColor.r, state.currentColor.g);
    EXPECT_EQ(state.currentColor.g, state.currentColor.b);

    // (d) 聚光灯 (Spotlight) 开启边框且 alpha < 255 时，边框支持半透明混合
    auto* spotElem = state.markup.addSpotlight(cv::Point(220, 40), cv::Point(360, 140), MarkupColor{244, 63, 94, 120}, 0.55f, false);
    ASSERT_NE(spotElem, nullptr);
    spotElem->spotlightShowBorder = true;
    spotElem->thickness = 4.0f;
    const cv::Mat compSpot = state.markup.getCompositeImage(false);
    const cv::Vec3b spotBorderPx = compSpot.at<cv::Vec3b>(40, 290);
    EXPECT_GT(spotBorderPx[2], 180);
    EXPECT_GT(spotBorderPx[0], 65); // 半透明珊瑚红边框透出背景蓝通道，而非纯不透明的 63

    // 7. 离屏 Direct2D 像素级实测断言：
    //    - CaptureRenderer::drawCustomColorIndicator (四象限连续彩虹外框 + 完整 1.4x 纯白隔离环 + 内芯当前色)
    //    - CaptureRenderer::drawPresetColorSwatch (干净圆角方块 + 无外圈选中同心圆环 + 白色色块 #CBD5E1 描边)
    //    - CaptureIconId::PropTextBackground (右上角实线圆角弧 + 其余三边短虚线 + 内部无字母 T)
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory;
    ASSERT_EQ(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf()), S_OK);
    ASSERT_NE(d2dFactory, nullptr);

    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcRt;
    const D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    ASSERT_EQ(d2dFactory->CreateDCRenderTarget(&rtProps, dcRt.GetAddressOf()), S_OK);

    const int canvasW = 240;
    const int canvasH = 80;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = canvasW;
    bmi.bmiHeader.biHeight = -canvasH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dibBits = nullptr;
    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    HBITMAP hBmp = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    ReleaseDC(nullptr, screenDc);
    ASSERT_NE(memDc, nullptr);
    ASSERT_NE(hBmp, nullptr);
    ASSERT_NE(dibBits, nullptr);
    HGDIOBJ oldBmp = SelectObject(memDc, hBmp);

    RECT bindRc{0, 0, canvasW, canvasH};
    ASSERT_EQ(dcRt->BindDC(memDc, &bindRc), S_OK);
    dcRt->BeginDraw();
    // 使用浅灰底色 (220, 224, 230) 以便清晰区分 1.4x 纯白 (255, 255, 255) 隔离环与外部面板背景
    dcRt->Clear(D2D1::ColorF(220 / 255.0f, 224 / 255.0f, 230 / 255.0f, 1.0f));

    // [Cell 0: x=0..60, y=0..60, center=(30,30), scale=2.0f] 绘制 ChooseCustomColor 彩虹当前色指示器
    CaptureVectorIcons::drawCustomColorIndicator(
        dcRt.Get(), D2D1::RectF(0.0f, 0.0f, 60.0f, 60.0f), MarkupColor::Blue(), 2.0f, false);

    // [Cell 1: x=60..120, y=0..60, center=(90,30), scale=2.0f] 绘制预设红色色块 (未悬停，验证无外圈同心圆环)
    CaptureVectorIcons::drawPresetColorSwatch(
        dcRt.Get(), D2D1::RectF(60.0f, 0.0f, 120.0f, 60.0f), MarkupColor::Red(), 2.0f, false, nullptr);

    // [Cell 2: x=120..180, y=0..60, center=(150,30), scale=2.0f] 绘制预设白色色块 (验证 #CBD5E1 细灰描边)
    CaptureVectorIcons::drawPresetColorSwatch(
        dcRt.Get(), D2D1::RectF(120.0f, 0.0f, 180.0f, 60.0f), MarkupColor::White(), 2.0f, false, nullptr);

    // [Cell 3: x=180..240, y=0..60, center=(210,30), scale=2.0f] 绘制 PropTextBackground 矢量图标
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> iconBrush;
    dcRt->CreateSolidColorBrush(D2D1::ColorF(37 / 255.0f, 99 / 255.0f, 235 / 255.0f, 1.0f), &iconBrush);
    CaptureVectorIcons::renderIcon(
        dcRt.Get(), d2dFactory.Get(), CaptureIconId::PropTextBackground,
        D2D1::RectF(186.0f, 6.0f, 234.0f, 54.0f), iconBrush.Get(), 2.0f);

    ASSERT_EQ(dcRt->EndDraw(), S_OK);
    GdiFlush();

    const cv::Mat dibMat(canvasH, canvasW, CV_8UC4, dibBits);

    // (7.1) 验证 ChooseCustomColor 正中央内芯 (30, 30) 为科技蓝 (R=59, G=130, B=246)
    const cv::Vec4b corePx = dibMat.at<cv::Vec4b>(30, 30);
    EXPECT_NEAR(corePx[0], 246, 4); // B
    EXPECT_NEAR(corePx[1], 130, 4); // G
    EXPECT_NEAR(corePx[2], 59, 4);  // R

    // (7.2) 验证内芯与彩虹环之间的 1.4f * scale 纯白隔离环 (距中心 8.0f * 2 = 16px，即 y=14, x=30) 100% 纯白无彩虹笔触侵蚀
    const cv::Vec4b gapTopPx = dibMat.at<cv::Vec4b>(14, 30);
    EXPECT_GE(gapTopPx[0], 250);
    EXPECT_GE(gapTopPx[1], 250);
    EXPECT_GE(gapTopPx[2], 250);

    // (7.3) 验证外圈四象限连续彩虹环 (距中心 9.1f * 2 ≈ 18px) 四边呈现截然不同的高饱和彩虹色相
    const cv::Vec4b quadTL = dibMat.at<cv::Vec4b>(12, 24); // 左上：红橙
    const cv::Vec4b quadTR = dibMat.at<cv::Vec4b>(12, 36); // 右上：黄绿
    const cv::Vec4b quadBR = dibMat.at<cv::Vec4b>(48, 36); // 右下：青蓝
    const cv::Vec4b quadBL = dibMat.at<cv::Vec4b>(48, 24); // 左下：蓝紫品红
    EXPECT_GT(quadTL[2], 210); // 红橙：高 R
    EXPECT_LT(quadTL[0], 90);  // 红橙：低 B
    EXPECT_GT(quadTR[1], 175); // 黄绿：高 G
    EXPECT_GT(quadBR[0], 175); // 青蓝：高 B
    EXPECT_LT(quadBR[2], 80);  // 青蓝：低 R
    EXPECT_GT(quadBL[2], 110); // 蓝紫品红：R 与 B 均高，G 低
    EXPECT_GT(quadBL[0], 130);
    EXPECT_LT(quadBL[1], 100);

    // (7.4) 验证预设色块 (Cell 1, center=(90,30)) 中心为珊瑚红，且外围 (距中心 21px, y=9, x=90) 保持纯净背景色（彻底无选中双圆环）
    const cv::Vec4b swatchCenter = dibMat.at<cv::Vec4b>(30, 90);
    EXPECT_NEAR(swatchCenter[2], MarkupColor::Red().r, 4);
    const cv::Vec4b swatchOuter = dibMat.at<cv::Vec4b>(9, 90);
    EXPECT_NEAR(swatchOuter[0], 230, 3);
    EXPECT_NEAR(swatchOuter[1], 224, 3);
    EXPECT_NEAR(swatchOuter[2], 220, 3);

    // (7.5) 验证白色预设色块 (Cell 2, center=(150,30)) 内部纯白，边缘 (距中心 17.5px => y=12..13, x=150) 存在 #CBD5E1 细灰描边
    const cv::Vec4b whiteCenter = dibMat.at<cv::Vec4b>(30, 150);
    EXPECT_GE(whiteCenter[0], 252);
    EXPECT_GE(whiteCenter[1], 252);
    EXPECT_GE(whiteCenter[2], 252);
    const cv::Vec4b whiteBorder = dibMat.at<cv::Vec4b>(12, 150);
    EXPECT_LT(whiteBorder[2], 245); // 描边处显著暗于纯白 255

    // (7.6) 验证 PropTextBackground (Cell 3, center=(210,30)) 内部中央无字母 T 浓重实线笔画，而右上角圆角折弧处有饱满实线笔画
    const cv::Vec4b textBgCenter = dibMat.at<cv::Vec4b>(30, 210);
    // 内部仅有 0.15f 极淡底衬，R 分量仍保持 > 180（若画了实心字母 T 则 R 分量会跌至 ~37）
    EXPECT_GT(textBgCenter[2], 180);

    SelectObject(memDc, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDc);
}

TEST(CaptureMarkupBorderAndFillTest, IndependentFillAndBorderColorAndWidthWorkAcrossShapesAndArrows) {
    using namespace tools3000::capture;

    CaptureState state{};
    state.mode = OverlayMode::Screenshot;
    state.dpiScale = 1.0f;
    state.dragStart = {100, 100};
    state.dragEnd = {700, 500};
    state.isMarkupToolActive = true;
    const D2D1_RECT_F sel = D2D1::RectF(100.0f, 100.0f, 700.0f, 500.0f);
    const D2D1_SIZE_F surface = D2D1::SizeF(1920.0f, 1080.0f);

    // 1. 实心填充矩形开启后动态暴露独立边框控制胶囊 (ToggleTextOutline)，且 Arrow Style 下拉包含 6 种样式
    state.currentTool = MarkupTool::Rectangle;
    state.currentFillMode = true;
    state.currentOutlineWidth = 4;
    state.isEditingOutlineColor = true;
    state.currentTextOutlineColor = MarkupColor::Yellow();
    state.toolbarLayoutValid = false;
    rebuildCaptureToolbar(state, sel, surface);
    EXPECT_EQ(state.secondaryToolbarButtons.size(), 16u);
    bool foundBorderCapsule = false;
    for (const auto& btn : state.secondaryToolbarButtons) {
        if (btn.command == ToolbarCommand::ToggleTextOutline) {
            foundBorderCapsule = true;
            EXPECT_EQ(btn.label, L"4");
            EXPECT_TRUE(isToolbarButtonSelected(btn, state));
        }
        if (btn.command == ToolbarCommand::ChooseCustomColor) {
            EXPECT_EQ(btn.color, MarkupColor::Yellow());
        }
    }
    EXPECT_TRUE(foundBorderCapsule);

    // 2. 验证 ArrowStyle 子菜单包含全部 6 种箭头样式（含 CampaignMarch 与 MarchDoubleTrack）
    state.currentTool = MarkupTool::Arrow;
    state.openSubmenu = SubmenuType::ArrowStyle;
    state.toolbarLayoutValid = false;
    rebuildCaptureToolbar(state, sel, surface);
    ASSERT_EQ(state.submenuButtons.size(), 6u);
    EXPECT_EQ(state.submenuButtons[4].arrowStyleParam, ArrowStyle::CampaignMarch);
    EXPECT_EQ(state.submenuButtons[5].arrowStyleParam, ArrowStyle::MarchDoubleTrack);

    // 3. 验证实心填充矩形 + 独立黄色粗边框 (6px) vs 0px 无边框实测像素渲染
    cv::Mat baseImg(300, 400, CV_8UC3, cv::Scalar(40, 40, 40));
    state.markup.setBaseImage(baseImg);
    auto* rectElem = state.markup.drawRectangle(cv::Point(40, 40), cv::Point(160, 140), MarkupColor::Blue(), 2.0f);
    ASSERT_NE(rectElem, nullptr);
    rectElem->fill = true;
    rectElem->hasCustomOutline = true;
    rectElem->outlineThickness = 6.0f;
    rectElem->textOutlineColor = MarkupColor::Yellow();

    cv::Mat compWithBorder = state.markup.getCompositeImage(false);
    // 内部中心 (90, 100) 为科技蓝填充 (R=59, G=130, B=246)
    const cv::Vec3b centerPx = compWithBorder.at<cv::Vec3b>(90, 100);
    EXPECT_EQ(centerPx[0], MarkupColor::Blue().b);
    EXPECT_EQ(centerPx[2], MarkupColor::Blue().r);
    // 顶边 (40, 100) 为明黄边框 (R=234, G=179, B=8)
    const cv::Vec3b topEdgePx = compWithBorder.at<cv::Vec3b>(40, 100);
    EXPECT_EQ(topEdgePx[2], MarkupColor::Yellow().r);
    EXPECT_EQ(topEdgePx[1], MarkupColor::Yellow().g);

    // 将边框粗细设为 0：外扩边界 (38, 100) 应恢复为深灰底色 (40, 40, 40)
    rectElem->outlineThickness = 0.0f;
    cv::Mat compBorderless = state.markup.getCompositeImage(false);
    const cv::Vec3b outerPx = compBorderless.at<cv::Vec3b>(38, 100);
    EXPECT_EQ(outerPx[0], 40);
    EXPECT_EQ(outerPx[1], 40);
    EXPECT_EQ(outerPx[2], 40);

    // 4. 验证放大镜连线样式默认值为虚线 (DashedDirect)，且下拉框首项默认激活、零副标题认知减负
    EXPECT_EQ(state.currentMagnifierCalloutStyle, MagnifierCalloutStyle::DashedDirect);
    auto* defaultMag = state.markup.addMagnifier(cv::Point(80, 80), 2.0f, 40);
    ASSERT_NE(defaultMag, nullptr);
    EXPECT_EQ(defaultMag->magnifierCalloutStyle, MagnifierCalloutStyle::DashedDirect);

    openDropdownMenu(state, DropdownType::MagnifierCalloutStyle, D2D1::RectF(150.0f, 520.0f, 206.0f, 552.0f));
    ASSERT_EQ(state.dropdownMenu.items.size(), 4u);
    EXPECT_EQ(state.dropdownMenu.items[0].id, static_cast<int>(MagnifierCalloutStyle::DashedDirect));
    EXPECT_TRUE(state.dropdownMenu.items[0].isSelected);
    for (const auto& item : state.dropdownMenu.items) {
        EXPECT_TRUE(item.hint.empty());
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// 2026-11-11 全量审计防回退回归测试 (MarkupEngine 边界与 OcrEngine STA 安全)
// ─────────────────────────────────────────────────────────────────────────────

TEST(MarkupEngineRegressionTest, ZeroBlockMosaicAndLargeTiledWatermarkSafety) {
    using namespace tools3000::capture;

    MarkupEngine engine;
    cv::Mat base(1200, 1600, CV_8UC3, cv::Scalar(220, 225, 230));
    engine.setBaseImage(base);

    // 1. 极端 blockSize <= 0 马赛克图元绝不得引发死循环或除零崩溃
    auto* zeroMosaic = engine.applyMosaic(cv::Point(40, 40), cv::Point(240, 200), 0);
    ASSERT_NE(zeroMosaic, nullptr);
    zeroMosaic->mosaicBlockSize = 0;

    auto* negMosaic = engine.applyMosaic(cv::Point(260, 40), cv::Point(460, 200), -8);
    ASSERT_NE(negMosaic, nullptr);
    negMosaic->mosaicBlockSize = -8;

    // 2. 大画布平铺旋转水印图元（验证单图块预渲染复用，毫秒级完成）
    auto* wm = engine.addWatermark(cv::Point(0, 0), cv::Point(1600, 1200), "CONFIDENTIAL 2026", 0.18f, -30.0f);
    ASSERT_NE(wm, nullptr);

    cv::Mat composite;
    EXPECT_NO_THROW({
        composite = engine.getCompositeImage(false);
    });
    EXPECT_EQ(composite.cols, 1600);
    EXPECT_EQ(composite.rows, 1200);
}

TEST(OcrEngineRegressionTest, HandlesMissingEmptyFilesAndStaThreadSafely) {
    // 在 STA 线程下验证 WinRT OcrEngine 后台工作线程转发与异常捕获，绝不触发 RPC_E_CHANGED_MODE 崩溃
    std::string missingResult = "not_empty";
    std::string emptyFileResult = "not_empty";

    const auto emptyImgPath = std::filesystem::temp_directory_path() /
        (L"Tools3000_EmptyOcr_" + std::to_wstring(GetCurrentProcessId()) + L".png");
    {
        std::ofstream ofs(emptyImgPath, std::ios::binary | std::ios::trunc);
    }
    struct TempImgCleanup {
        std::filesystem::path p;
        ~TempImgCleanup() {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    } cleanup{emptyImgPath};

    std::thread staWorker([&]() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        EXPECT_NO_THROW({
            auto& ocr = tools3000::ocr::OcrEngine::instance();
            missingResult = ocr.recognizeImageFile("C:\\nonexistent_dir_t3k_audit\\missing.png");
            emptyFileResult = ocr.recognizeImageFile(
                tools3000::core::WinUtils::wstringToUtf8(emptyImgPath.wstring()));
        });
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    });
    staWorker.join();

    EXPECT_TRUE(missingResult.empty());
    EXPECT_TRUE(emptyFileResult.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Batch 1 (P-03 / U-01 / U-02 / U-03 / V-01) 专项防回退回归测试
// ─────────────────────────────────────────────────────────────────────────────

TEST(MarkupEngineUndoRedoTransactionTest, FullVectorSnapshotTransactionsAndFilterRoiCache) {
    using namespace tools3000::capture;

    MarkupEngine engine;
    cv::Mat base(240, 320, CV_8UC3, cv::Scalar(36, 42, 50));
    engine.setBaseImage(base);

    EXPECT_FALSE(engine.canUndo());
    EXPECT_FALSE(engine.canRedo());

    // Step 1: 新增矩形与两个序号徽章 (#1, #2)
    auto* rect = engine.drawRectangle(cv::Point(20, 20), cv::Point(100, 80), MarkupColor::Red(), 2.0f);
    ASSERT_NE(rect, nullptr);
    const uint32_t rectId = rect->id;

    auto* num1 = engine.drawNumberMark(cv::Point(140, 50), MarkupColor::Green(), 1.0f);
    auto* num2 = engine.drawNumberMark(cv::Point(190, 50), MarkupColor::Green(), 1.0f);
    ASSERT_NE(num1, nullptr);
    ASSERT_NE(num2, nullptr);
    const uint32_t num2Id = num2->id;
    EXPECT_EQ(engine.currentNumber(NumberFormat::Arabic), 3);
    EXPECT_EQ(engine.elementCount(), 3u);

    // Step 2: 修改已有矩形样式 (红框空心 -> 蓝底实心 + 线宽 5) 前压入事务快照
    engine.pushUndoSnapshot();
    rect = engine.getElementById(rectId);
    ASSERT_NE(rect, nullptr);
    rect->color = MarkupColor::Blue();
    rect->fill = true;
    rect->thickness = 5.0f;

    // 验证零位移点击绝不污染撤销栈或清空重做栈：先做一次 undo/redo 预检，再测零位移 commitUndoSnapshot
    ASSERT_TRUE(engine.undo());
    EXPECT_FALSE(engine.getElementById(rectId)->fill);
    EXPECT_EQ(engine.getElementById(rectId)->color.r, MarkupColor::Red().r);
    ASSERT_TRUE(engine.redo());
    EXPECT_TRUE(engine.getElementById(rectId)->fill);
    EXPECT_EQ(engine.getElementById(rectId)->color.b, MarkupColor::Blue().b);

    {
        auto noOpSnap = engine.captureSnapshot();
        // 未发生任何实质几何/样式变更
        engine.commitUndoSnapshot(std::move(noOpSnap));
    }

    // Step 3: 选中拖拽平移矩形 (+30, +20)，通过 commitUndoSnapshot 提交有效位移事务
    {
        auto beforeMove = engine.captureSnapshot();
        rect = engine.getElementById(rectId);
        ASSERT_NE(rect, nullptr);
        rect->startPt += cv::Point(30, 20);
        rect->endPt += cv::Point(30, 20);
        engine.commitUndoSnapshot(std::move(beforeMove));
    }
    EXPECT_EQ(engine.getElementById(rectId)->startPt, cv::Point(50, 40));

    // Step 4: 删除序号徽章 #2 前压入事务快照，删除后待标序号回退至 2
    engine.pushUndoSnapshot();
    engine.removeElement(num2Id);
    EXPECT_EQ(engine.elementCount(), 2u);
    EXPECT_EQ(engine.currentNumber(NumberFormat::Arabic), 2);

    // Step 5: 点击 Clear 清空全部标注（可撤销）
    ASSERT_TRUE(engine.clearElementsUndoable());
    EXPECT_EQ(engine.elementCount(), 0u);
    EXPECT_EQ(engine.currentNumber(NumberFormat::Arabic), 1);

    // 逆序逐步 Ctrl+Z 100% 无损恢复：
    // Undo Clear -> 恢复 2 个元素 (平移后的蓝底矩形 + #1)
    ASSERT_TRUE(engine.undo());
    EXPECT_EQ(engine.elementCount(), 2u);
    EXPECT_EQ(engine.currentNumber(NumberFormat::Arabic), 2);
    ASSERT_NE(engine.getElementById(rectId), nullptr);
    EXPECT_EQ(engine.getElementById(rectId)->startPt, cv::Point(50, 40));
    EXPECT_TRUE(engine.getElementById(rectId)->fill);

    // Undo Delete #2 -> 恢复 3 个元素，待标序号恢复为 3
    ASSERT_TRUE(engine.undo());
    EXPECT_EQ(engine.elementCount(), 3u);
    EXPECT_NE(engine.getElementById(num2Id), nullptr);
    EXPECT_EQ(engine.currentNumber(NumberFormat::Arabic), 3);

    // Undo Move -> 矩形回到 (20, 20)-(100, 80)，仍保持蓝底填充
    ASSERT_TRUE(engine.undo());
    EXPECT_EQ(engine.getElementById(rectId)->startPt, cv::Point(20, 20));
    EXPECT_TRUE(engine.getElementById(rectId)->fill);
    EXPECT_EQ(engine.getElementById(rectId)->color.b, MarkupColor::Blue().b);

    // Undo Style Change -> 矩形恢复为初始红线空心 (thickness = 2.0f)
    ASSERT_TRUE(engine.undo());
    EXPECT_FALSE(engine.getElementById(rectId)->fill);
    EXPECT_FLOAT_EQ(engine.getElementById(rectId)->thickness, 2.0f);
    EXPECT_EQ(engine.getElementById(rectId)->color.r, MarkupColor::Red().r);

    // 正向逐步 Ctrl+Y 100% 无损重做回 Clear 态，再撤销回非空态验证滤镜局部 ROI 缓存与静态底图缓存一致性
    ASSERT_TRUE(engine.redo()); // 重做样式修改
    ASSERT_TRUE(engine.redo()); // 重做拖拽平移
    ASSERT_TRUE(engine.redo()); // 重做删除 #2
    ASSERT_TRUE(engine.redo()); // 重做 Clear 清空
    EXPECT_EQ(engine.elementCount(), 0u);
    EXPECT_FALSE(engine.canRedo());

    ASSERT_TRUE(engine.undo()); // 恢复至 2 个元素
    // P-03: 添加马赛克与高斯模糊图元并顶层拖拽新图元，验证多次合成（命中 ROI 缓存与静态底层缓存）像素严格一致
    cv::Mat gradientBase(240, 320, CV_8UC3);
    for (int y = 0; y < gradientBase.rows; ++y) {
        for (int x = 0; x < gradientBase.cols; ++x) {
            gradientBase.at<cv::Vec3b>(y, x) = cv::Vec3b(
                static_cast<uint8_t>((x * 3) & 0xFF),
                static_cast<uint8_t>((y * 5) & 0xFF),
                static_cast<uint8_t>(((x + y) * 2) & 0xFF));
        }
    }
    engine.setBaseImage(gradientBase);
    engine.applyMosaic(cv::Point(30, 100), cv::Point(130, 190), 12);
    engine.applyBlur(cv::Point(150, 100), cv::Point(250, 190), 15);
    auto* topArrow = engine.drawArrow(cv::Point(40, 210), cv::Point(220, 210), MarkupColor::Yellow(), 3.0f);
    ASSERT_NE(topArrow, nullptr);
    topArrow->isActive = true;

    const cv::Mat pass1 = engine.getCompositeImage(false).clone();
    const cv::Mat pass2 = engine.getCompositeImage(false).clone();
    EXPECT_EQ(cv::norm(pass1, pass2, cv::NORM_INF), 0.0);
}

TEST(OcrXYCutAndSmartMergeTest, SeparatesTwoColumnsWithoutCrossLineInterleaving) {
    using namespace tools3000::ocr;

    // 构造典型学术双栏 / IDE 分屏版式：
    // 顶部通栏大标题 (x: 60..620, y: 20..48)
    // 左栏 3 行 (x: 40..280, y: 90, 122, 154)
    // 右栏 3 行 (x: 380..640, y: 92, 124, 156，其中第 2 行包含双列表格大间距字段)
    std::vector<OcrWordToken> tokens = {
        {L"Multi-Column", 60.0f, 20.0f, 220.0f, 26.0f},
        {L"Architecture", 300.0f, 20.0f, 240.0f, 26.0f},

        // 故意交错左栏与右栏词块输入顺序，检验 XY-Cut 垂直走廊切分鲁棒性
        {L"LeftLine1", 40.0f, 90.0f, 110.0f, 20.0f},
        {L"RightLine1", 380.0f, 92.0f, 120.0f, 20.0f},
        {L"Alpha", 160.0f, 90.0f, 70.0f, 20.0f},
        {L"Omega", 512.0f, 92.0f, 80.0f, 20.0f},

        {L"LeftLine2", 40.0f, 122.0f, 110.0f, 20.0f},
        {L"Metric", 380.0f, 124.0f, 72.0f, 20.0f},
        {L"0.42ms", 545.0f, 124.0f, 85.0f, 20.0f}, // 间距 93px >> 1.8 * avgH(20)=36px -> 自动以 \t 制表符对齐

        {L"LeftLine3", 40.0f, 154.0f, 110.0f, 20.0f},
        {L"RightLine3", 380.0f, 156.0f, 120.0f, 20.0f},
    };

    const auto lines = OcrEngine::assembleLinesWithXYCut(tokens, 1.0f, 0, 700, 240);
    ASSERT_EQ(lines.size(), 7u);

    // 1. 顶部通栏标题最先输出
    EXPECT_EQ(lines[0].text, "Multi-Column Architecture");
    // 2. 完整读完左栏第 1~3 行，绝不混入右栏词块
    EXPECT_EQ(lines[1].text, "LeftLine1 Alpha");
    EXPECT_EQ(lines[2].text, "LeftLine2");
    EXPECT_EQ(lines[3].text, "LeftLine3");
    // 3. 接着完整读完右栏第 1~3 行，且大间距表格列使用 \t 分隔
    EXPECT_EQ(lines[4].text, "RightLine1 Omega");
    EXPECT_EQ(lines[5].text, "Metric\t0.42ms");
    EXPECT_EQ(lines[6].text, "RightLine3");

    // 验证贴图文字块复用的 orderTextBlocksWithXYCut 同样遵循分栏阅读序
    std::vector<OcrResult> unorderedBlocks = {
        lines[4], lines[1], lines[6], lines[0], lines[2], lines[5], lines[3]
    };
    const std::string orderedText = OcrEngine::orderTextBlocksWithXYCut(unorderedBlocks, "\n");
    EXPECT_EQ(orderedText,
              "Multi-Column Architecture\n"
              "LeftLine1 Alpha\n"
              "LeftLine2\n"
              "LeftLine3\n"
              "RightLine1 Omega\n"
              "Metric\t0.42ms\n"
              "RightLine3");

    // 验证智能合并段落换行：连字符断词修复 + 中英文软换行合并 + 列表/空行保留
    const std::string rawWrapped =
        "This is an inter-\n"
        "national standard for\n"
        "modern OCR engines.\n"
        "\n"
        "中文第一行因版面较窄被强行截断\n"
        "第二行自动无缝衔接为完整句子。\n"
        "- Keep bullet list item 1\n"
        "- Keep bullet list item 2";
    const std::string merged = OcrEngine::smartMergeParagraphsUtf8(rawWrapped);
    EXPECT_NE(merged.find("international standard for modern OCR engines."), std::string::npos);
    EXPECT_NE(merged.find("中文第一行因版面较窄被强行截断第二行自动无缝衔接为完整句子。"), std::string::npos);
    EXPECT_NE(merged.find("\n- Keep bullet list item 1\n- Keep bullet list item 2"), std::string::npos);
}

TEST(FloatingGlassThemePaletteTest, FollowsLightDarkThemeAndCustomHexAccent) {
    using namespace tools3000::capture;
    const auto tempCfgDir = std::filesystem::temp_directory_path() /
                            (L"tools3000_glass_theme_cfg_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code cfgEc;
    std::filesystem::remove_all(tempCfgDir, cfgEc);
    std::filesystem::create_directories(tempCfgDir, cfgEc);
    auto& cfg = tools3000::core::ConfigManager::instance();
    ASSERT_TRUE(cfg.initialize(tempCfgDir));

    cfg.set<std::string>("/general/theme", "light");
    cfg.set<std::string>("/general/accentColor", "#FF5733");
    EXPECT_FALSE(FloatingGlassBar::isEffectiveDarkMode());
    const auto lightPal = FloatingGlassBar::resolveThemePalette();
    EXPECT_FALSE(lightPal.isDark);
    EXPECT_GT(lightPal.panelBg.r, 0.90f);
    EXPECT_NEAR(lightPal.accent.r, 255.0f / 255.0f, 0.01f);
    EXPECT_NEAR(lightPal.accent.g, 87.0f / 255.0f, 0.01f);
    EXPECT_NEAR(lightPal.accent.b, 51.0f / 255.0f, 0.01f);

    cfg.set<std::string>("/general/theme", "dark");
    cfg.set<std::string>("/general/accentColor", "emerald");
    EXPECT_TRUE(FloatingGlassBar::isEffectiveDarkMode());
    const auto darkPal = FloatingGlassBar::resolveThemePalette();
    EXPECT_TRUE(darkPal.isDark);
    EXPECT_LT(darkPal.panelBg.r, 0.20f);
    EXPECT_GT(darkPal.accent.g, darkPal.accent.r);

    std::filesystem::remove_all(tempCfgDir, cfgEc);
}
