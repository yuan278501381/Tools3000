// ─────────────────────────────────────────────────────────────────────────────
// test_live_ocr.inc — 微信级原图实景高精度离线文字识别 (Live Text OCR) 专项单元测试套件
//
// 覆盖范围:
//   1. UTF-8 码点拆分与字符级 charBoxes 精确比例切分、XY-Cut 词块子框对齐
//   2. DeepOcrEngine: DBNet 概率图 Unclip 四点检测、透视矫正、CTC Greedy 单字符时间步解码与 ONNX 魔数校验
//   3. OcrTableReconstructor: 多行多列表格重建、空白缺省格对齐、CF_HTML 字节偏移头与双格式系统剪贴板实测
//   4. OpenCV QRCodeEncoder + detectQrCodes 正色/深底反色二维码合成图毫秒级解码
//   5. 黑底浅字/低对比度代码图像 CLAHE + 反相自适应增强识别
//   6. PinInputController & PinWindow 零门槛悬停 IDC_IBEAM、字符级流式选字、双击选词、表格胶囊与空白处拖拽窗口端到端测试
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include <opencv2/objdetect.hpp>
#include <opencv2/imgproc.hpp>

TEST(LiveOcrCharBoxesTest, ProportionalCodepointSplittingAndXYCutSubBoxes) {
    using namespace tools3000::ocr;

    const std::string mixed = "Hi世界2026";
    const auto cps = OcrEngine::splitUtf8Codepoints(mixed);
    ASSERT_EQ(cps.size(), 8u);
    EXPECT_EQ(cps[0], "H");
    EXPECT_EQ(cps[1], "i");
    EXPECT_EQ(cps[2], "世");
    EXPECT_EQ(cps[3], "界");
    EXPECT_EQ(cps[7], "6");

    EXPECT_EQ(OcrEngine::sliceUtf8Codepoints(mixed, 1, 5), "i世界2");

    const cv::Rect lineBox(20, 40, 160, 24);
    const auto boxes = OcrEngine::buildProportionalCharBoxes(mixed, lineBox);
    ASSERT_EQ(boxes.size(), 8u);
    EXPECT_EQ(boxes.front().x, 20);
    EXPECT_EQ(boxes.back().x + boxes.back().width, 180);
    // 全角汉字宽度显著大于窄字母 'i'
    EXPECT_GT(boxes[2].width, boxes[1].width);
    for (size_t i = 0; i + 1 < boxes.size(); ++i) {
        EXPECT_LE(boxes[i].x, boxes[i + 1].x);
        EXPECT_EQ(boxes[i].y, 40);
        EXPECT_EQ(boxes[i].height, 24);
    }

    // 验证 XY-Cut 行组装保留词级精确位置并在列走廊处生成独立 \t 间隙框
    std::vector<OcrWordToken> tokens = {
        {L"Alpha", 10.0f, 20.0f, 50.0f, 18.0f},
        {L"数据",  120.0f, 21.0f, 40.0f, 18.0f},
    };
    const auto assembled = OcrEngine::assembleLinesWithXYCut(tokens, 1.0f, 0, 300, 100);
    ASSERT_EQ(assembled.size(), 1u);
    EXPECT_EQ(assembled[0].text, "Alpha\t数据");
    const size_t expectedCount = OcrEngine::countUtf8Codepoints(assembled[0].text);
    ASSERT_EQ(assembled[0].charBoxes.size(), expectedCount);
    // 第 5 个字符是制表符 '\t'，位于左词右缘 (60) 与右词左缘 (120) 之间
    EXPECT_GE(assembled[0].charBoxes[5].x, 58);
    EXPECT_LE(assembled[0].charBoxes[5].x + assembled[0].charBoxes[5].width, 122);
    // "数" 的起点紧贴右词起点 120，绝不被中间大空白均摊拉偏
    EXPECT_NEAR(assembled[0].charBoxes[6].x, 120, 2);
}

TEST(DeepOcrEngineTest, DbPostprocessPerspectiveCtcCharBoxesAndOnnxValidation) {
    using namespace tools3000::ocr;

    // 1. ONNX 魔数安全校验：拦截 HTML / JSON 伪文件，放行合法 Protobuf ModelProto 头
    const std::string htmlErrorPage = "<!DOCTYPE html><html><body>404 Not Found</body></html>";
    EXPECT_FALSE(DeepOcrEngine::isValidOnnxBuffer(
        reinterpret_cast<const uint8_t*>(htmlErrorPage.data()), htmlErrorPage.size(), 16));

    std::vector<uint8_t> validMockOnnx(512, 0);
    validMockOnnx[0] = 0x08; // field 1 (ir_version), varint
    validMockOnnx[1] = 0x07;
    validMockOnnx[2] = 0x12; // field 2 (producer_name), length-delimited
    validMockOnnx[3] = 0x04;
    EXPECT_TRUE(DeepOcrEngine::isValidOnnxBuffer(validMockOnnx.data(), validMockOnnx.size(), 256));

    // 2. DBNet 概率图后处理与透视变换倾斜拉平
    cv::Mat probMap = cv::Mat::zeros(160, 320, CV_32FC1);
    cv::RotatedRect tiltedBox(cv::Point2f(160.0f, 80.0f), cv::Size2f(140.0f, 24.0f), -12.0f);
    cv::Point2f rpts[4]{};
    tiltedBox.points(rpts);
    std::vector<cv::Point> polyPts;
    for (const auto& p : rpts) polyPts.emplace_back(static_cast<int>(p.x), static_cast<int>(p.y));
    cv::fillConvexPoly(probMap, polyPts, cv::Scalar(0.92f));

    const auto quads = DeepOcrEngine::postprocessDbMap(probMap, 320, 160, 1.0f, 1.0f, 0.30f, 0.55f, 1.5f);
    ASSERT_EQ(quads.size(), 1u);
    EXPECT_GT(quads[0].score, 0.80f);

    cv::Mat sampleSrc(160, 320, CV_8UC3, cv::Scalar(250, 250, 250));
    cv::Mat invPersp;
    bool rotVert = true;
    cv::Mat cropped = DeepOcrEngine::warpPerspectiveQuad(sampleSrc, quads[0].corners, &invPersp, &rotVert);
    EXPECT_FALSE(cropped.empty());
    EXPECT_FALSE(rotVert);
    EXPECT_GT(cropped.cols, cropped.rows * 2);
    ASSERT_EQ(invPersp.rows, 3);
    ASSERT_EQ(invPersp.cols, 3);

    // 3. SVTR / CRNN CTC Greedy 解码：验证连续同字符折叠、blank 分隔重复字符保留与单字符时间步切分
    // 词表: 1->"C", 2->"o", 3->"d", 4->"e", 5->"中", 6(space)
    const std::vector<std::string> dict = {"C", "o", "d", "e", "中"};
    const int T = 20;
    const int C = 7; // 0=blank, 1..5=dict, 6=space
    cv::Mat logits(T, C, CV_32FC1, cv::Scalar(-5.0f));
    for (int t = 0; t < T; ++t) logits.at<float>(t, 0) = 4.0f; // 默认为 blank

    auto setStepClass = [&](int t, int cls) {
        logits.at<float>(t, 0) = -4.0f;
        logits.at<float>(t, cls) = 6.0f;
    };
    // 构造 "Code 中" 且测试 "oo" -> 这里构造 "Coo中":
    // t=1,2 -> 'C'(1); t=4,5 -> 'o'(2); t=6 -> blank(0); t=7 -> 'o'(2); t=10 -> space(6); t=13,14 -> '中'(5)
    setStepClass(1, 1);
    setStepClass(2, 1);
    setStepClass(4, 2);
    setStepClass(5, 2);
    setStepClass(7, 2);
    setStepClass(10, 6);
    setStepClass(13, 5);
    setStepClass(14, 5);

    const auto decoded = DeepOcrEngine::decodeCtcGreedy(logits, dict, 0.85f);
    EXPECT_EQ(decoded.text, "Coo 中");
    EXPECT_GT(decoded.confidence, 0.95f);
    ASSERT_EQ(decoded.chars.size(), 5u);
    EXPECT_EQ(decoded.chars[0].utf8Char, "C");
    EXPECT_EQ(decoded.chars[1].utf8Char, "o");
    EXPECT_EQ(decoded.chars[2].utf8Char, "o");
    EXPECT_EQ(decoded.chars[3].utf8Char, " ");
    EXPECT_EQ(decoded.chars[4].utf8Char, "中");

    for (size_t i = 0; i + 1 < decoded.chars.size(); ++i) {
        EXPECT_LT(decoded.chars[i].normLeft, decoded.chars[i].normRight);
        EXPECT_LE(decoded.chars[i].normRight, decoded.chars[i + 1].normLeft + 1e-4f);
    }

    const auto projectedBoxes = DeepOcrEngine::projectCharSpansToImage(
        decoded.text, decoded.chars, cropped.cols, cropped.rows,
        false, false, invPersp, cv::Rect(80, 60, 160, 32), 320, 160);
    ASSERT_EQ(projectedBoxes.size(), 5u);
    EXPECT_LT(projectedBoxes[0].x, projectedBoxes[4].x);
}

TEST(OcrTableReconstructorTest, GridAlignmentCfHtmlOffsetsAndDualClipboardWrite) {
    using namespace tools3000::ocr;

    // 1. 纯多行正文段落不应误判为表格
    std::vector<OcrResult> prose = {
        {"第一行普通文章内容，没有任何多列对齐结构。", 0.99f, cv::Rect(20, 20, 320, 22), {}},
        {"第二行继续陈述系统架构与工程规范要求。", 0.99f, cv::Rect(20, 50, 310, 22), {}},
        {"第三行收尾。", 0.99f, cv::Rect(20, 80, 120, 22), {}},
    };
    EXPECT_FALSE(OcrTableReconstructor::reconstruct(prose).isTable);

    // 2. 构造 3 行 x 3 列表格（第 2 行中间列为空缺格，第 3 行含 HTML 特殊字符 < > &）
    std::vector<OcrResult> tableBlocks = {
        {"模块名称", 0.99f, cv::Rect(20,  20, 80, 22), {}},
        {"延迟指标", 0.99f, cv::Rect(160, 20, 80, 22), {}},
        {"状态说明", 0.99f, cv::Rect(300, 20, 80, 22), {}},

        {"PP-OCRv4", 0.99f, cv::Rect(22,  56, 84, 22), {}},
        // 中间列留空，测试跨列对位不左移错位
        {"已就绪",   0.99f, cv::Rect(302, 56, 64, 22), {}},

        {"Quirc QR", 0.99f, cv::Rect(20,  92, 82, 22), {}},
        {"< 5ms",    0.99f, cv::Rect(162, 92, 58, 22), {}},
        {"HTML & TSV", 0.99f, cv::Rect(298, 92, 92, 22), {}},
    };

    const OcrTableGrid grid = OcrTableReconstructor::reconstruct(tableBlocks);
    ASSERT_TRUE(grid.isTable);
    ASSERT_EQ(grid.rowCount, 3u);
    ASSERT_EQ(grid.colCount, 3u);
    EXPECT_EQ(grid.cells[0][0], "模块名称");
    EXPECT_EQ(grid.cells[0][1], "延迟指标");
    EXPECT_EQ(grid.cells[0][2], "状态说明");
    EXPECT_EQ(grid.cells[1][0], "PP-OCRv4");
    EXPECT_EQ(grid.cells[1][1], "");
    EXPECT_EQ(grid.cells[1][2], "已就绪");
    EXPECT_EQ(grid.cells[2][1], "< 5ms");
    EXPECT_EQ(grid.cells[2][2], "HTML & TSV");

    const std::string tsv = grid.toTsv("\r\n");
    EXPECT_NE(tsv.find("PP-OCRv4\t\t已就绪"), std::string::npos);

    const std::string htmlFrag = grid.toHtmlTable();
    EXPECT_NE(htmlFrag.find("&lt; 5ms"), std::string::npos);
    EXPECT_NE(htmlFrag.find("HTML &amp; TSV"), std::string::npos);

    // 3. 严格校验 Windows CF_HTML 字节偏移头精确性
    const std::string cfHtml = OcrTableReconstructor::buildCfHtmlClipboardPayload(htmlFrag);
    auto extractOffset = [&](const char* key) -> size_t {
        const size_t pos = cfHtml.find(key);
        if (pos == std::string::npos) return 0;
        return static_cast<size_t>(std::strtoull(cfHtml.c_str() + pos + std::strlen(key), nullptr, 10));
    };
    const size_t startHtml = extractOffset("StartHTML:");
    const size_t endHtml = extractOffset("EndHTML:");
    const size_t startFrag = extractOffset("StartFragment:");
    const size_t endFrag = extractOffset("EndFragment:");

    ASSERT_GT(startHtml, 0u);
    EXPECT_EQ(endHtml, cfHtml.size());
    EXPECT_EQ(cfHtml.substr(startFrag, endFrag - startFrag), htmlFrag);

    // 4. 真实写入系统剪贴板并同时回读 HTML Format 与 CF_UNICODETEXT
    if (OcrTableReconstructor::copyTableToClipboard(grid)) {
        const UINT htmlFmt = RegisterClipboardFormatW(L"HTML Format");
        if (OpenClipboard(nullptr)) {
            HANDLE hHtml = GetClipboardData(htmlFmt);
            HANDLE hUni = GetClipboardData(CF_UNICODETEXT);
            EXPECT_NE(hHtml, nullptr);
            EXPECT_NE(hUni, nullptr);
            if (hHtml) {
                const char* rawHtml = static_cast<const char*>(GlobalLock(hHtml));
                if (rawHtml) {
                    std::string clipHtml(rawHtml);
                    GlobalUnlock(hHtml);
                    EXPECT_NE(clipHtml.find("<table"), std::string::npos);
                    EXPECT_NE(clipHtml.find("PP-OCRv4"), std::string::npos);
                }
            }
            if (hUni) {
                const wchar_t* rawUni = static_cast<const wchar_t*>(GlobalLock(hUni));
                if (rawUni) {
                    std::wstring clipTsv(rawUni);
                    GlobalUnlock(hUni);
                    EXPECT_NE(clipTsv.find(L"PP-OCRv4\t\t"), std::wstring::npos);
                }
            }
            CloseClipboard();
        }
    }
}

TEST(LiveOcrQrAndDarkEnhancementTest, DecodesSyntheticQrAndEnhancesDarkBackgroundText) {
    using namespace tools3000::ocr;

    // 1. 使用 OpenCV QRCodeEncoder 合成真实二维码图像（含普通白底黑码与暗黑模式反色码）
    const std::string expectedPayload = "https://github.com/yuan278501381/tools3000?live=ocr2026";
    cv::Mat qrBits;
    auto encoder = cv::QRCodeEncoder::create();
    encoder->encode(expectedPayload, qrBits);
    ASSERT_FALSE(qrBits.empty());

    cv::Mat qrLarge;
    cv::resize(qrBits, qrLarge, cv::Size(qrBits.cols * 8, qrBits.rows * 8), 0.0, 0.0, cv::INTER_NEAREST);
    cv::Mat canvas(qrLarge.rows + 80, qrLarge.cols + 80, CV_8UC3, cv::Scalar(255, 255, 255));
    cv::Mat qrBgr;
    cv::cvtColor(qrLarge, qrBgr, cv::COLOR_GRAY2BGR);
    qrBgr.copyTo(canvas(cv::Rect(40, 40, qrLarge.cols, qrLarge.rows)));

    const auto qrHits = OcrEngine::detectQrCodes(canvas);
    ASSERT_EQ(qrHits.size(), 1u);
    EXPECT_EQ(qrHits[0].text, expectedPayload);
    EXPECT_GE(qrHits[0].boundingBox.x, 30);
    EXPECT_GE(qrHits[0].boundingBox.y, 30);
    EXPECT_GT(qrHits[0].boundingBox.width, 100);

    // 暗黑模式反白二维码顺手识别验证
    cv::Mat darkQrCanvas = cv::Scalar(265, 265, 265) - canvas; // 白变深灰/黑，黑码变亮白
    darkQrCanvas = cv::Scalar(255, 255, 255) - canvas;
    darkQrCanvas = darkQrCanvas * 0.85 + cv::Scalar(20, 20, 24);
    EXPECT_TRUE(OcrEngine::isDarkBackgroundImage(darkQrCanvas));
    const auto darkQrHits = OcrEngine::detectQrCodes(darkQrCanvas);
    ASSERT_EQ(darkQrHits.size(), 1u);
    EXPECT_EQ(darkQrHits[0].text, expectedPayload);

    // 2. 深底浅字低对比代码截图增强识别验证
    cv::Mat darkCodeImg(140, 540, CV_8UC3, cv::Scalar(24, 24, 36));
    cv::putText(darkCodeImg, "Tools3000 LiveOCR 2026", cv::Point(28, 86),
                cv::FONT_HERSHEY_SIMPLEX, 1.15, cv::Scalar(118, 128, 148), 2, cv::LINE_AA);

    EXPECT_TRUE(OcrEngine::isDarkBackgroundImage(darkCodeImg));
    bool wasInverted = false;
    cv::Mat enhanced = OcrEngine::enhanceImageForRecognition(darkCodeImg, &wasInverted);
    EXPECT_TRUE(wasInverted);
    EXPECT_FALSE(enhanced.empty());
    EXPECT_FALSE(OcrEngine::isDarkBackgroundImage(enhanced));

    if (OcrEngine::instance().initialize() && OcrEngine::instance().isAvailable()) {
        const auto lines = OcrEngine::instance().extractText(darkCodeImg);
        ASSERT_FALSE(lines.empty());
        std::string allRec;
        for (const auto& ln : lines) {
            allRec += ln.text;
            EXPECT_EQ(ln.charBoxes.size(), OcrEngine::countUtf8Codepoints(ln.text));
        }
        EXPECT_NE(allRec.find("2026"), std::string::npos);
    }
}

TEST(PinLiveTextInteractionTest, ZeroThresholdIBeamCursorCharSelectionDoubleClickAndTablePill) {
    using namespace tools3000::capture;

    // 1. PinInputController 原图实景选字状态机纯函数契约测试
    {
        PinLiveTextState st{};
        st.isFolded = false;
        st.liveTextEnabled = true;
        st.ocrReady = true;

        // 平时指向无字空白区域 -> 普通窗口平移指针 IDC_SIZEALL + 按下直接拖拽窗口
        st.hitTextBlock = false;
        EXPECT_EQ(PinInputController::resolveCursorKind(st), PinCursorKind::MoveWindow);
        EXPECT_EQ(PinInputController::resolveLeftPressMode(st, false), PinLeftPressMode::StartWindowDrag);

        // 无需按 Alt、无需开启 textSelectionMode，鼠标一旦指向已识别文字框 -> 自动切换 IDC_IBEAM + 按下直接流式选字
        st.hitTextBlock = true;
        EXPECT_EQ(PinInputController::resolveCursorKind(st), PinCursorKind::TextBeam);
        EXPECT_EQ(PinInputController::resolveLeftPressMode(st, false), PinLeftPressMode::StartLiveTextSelection);

        // 若悬停在微晶胶囊/工具栏按钮/二维码角标上 -> 优先切换手型指针 IDC_HAND + Drag-First 按钮候选
        st.hoverInteractiveButton = true;
        EXPECT_EQ(PinInputController::resolveCursorKind(st), PinCursorKind::HandButton);
        EXPECT_EQ(PinInputController::resolveLeftPressMode(st, true), PinLeftPressMode::PressInteractiveButton);

        // 正在拖拽窗口期间 -> 锁定 IDC_SIZEALL
        st.hoverInteractiveButton = false;
        st.isDragging = true;
        EXPECT_EQ(PinInputController::resolveCursorKind(st), PinCursorKind::MoveWindow);
    }

    // 2. PinWindow 端到端真窗口测试：零门槛悬停、单字符级流式拖选、双击选词、表格胶囊浮现与空白处拖拽
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
    ASSERT_TRUE(capturePlugin->initialize());

    const auto tempPng = std::filesystem::temp_directory_path() /
                         (L"tools3000_live_pin_ocr_" + std::to_wstring(GetCurrentProcessId()) + L".png");

    struct CleanupGuard {
        HMODULE mod = nullptr;
        tools3000::core::IPlugin* plugin = nullptr;
        std::filesystem::path png;
        ~CleanupGuard() {
            while (HWND existing = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
                SendMessageW(existing, WM_KEYDOWN, VK_DELETE, 0);
                if (IsWindow(existing)) DestroyWindow(existing);
            }
            if (plugin) plugin->shutdown();
            if (mod) FreeLibrary(mod);
            std::error_code ec;
            std::filesystem::remove(png, ec);
        }
    } guard{hMod, capturePlugin, tempPng};

    while (HWND existing = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
        SendMessageW(existing, WM_KEYDOWN, VK_DELETE, 0);
        if (IsWindow(existing)) DestroyWindow(existing);
    }

    cv::Mat pinImg(260, 480, CV_8UC3, cv::Scalar(246, 248, 252));
    std::vector<uchar> encoded;
    ASSERT_TRUE(cv::imencode(".png", pinImg, encoded));
    {
        std::ofstream ofs(tempPng, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    }

    auto& bridge = tools3000::core::MessageBridge::instance();
    auto invokeRpc = [&](int id, const char* method, const nlohmann::json& params) -> nlohmann::json {
        nlohmann::json req = {{"id", id}, {"method", method}, {"params", params}};
        const std::string raw = bridge.handleMessage(req.dump());
        const auto parsed = nlohmann::json::parse(raw, nullptr, false);
        return parsed.is_object() && parsed.contains("result") ? parsed["result"] : parsed;
    };

    ASSERT_TRUE(invokeRpc(3101, "capture.pinImageFile", {
        {"path", tools3000::core::WinUtils::wstringToUtf8(tempPng.wstring())}
    }).value("success", false));

    HWND pinHwnd = FindWindowW(L"Tools3000_PinWindow", nullptr);
    ASSERT_NE(pinHwnd, nullptr);
    SetWindowPos(pinHwnd, nullptr, 260, 260, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    // 注入 2x2 表格文字块与 1 个二维码热区（此时 textSelectionMode 保持默认 false！）
    // 第 0 块 "ABCDEFGHIJ" 位于 x=[20, 120], w=100 (每个字母精确宽 10px!), y=[50, 74]
    // 第 2 块位于 y=[142, 166]，在 100%~200% DPI 下均避开第 0 块下方的悬浮胶囊操作条
    const auto initInspect = invokeRpc(3102, "capture.inspectPinOcrState", {
        {"mockBlocks", nlohmann::json::array({
            {{"text", "ABCDEFGHIJ"}, {"x", 20},  {"y", 50},  {"w", 100}, {"h", 24}},
            {{"text", "Col1Val"},    {"x", 200}, {"y", 50},  {"w", 80},  {"h", 24}},
            {{"text", "Row2Key hello_world"}, {"x", 20}, {"y", 142}, {"w", 150}, {"h", 24}},
            {{"text", "Col2Val"},    {"x", 200}, {"y", 142}, {"w", 80},  {"h", 24}}
        })},
        {"mockQrCodes", nlohmann::json::array({
            {{"text", "https://tools3000.local/qr"}, {"x", 360}, {"y", 150}, {"w", 72}, {"h", 72}}
        })}
    });
    ASSERT_TRUE(initInspect.value("success", false));
    EXPECT_FALSE(initInspect.value("textSelectionMode", true));
    EXPECT_TRUE(initInspect.value("hasDetectedTable", false));
    EXPECT_EQ(initInspect.value("qrCodeCount", 0), 1);

    // (A) 零门槛悬停：不按 Alt、不开模式，鼠标移至 (45, 62) -> 命中第 0 块，驱动 IDC_IBEAM
    SendMessageW(pinHwnd, WM_MOUSEMOVE, 0, MAKELPARAM(45, 62));
    const auto hoverTextInspect = invokeRpc(3103, "capture.inspectPinOcrState", nlohmann::json::object());
    EXPECT_EQ(hoverTextInspect.value("hoveredBlockIndex", -1), 0);
    EXPECT_EQ(SendMessageW(pinHwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(pinHwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)), TRUE);

    // (B) 字符级精准流式拖选：在 "ABCDEFGHIJ" (x=20..120, 每字母 10px) 上从 x=45 ('C', idx=2) 拖选到 x=75 ('F', idx=5)
    SendMessageW(pinHwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(45, 62));
    SendMessageW(pinHwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(75, 62));
    SendMessageW(pinHwnd, WM_LBUTTONUP, 0, MAKELPARAM(75, 62));

    const auto charSelInspect = invokeRpc(3104, "capture.inspectPinOcrState", nlohmann::json::object());
    EXPECT_EQ(charSelInspect.value("selectedBlockCount", 0), 1);
    EXPECT_EQ(charSelInspect.value("selectedText", ""), "CDEF");
    EXPECT_TRUE(charSelInspect.value("copyTableBtnVisible", false));
    EXPECT_TRUE(charSelInspect.value("openPanelBtnVisible", false));

    // (C) 双击选词：在 "Row2Key hello_world" 右侧单词 "hello_world" 处 (130, 154) 双击 -> 自动精准选中 "hello_world"
    SendMessageW(pinHwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(130, 154));
    SendMessageW(pinHwnd, WM_LBUTTONUP, 0, MAKELPARAM(130, 154));
    const auto dblClickInspect = invokeRpc(3105, "capture.inspectPinOcrState", nlohmann::json::object());
    EXPECT_EQ(dblClickInspect.value("selectedText", ""), "hello_world");

    // (D) 空白无字且远离胶囊与顶部悬停栏的区域 (390, 96) 按住左键拖拽：清除文字选区并 100% 正常移动贴图窗口
    RECT beforeDragRc{};
    ASSERT_TRUE(GetWindowRect(pinHwnd, &beforeDragRc));
    SendMessageW(pinHwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(390, 96));
    SendMessageW(pinHwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(428, 120));
    SendMessageW(pinHwnd, WM_LBUTTONUP, 0, MAKELPARAM(428, 120));

    RECT afterDragRc{};
    ASSERT_TRUE(GetWindowRect(pinHwnd, &afterDragRc));
    EXPECT_EQ(afterDragRc.left - beforeDragRc.left, 38);
    EXPECT_EQ(afterDragRc.top - beforeDragRc.top, 24);

    const auto afterBlankDrag = invokeRpc(3106, "capture.inspectPinOcrState", nlohmann::json::object());
    EXPECT_EQ(afterBlankDrag.value("selectedBlockCount", -1), 0);

    SendMessageW(pinHwnd, WM_KEYDOWN, VK_DELETE, 0);
    capturePlugin->shutdown();
    guard.plugin = nullptr;
    EXPECT_TRUE(FreeLibrary(hMod));
    guard.mod = nullptr;
}

TEST(LiveOcrAndTableTest, MixedLeftBadgeRightAlignedTableWithMissingCellAndBrokenPhrase) {
    using namespace tools3000::ocr;

    auto makeTok = [](const char* text, int x, int y, int w, int h = 18) {
        OcrResult r;
        r.text = text;
        r.confidence = 0.98f;
        r.box = cv::Rect(x, y, w, h);
        return r;
    };

    // 模拟真实「关于 -> 技术栈」卡片：
    // 第 1 列左对齐 (x=40)，第 2 列胶囊左对齐 (x=290，其中 Vite 行缺失胶囊)，
    // 第 3 列右对齐 (right=920，长短句左边界跨度从 560 到 740)，
    // 且 SQLite 行第 3 列中间因单词漏检出现 32px 单行缺口
    std::vector<OcrResult> blocks = {
        makeTok("C++20", 40, 40, 56),
        makeTok("MSVC v145 · x64 / ARM64", 290, 40, 175),
        makeTok("原生微内核主程序与双架构 (x64 / ARM64) 极速运行时", 560, 40, 360),

        makeTok("Direct2D & DirectComposition", 40, 82, 210),
        makeTok("GPU Hardware", 290, 82, 102),
        makeTok("截图、贴图、按键提示与鼠标特效的显卡硬件加速绘制", 575, 82, 345),

        makeTok("React & TypeScript", 40, 124, 145),
        makeTok("19.2 / 6.0", 290, 124, 76),
        makeTok("界面交互组件体系与强类型状态管理", 696, 124, 224),

        // Vite 行第 2 列胶囊漏检缺失，第 3 列右对齐 (right=920)
        makeTok("Vite", 40, 166, 38),
        makeTok("前端资源极速编译与按需分包构建引擎", 682, 166, 238),

        makeTok("Lucide Icons", 40, 208, 96),
        makeTok("1.21", 290, 208, 42),
        makeTok("全界面统一高清矢量图标体系", 738, 208, 182),

        // SQLite 行第 3 列内部出现 30px 缺口（上下多行在 x=710..740 均有文字覆盖，非垂直空白走廊）
        makeTok("SQLite & zlib", 40, 250, 106),
        makeTok("3.x / 1.3", 290, 250, 68),
        makeTok("本地按键统计、打开历史存储与", 548, 250, 164),
        makeTok("文档压缩流无损提取", 742, 250, 178),
    };

    const OcrTableGrid grid = OcrTableReconstructor::reconstruct(blocks);
    ASSERT_TRUE(grid.isTable);
    EXPECT_EQ(grid.rowCount, 6u);
    EXPECT_EQ(grid.colCount, 3u);

    // 验证 Vite 行：第 1 列 "Vite"，第 2 列保留空白 ""，第 3 列精准落入右对齐第 3 列
    ASSERT_EQ(grid.cells[3].size(), 3u);
    EXPECT_EQ(grid.cells[3][0], "Vite");
    EXPECT_EQ(grid.cells[3][1], "");
    EXPECT_EQ(grid.cells[3][2], "前端资源极速编译与按需分包构建引擎");

    // 验证 SQLite 行：单行内部断句已自动缝合进第 3 列，未产生第 4/5 幽灵列
    ASSERT_EQ(grid.cells[5].size(), 3u);
    EXPECT_EQ(grid.cells[5][0], "SQLite & zlib");
    EXPECT_EQ(grid.cells[5][1], "3.x / 1.3");
    EXPECT_NE(grid.cells[5][2].find("本地按键统计"), std::string::npos);
    EXPECT_NE(grid.cells[5][2].find("文档压缩流无损提取"), std::string::npos);
}

TEST(LiveOcrAndTableTest, ExtractEmbeddedCharacterDictFromOnnxMetadataProps) {
    using namespace tools3000::ocr;

    auto appendVarint = [](std::vector<uint8_t>& buf, uint64_t val) {
        while (val >= 0x80u) {
            buf.push_back(static_cast<uint8_t>((val & 0x7Fu) | 0x80u));
            val >>= 7u;
        }
        buf.push_back(static_cast<uint8_t>(val));
    };

    std::string dictBody = "\xEF\xBB\xBF" "0\r\n1\nA\nB\n中\n文\n测\n试\n字\n典\n提取\n成功\n!\n@\n#\n$\n%\n^";
    std::vector<uint8_t> entryProto;
    // field 1 (key = "character"): tag 0x0A
    entryProto.push_back(0x0A);
    appendVarint(entryProto, 9);
    const char* keyStr = "character";
    entryProto.insert(entryProto.end(), keyStr, keyStr + 9);
    // field 2 (value = dictBody): tag 0x12
    entryProto.push_back(0x12);
    appendVarint(entryProto, dictBody.size());
    entryProto.insert(entryProto.end(), dictBody.begin(), dictBody.end());

    std::vector<uint8_t> fakeOnnx;
    // ir_version = 8 (field 1, varint -> 0x08 0x08)
    fakeOnnx.push_back(0x08);
    fakeOnnx.push_back(0x08);
    // metadata_props (field 14, length-delimited -> (14 << 3) | 2 = 0x72)
    fakeOnnx.push_back(0x72);
    appendVarint(fakeOnnx, entryProto.size());
    fakeOnnx.insert(fakeOnnx.end(), entryProto.begin(), entryProto.end());

    std::vector<std::string> extracted;
    ASSERT_TRUE(DeepOcrEngine::extractEmbeddedCharacterDictFromOnnx(
        fakeOnnx.data(), fakeOnnx.size(), extracted));
    ASSERT_GE(extracted.size(), 16u);
    EXPECT_EQ(extracted[0], "0");
    EXPECT_EQ(extracted[1], "1");
    EXPECT_EQ(extracted[4], "中");
    EXPECT_EQ(extracted[5], "文");
}

TEST(DeepOcrEngineTest, PatchOnnxGraphInputShapeFixesDynamicSymbolicDimensions) {
    using namespace tools3000::ocr;

    auto appendVarint = [](std::vector<uint8_t>& buf, uint64_t val) {
        while (val >= 0x80u) {
            buf.push_back(static_cast<uint8_t>((val & 0x7Fu) | 0x80u));
            val >>= 7u;
        }
        buf.push_back(static_cast<uint8_t>(val));
    };
    auto appendLenDelimited = [&](std::vector<uint8_t>& dst, uint32_t fieldNum, const std::vector<uint8_t>& payload) {
        appendVarint(dst, (static_cast<uint64_t>(fieldNum) << 3u) | 2u);
        appendVarint(dst, payload.size());
        dst.insert(dst.end(), payload.begin(), payload.end());
    };
    auto makeSymDim = [&](const std::string& param) {
        std::vector<uint8_t> d;
        appendVarint(d, (2u << 3u) | 2u); // dim_param (field 2, string)
        appendVarint(d, param.size());
        d.insert(d.end(), param.begin(), param.end());
        return d;
    };
    auto makeValDim = [&](uint64_t v) {
        std::vector<uint8_t> d;
        appendVarint(d, (1u << 3u) | 0u); // dim_value (field 1, int64)
        appendVarint(d, v);
        return d;
    };

    // 构造模拟 paddle2onnx 导出的动态符号维度 [p2o.DynamicDimension.0, 3, ?, p2o.DynamicDimension.1]
    std::vector<uint8_t> shapeProto;
    appendLenDelimited(shapeProto, 1, makeSymDim("p2o.DynamicDimension.0"));
    appendLenDelimited(shapeProto, 1, makeValDim(3));
    appendLenDelimited(shapeProto, 1, makeSymDim("?"));
    appendLenDelimited(shapeProto, 1, makeSymDim("p2o.DynamicDimension.1"));

    std::vector<uint8_t> tensorTypeProto;
    tensorTypeProto.push_back(0x08); // elem_type = FLOAT (1)
    tensorTypeProto.push_back(0x01);
    appendLenDelimited(tensorTypeProto, 2, shapeProto); // field 2: shape

    std::vector<uint8_t> typeProto;
    appendLenDelimited(typeProto, 1, tensorTypeProto); // field 1: tensor_type

    std::vector<uint8_t> valueInfoProto;
    const std::vector<uint8_t> inputName = {'x'};
    appendLenDelimited(valueInfoProto, 1, inputName);   // field 1: name = "x"
    appendLenDelimited(valueInfoProto, 2, typeProto);   // field 2: type

    std::vector<uint8_t> graphProto;
    appendLenDelimited(graphProto, 11, valueInfoProto); // field 11: input

    std::vector<uint8_t> modelProto;
    modelProto.push_back(0x08); // ir_version = 8
    modelProto.push_back(0x08);
    appendLenDelimited(modelProto, 7, graphProto);      // field 7: graph

    std::vector<uint8_t> patched;
    ASSERT_TRUE(DeepOcrEngine::patchOnnxGraphInputShape(
        modelProto.data(), modelProto.size(), {1, 3, 48, 320}, patched));
    ASSERT_FALSE(patched.empty());

    // 符号字符串 "p2o.DynamicDimension" 必须被彻底剥离，替换为定长 varint 维度 [1, 3, 48, 320]
    const std::string patchedRaw(reinterpret_cast<const char*>(patched.data()), patched.size());
    EXPECT_EQ(patchedRaw.find("p2o.DynamicDimension"), std::string::npos);
    // 验证 [48] -> 0x08 0x30 与 [320] -> 0x08 0xC0 0x02 均已写入新张量形状
    const std::string dim48("\x08\x30", 2);
    const std::string dim320("\x08\xC0\x02", 3);
    EXPECT_NE(patchedRaw.find(dim48), std::string::npos);
    EXPECT_NE(patchedRaw.find(dim320), std::string::npos);

    // 若当前机器已安装完整 PP-OCRv4 模型，执行真实 OpenCV DNN 固定分桶前向推理烟雾断言
    if (DeepOcrEngine::instance().isModelInstalled()) {
        cv::Mat synth(96, 520, CV_8UC3, cv::Scalar(252, 253, 255));
        cv::putText(synth, "Win32 PinyinEngine 8.1", cv::Point(24, 62),
                    cv::FONT_HERSHEY_SIMPLEX, 1.05, cv::Scalar(28, 32, 38), 2, cv::LINE_AA);
        const auto res = DeepOcrEngine::instance().extractText(synth);
        ASSERT_FALSE(res.empty());
        std::string joined;
        for (const auto& r : res) joined += r.text + " ";
        EXPECT_NE(joined.find("Win32"), std::string::npos);
        EXPECT_NE(joined.find("8.1"), std::string::npos);
    }
}

TEST(OcrTableReconstructorTest, FiltersSeparatorNoiseRowsAndCoalescesZeroCooccurrenceOutlierColumn) {
    using namespace tools3000::ocr;

    auto makeTok = [](const char* text, int x, int y, int w, int h = 18) {
        OcrResult r;
        r.text = text;
        r.confidence = 0.96f;
        r.box = cv::Rect(x, y, w, h);
        return r;
    };

    // 复现 12 行表格场景：
    // - 第 8 行与底行混入水平分割线被误检出的孤立标点噪点 "," 与 "."
    // - 第 6 行词尾带伪标点 "Windows.Media.Ocr & Pdf ,"
    // - 其余 11 行中间胶囊位于 x=290，唯独第 6 行 "100% Offline" 胶囊偏移至 x=336（与 x=290 列零同行共现）
    std::vector<OcrResult> blocks = {
        makeTok("C++20", 40, 30, 58), makeTok("MSVC v145", 290, 30, 96), makeTok("原生微内核主程序", 640, 30, 260),
        makeTok("Rust Kernel", 40, 68, 92), makeTok("Edition 2021", 290, 68, 102), makeTok("高速文档正文检索内核", 620, 68, 280),
        makeTok("Direct2D", 40, 106, 84), makeTok("GPU Hardware", 290, 106, 108), makeTok("显卡硬件加速绘制", 660, 106, 240),
        makeTok("Win32 LL Hooks", 40, 144, 120), makeTok("Native API", 290, 144, 88), makeTok("全局热键与手势监听", 650, 144, 250),
        makeTok("PinyinEngine", 40, 182, 106), makeTok("Built-in", 290, 182, 72), makeTok("中文全拼与首字母查找", 630, 182, 270),
        makeTok(",", 292, 203, 6, 6), // 分割线孤立噪点行，必须被彻底剔除
        makeTok("Windows.Media.Ocr & Pdf ,", 40, 224, 198),
        makeTok("100% Offline", 336, 224, 98), // 零同行共现的单行轻微右偏胶囊，必须归并至第 2 列而非新建空列 C
        makeTok("纯本地离线文字识别与 PDF 渲染", 610, 224, 290),
        makeTok("UIAutomation", 40, 262, 112), makeTok("System API", 290, 262, 88), makeTok("文件对话框目录跳转", 650, 262, 250),
        makeTok("WebView2", 40, 300, 82), makeTok("Evergreen", 290, 300, 84), makeTok("设置中心与搜索面板容器", 630, 300, 270),
        makeTok("Vite", 40, 338, 42), makeTok("8.0", 290, 338, 36), makeTok("前端按需分包构建引擎", 640, 338, 260),
        makeTok("Lucide Icons", 40, 376, 98), makeTok("1.21", 290, 376, 40), makeTok("高清矢量图标体系", 668, 376, 232),
        makeTok(".", 42, 402, 5, 5), makeTok(".", 292, 402, 5, 5), // 底部边框孤立标点噪点行
    };

    const OcrTableGrid grid = OcrTableReconstructor::reconstruct(blocks);
    ASSERT_TRUE(grid.isTable);
    EXPECT_EQ(grid.colCount, 3u); // 严禁出现第 4 幽灵列（C 空列）
    EXPECT_EQ(grid.rowCount, 10u); // 2 行纯标点分割线噪点已彻底滤除

    ASSERT_EQ(grid.cells[5].size(), 3u);
    EXPECT_EQ(grid.cells[5][0], "Windows.Media.Ocr & Pdf"); // 尾部伪标点 ',' 已剥离
    EXPECT_EQ(grid.cells[5][1], "100% Offline");            // 精准落入第 2 列 B
    EXPECT_EQ(grid.cells[5][2], "纯本地离线文字识别与 PDF 渲染");
}
