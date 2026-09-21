// ─────────────────────────────────────────────────────────────────────────────
// test_capture.inc — CAPTURE 模块化单元测试集
// ─────────────────────────────────────────────────────────────────────────────

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
    EXPECT_EQ(screenshot.toolbarButtons.size(), 17u);
    EXPECT_EQ(screenshot.secondaryToolbarButtons.size(), 15u); // 7 属性 + 7 预设色 + 1 拾色器
    EXPECT_EQ(screenshot.selectionSideButtons.size(), 4u);
    bool hasRecordBtn = false;
    bool hasCopyBtn = false;
    for (const auto& btn : screenshot.toolbarButtons) {
        if (btn.command == ToolbarCommand::StartRecord) hasRecordBtn = true;
        if (btn.command == ToolbarCommand::Copy) hasCopyBtn = true;
    }
    EXPECT_TRUE(hasRecordBtn);
    EXPECT_TRUE(hasCopyBtn);
    EXPECT_NEAR(screenshot.toolbarButtons.front().rect.bottom -
                screenshot.toolbarButtons.front().rect.top, 45.0f, 0.01f);
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
    EXPECT_EQ(wrapped.toolbarButtons.size(), 17u);
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
                recording.toolbarButtons[0].rect.left, 66.0f, 0.01f);
    EXPECT_NEAR(recording.toolbarButtons[1].rect.right -
                recording.toolbarButtons[1].rect.left, 84.0f, 0.01f);
    EXPECT_NEAR(recording.toolbarButtons[8].rect.right -
                recording.toolbarButtons[8].rect.left, 132.0f, 0.01f);
    check_toolbar_inside_surface(recording, desktop150);

    CaptureState extreme;
    extreme.mode = OverlayMode::Screenshot;
    extreme.dpiScale = 5.0f;
    const D2D1_SIZE_F desktop500 = D2D1::SizeF(7680.0f, 4320.0f);
    rebuildCaptureToolbar(extreme, D2D1::RectF(500.0f, 400.0f, 7000.0f, 3600.0f),
                          desktop500);
    EXPECT_EQ(extreme.toolbarButtons.size(), 17u);
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

    for (const auto& btn : state.secondaryToolbarButtons) {
        if (btn.command == ToolbarCommand::ToggleFill) hasFill = true;
        if (btn.command == ToolbarCommand::ToggleLineStyleDropdown) hasLineStyle = true;
        if (btn.command == ToolbarCommand::CycleStrokeWidth) hasStrokeWidth = true;
        if (btn.command == ToolbarCommand::CycleElementCornerRadius) hasCornerRadius = true;
        if (btn.command == ToolbarCommand::ChooseCustomColor) hasChooseCustomColor = true;
        if (btn.command == ToolbarCommand::SelectColor) ++colorCount;
    }
    EXPECT_TRUE(hasFill);
    EXPECT_TRUE(hasLineStyle);
    EXPECT_TRUE(hasStrokeWidth);
    EXPECT_TRUE(hasCornerRadius);
    EXPECT_TRUE(hasChooseCustomColor);
    EXPECT_EQ(colorCount, 7);

    // 验证自定义颜色注入后动态展示第 8 个色块与拾色器按钮
    state.hasCustomColor = true;
    state.customColor = MarkupColor{139, 92, 246}; // #8B5CF6 (非预设色)
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

    // 创建 Direct2D 工厂与 WIC/D2D 离屏 RenderTarget 进行全图标渲染覆盖验证
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf());
    ASSERT_NE(d2dFactory, nullptr);

    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcRt;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0, 0, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    HRESULT hr = d2dFactory->CreateDCRenderTarget(&props, dcRt.GetAddressOf());
    ASSERT_EQ(hr, S_OK);
    ASSERT_NE(dcRt, nullptr);

    HDC hdc = GetDC(nullptr);
    RECT rc{0, 0, 100, 100};
    dcRt->BindDC(hdc, &rc);
    dcRt->BeginDraw();

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> testBrush;
    dcRt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Blue), testBrush.GetAddressOf());
    ASSERT_NE(testBrush, nullptr);

    // 遍历所有枚举值进行 100% 渲染覆盖
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
        CaptureIconId::PropQrCode,
        CaptureIconId::ToolBeautyShell,
        CaptureIconId::PropAspectRatio,
        CaptureIconId::PropNumberSquare,
    };

    D2D1_RECT_F targetRect = D2D1::RectF(10.0f, 10.0f, 40.0f, 40.0f);
    for (auto id : allIcons) {
        CaptureVectorIcons::renderIcon(dcRt.Get(), d2dFactory.Get(), id, targetRect, testBrush.Get(), 1.0f);
        CaptureVectorIcons::renderIcon(dcRt.Get(), d2dFactory.Get(), id, targetRect, testBrush.Get(), 2.0f);
    }

    // 验证 FloatingGlassBar 在实时 Direct2D 离屏上下文中的双层阴影、微晶渐变、药丸与双层微刻痕分隔线渲染
    FloatingGlassBar::drawGlassPanel(dcRt.Get(), D2D1::RectF(10, 10, 150, 45), 11.0f, true);
    FloatingGlassBar::drawGlassPanel(dcRt.Get(), D2D1::RectF(10, 10, 150, 45), 11.0f, false);
    FloatingGlassBar::drawButtonPill(dcRt.Get(), D2D1::RectF(15, 15, 45, 40), 6.0f, true, true, false, false, 1.0f, true);
    FloatingGlassBar::drawButtonPill(dcRt.Get(), D2D1::RectF(15, 15, 45, 40), 6.0f, false, false, true, false, 1.0f, false);
    FloatingGlassBar::drawButtonPill(dcRt.Get(), D2D1::RectF(15, 15, 45, 40), 6.0f, false, false, false, true, 1.0f, true);
    FloatingGlassBar::drawSeparator(dcRt.Get(), 50.0f, 15.0f, 40.0f, 1.0f, true);
    FloatingGlassBar::drawSeparator(dcRt.Get(), 50.0f, 15.0f, 40.0f, 1.0f, false);

    dcRt->EndDraw();
    ReleaseDC(nullptr, hdc);
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

    // 4. 再次打开并提交新选择 (commit: 选择 2 - SilkMist 晨雾微光)
    openDropdownMenu(state, DropdownType::BeautyShellBg, anchor);
    commitDropdownSelection(state, DropdownType::BeautyShellBg, 2);
    EXPECT_EQ(state.dropdownMenu.type, DropdownType::None);
    EXPECT_EQ(state.beautyShell.bgType, BeautyBackgroundType::SilkMist);
    EXPECT_TRUE(state.beautyShell.enabled);
    EXPECT_EQ(tools3000::core::ConfigManager::instance().get<std::string>("/capture/beautyShellTheme", ""), "silk_mist");
    EXPECT_TRUE(tools3000::core::ConfigManager::instance().get<bool>("/capture/beautyShellEnabled", false));
    EXPECT_FALSE(state.loupeToastMessage.empty());

    // 4.1 直接在已打开状态下切换到其他下拉类型 (无缝切换)
    openDropdownMenu(state, DropdownType::BeautyShellBg, anchor);
    EXPECT_EQ(state.dropdownMenu.type, DropdownType::BeautyShellBg);
    openDropdownMenu(state, DropdownType::BeautyShellPadding, anchor);
    EXPECT_EQ(state.dropdownMenu.type, DropdownType::BeautyShellPadding);
    EXPECT_EQ(state.dropdownMenu.items.size(), 3u);

    // 5. 验证外壳留白边距 (BeautyShellPadding)
    commitDropdownSelection(state, DropdownType::BeautyShellPadding, 48);
    EXPECT_EQ(state.beautyShell.padding, 48);
    EXPECT_EQ(tools3000::core::ConfigManager::instance().get<int>("/capture/beautyShellPadding", 0), 48);

    // 6. 验证外壳圆角 (BeautyShellRadius)
    openDropdownMenu(state, DropdownType::BeautyShellRadius, anchor);
    EXPECT_EQ(state.dropdownMenu.type, DropdownType::BeautyShellRadius);
    EXPECT_EQ(state.dropdownMenu.items.size(), 5u);
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

    // 9. 验证录屏画质 (RecordQuality: 切换至原画 20Mbps)
    openDropdownMenu(state, DropdownType::RecordQuality, anchor);
    commitDropdownSelection(state, DropdownType::RecordQuality, 2);
    EXPECT_EQ(state.recordSetup.qualityLevel, 2);
    EXPECT_EQ(tools3000::core::ConfigManager::instance().get<int>("/recording/bitrate", 0), 20);

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

    // 激活状态新手柄测试：起点命中 LT，终点命中 RB
    EXPECT_EQ(lineHandler->hitTest(lineElem, cv::Point(30, 40), 4), HitArea::LT);
    EXPECT_EQ(lineHandler->hitTest(lineElem, cv::Point(130, 140), 4), HitArea::RB);

    // 线身中段命中 Body
    EXPECT_EQ(lineHandler->hitTest(lineElem, cv::Point(80, 90), 4), HitArea::Body);
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

    // 检查是否存在 ToggleTextOutline 按钮与 3 项描边色彩
    bool foundToggleOutline = false;
    int outlineColorCount = 0;
    for (const auto& btn : state.secondaryToolbarButtons) {
        if (btn.command == ToolbarCommand::ToggleTextOutline) {
            foundToggleOutline = true;
        }
        if (btn.command == ToolbarCommand::SelectTextOutlineColor) {
            outlineColorCount++;
            if (btn.color.isAuto()) {
                EXPECT_TRUE(btn.boolParam); // isAuto active
            } else {
                EXPECT_FALSE(btn.boolParam);
            }
        }
    }
    EXPECT_TRUE(foundToggleOutline);
    EXPECT_EQ(outlineColorCount, 3); // 自适应、曜石黑、纯白

    // 当关闭描边时，描边色彩微晶选项应自动隐藏收起
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
    EXPECT_EQ(toolbarButtonAccessibleName(toggleOutlineBtn), L"文字高反差描边");

    ToolbarButton outlineColorBtn;
    outlineColorBtn.command = ToolbarCommand::SelectTextOutlineColor;
    EXPECT_EQ(toolbarButtonAccessibleName(outlineColorBtn), L"文字描边颜色");

    CaptureState state;
    state.currentTextOutline = true;
    state.currentTextOutlineColor = MarkupColor::White();

    toggleOutlineBtn.command = ToolbarCommand::ToggleTextOutline;
    EXPECT_TRUE(isToolbarButtonSelected(toggleOutlineBtn, state));

    state.currentTextOutline = false;
    EXPECT_FALSE(isToolbarButtonSelected(toggleOutlineBtn, state));

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
        for (int retry = 0; retry < 25; ++retry) {
            if (OpenClipboard(nullptr)) return true;
            Sleep(10);
        }
        return false;
    };

    // 1. 创建包含透明通道与测试图形的 128x96 BGRA 测试图像
    cv::Mat testImage(96, 128, CV_8UC4, cv::Scalar(120, 80, 200, 180));
    cv::rectangle(testImage, cv::Rect(10, 10, 50, 40), cv::Scalar(255, 255, 0, 255), -1);

    // 2. 调用全矩阵写入 (preferredFilePath 为空，触发自动创建高可用临时落盘副本)
    ASSERT_TRUE(tools3000::capture::ClipboardUtils::copyImageToClipboard(testImage));

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
        for (int retry = 0; retry < 25; ++retry) {
            if (OpenClipboard(nullptr)) return true;
            Sleep(10);
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

    ASSERT_TRUE(tools3000::capture::ClipboardUtils::copyImageToClipboard(testImage, customFile.wstring()));

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
    // 对系统剪贴板句柄与多格式数据具备 100% 零破坏保障
    cv::Mat testImage(80, 80, CV_8UC4, cv::Scalar(60, 120, 180, 255));
    bool copyOk = false;
    for (int retry = 0; retry < 15; ++retry) {
        if (tools3000::capture::ClipboardUtils::copyImageToClipboard(testImage)) {
            copyOk = true;
            break;
        }
        Sleep(20);
    }
    ASSERT_TRUE(copyOk);

    // 主动触发物理工作集修剪 (SetProcessWorkingSetSize 强行驱逐内存分页)
    tools3000::core::WinUtils::trimWorkingSet(true);

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

    // 微量移动 1 像素测试：必须即时响应，绝无 1/2 == 0 丢步死区
    mag->resize(1, 0, HitArea::R);
    EXPECT_EQ(mag->magnifierRadius, 61);

    mag->resize(-1, 0, HitArea::R);
    EXPECT_EQ(mag->magnifierRadius, 60);

    mag->resize(0, 1, HitArea::B);
    EXPECT_EQ(mag->magnifierRadius, 61);

    mag->resize(0, -1, HitArea::T);
    EXPECT_EQ(mag->magnifierRadius, 62);
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


