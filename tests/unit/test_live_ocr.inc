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
#include "capture/TextAnnotationHelper.h"
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
        bool clipReady = false;
        for (int retry = 0; retry < 20; ++retry) {
            if (OpenClipboard(nullptr)) {
                SetLastError(0);
                if (GetClipboardData(CF_UNICODETEXT) == nullptr && GetLastError() == ERROR_CLIPBOARD_NOT_OPEN) {
                    CloseClipboard();
                    Sleep(10);
                    continue;
                }
                clipReady = true;
                break;
            }
            Sleep(8);
        }
        if (clipReady) {
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

TEST(LiveOcrLayoutAndRowAlignmentTest, DenseFormLayoutIsolatesSidePaneCanyonsAndMisalignedRows) {
    using namespace tools3000::ocr;

    // 合成 1880x920 企业级密集分栏表单界面（含 x=1500 物理竖向窗格分割线）
    cv::Mat formCanvas(920, 1880, CV_8UC3, cv::Scalar(246, 247, 249));
    cv::line(formCanvas, cv::Point(1500, 36), cv::Point(1500, 896), cv::Scalar(162, 168, 176), 2);

    auto makeRaw = [](const char* text, int x, int y, int w, int h = 18) {
        OcrResult r;
        r.text = text;
        r.confidence = 0.99f;
        r.box = cv::Rect(x, y, w, h);
        r.charBoxes = OcrEngine::buildProportionalCharBoxes(text, r.box);
        return r;
    };

    std::vector<OcrResult> rawBlocks = {
        // 1. 主窗格标准对齐表格行（含大跨度空缺列，各列严格同水平基线 y=310 / y=332 / y=418）
        makeRaw("1", 24, 310, 14),
        makeRaw("JX0200-00680", 46, 310, 112),
        makeRaw("SY-GC-243-13150 高压真空断路器", 176, 310, 290),
        makeRaw("7", 778, 310, 14),
        makeRaw("0.0000", 916, 310, 44),
        makeRaw("J2", 968, 310, 18),
        makeRaw("手动", 1256, 310, 34),

        makeRaw("2", 24, 332, 14),
        makeRaw("WXF0300-00193", 46, 332, 118),
        makeRaw("PJG550系列气箱部分 10kV", 176, 332, 260),
        makeRaw("7", 778, 332, 14),
        makeRaw("0.0000", 916, 332, 44),
        makeRaw("J2", 968, 332, 18),
        makeRaw("手动", 1256, 332, 34),

        makeRaw("6", 24, 418, 14),
        makeRaw("WXF0300-00197", 46, 418, 118),
        makeRaw("PJG550系列气箱部分(母联) 10kV", 176, 418, 310),
        makeRaw("3", 778, 418, 14),
        makeRaw("0.0000", 916, 418, 44),
        makeRaw("J2", 968, 418, 18),
        makeRaw("手动", 1256, 418, 34),

        // 2. x=1500 分割线右侧的自定义字段侧边栏（"合同考核分类" y=419 与主表第 6 行仅差 1px，但隔着竖分割线）
        makeRaw("所属集团客户代码", 1545, 374, 118),
        makeRaw("所属集团客户名称", 1545, 396, 118),
        makeRaw("合同考核分类", 1545, 419, 92),
        makeRaw("是否正式合同", 1545, 441, 92),
        makeRaw("星云答复交货日期", 1545, 774, 122),
        makeRaw("第1次CDD原因", 1545, 796, 98),
        makeRaw("第2次CDD原因", 1545, 818, 98),
        makeRaw("第3次CDD原因", 1545, 840, 98),

        // 3. 左下角「备注」多行表单区（左侧标签 "备注" y=810 垂直居中夹在右侧两行输入框 y=803 / y=820 之间）
        makeRaw("备注", 18, 810, 36),
        makeRaw("基于 采购申请 12700. 基于", 176, 803, 180),
        makeRaw("采购订单 38604.", 176, 820, 132),

        // 4. 隔着 780px 半屏空白鸿沟且高低错位 8px 的右下角金额汇总区
        makeRaw("到期付款总计", 1136, 795, 92),
        makeRaw("62,376.00 RMB", 1366, 795, 104),
        makeRaw("对账的金额", 1136, 811, 80),
        makeRaw("到期余额", 1136, 828, 68),
        makeRaw("62,376.00 RMB", 1366, 828, 104),
    };

    const auto assembled = OcrEngine::assembleRecognizedBlocksWithLayout(rawBlocks, formCanvas);
    const auto atomic = OcrEngine::splitIntoAtomicBlocks(assembled);
    ASSERT_FALSE(atomic.empty());

    auto findBlock = [&](const std::string& exactText, int minY = 0) -> const OcrResult* {
        for (const auto& b : atomic) {
            if (b.text == exactText && b.box.y >= minY) return &b;
        }
        return nullptr;
    };

    const OcrResult* row6Code = findBlock("WXF0300-00197");
    const OcrResult* row6Manual = findBlock("手动", 400);
    const OcrResult* sideContract = findBlock("合同考核分类");
    const OcrResult* remarkLine1 = findBlock("基于 采购申请 12700. 基于");
    const OcrResult* remarkLine2 = findBlock("采购订单 38604.");
    const OcrResult* rightReconAmt = findBlock("对账的金额");
    const OcrResult* rightBalanceKey = findBlock("到期余额");
    const OcrResult* rightBalanceVal = findBlock("62,376.00 RMB", 820);
    const OcrResult* sideCdd2 = findBlock("第2次CDD原因");

    ASSERT_NE(row6Code, nullptr);
    ASSERT_NE(row6Manual, nullptr);
    ASSERT_NE(sideContract, nullptr);
    ASSERT_NE(remarkLine1, nullptr);
    ASSERT_NE(remarkLine2, nullptr);
    ASSERT_NE(rightReconAmt, nullptr);
    ASSERT_NE(rightBalanceKey, nullptr);
    ASSERT_NE(rightBalanceVal, nullptr);
    ASSERT_NE(sideCdd2, nullptr);

    // (A) 主表第 6 行内部真正水平对齐的跨列单元格共享同一 layoutRegionId 与 alignedRowId（支持整行高亮）
    EXPECT_EQ(row6Code->layoutRegionId, row6Manual->layoutRegionId);
    EXPECT_GE(row6Code->alignedRowId, 0);
    EXPECT_EQ(row6Code->alignedRowId, row6Manual->alignedRowId);

    // (B) 右侧自定义字段侧边栏被竖向分割线物理隔离，绝不与主表第 6 行串成一行
    EXPECT_NE(sideContract->layoutRegionId, row6Code->layoutRegionId);
    EXPECT_NE(sideContract->alignedRowId, row6Code->alignedRowId);

    // (C) 左下备注、右下错位金额、右侧侧边栏三者分属独立区域与独立行，彻底杜绝横跨全屏拼接
    EXPECT_EQ(remarkLine1->layoutRegionId, remarkLine2->layoutRegionId);
    EXPECT_NE(remarkLine1->layoutRegionId, rightReconAmt->layoutRegionId);
    EXPECT_NE(remarkLine1->layoutRegionId, sideCdd2->layoutRegionId);
    EXPECT_NE(rightReconAmt->layoutRegionId, sideCdd2->layoutRegionId);
    EXPECT_NE(remarkLine1->alignedRowId, rightReconAmt->alignedRowId);
    EXPECT_NE(remarkLine2->alignedRowId, rightReconAmt->alignedRowId);

    // (D) 右下汇总区内部真正水平对齐的 Key-Value（"到期余额" 与 "62,376.00 RMB"）保持同行对齐关系
    EXPECT_EQ(rightBalanceKey->layoutRegionId, rightBalanceVal->layoutRegionId);
    EXPECT_EQ(rightBalanceKey->alignedRowId, rightBalanceVal->alignedRowId);

    // (E) 零空白幽灵框校验：在下方表单区 (y >= 780) 绝无任何块跨越中部 x=[420..1080] 空白鸿沟
    for (const auto& b : atomic) {
        if (b.box.y >= 780) {
            EXPECT_FALSE(b.box.x < 420 && (b.box.x + b.box.width) > 1080)
                << "Unexpected canyon-spanning ghost box for text: " << b.text;
        }
    }

    // (F) PinWindow 真窗口端到端流式拖选验证：拖选左下角备注两行，绝不误吸左侧孤立「备注」标签或右下角「对账的金额」
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
                         (L"tools3000_form_pin_ocr_" + std::to_wstring(GetCurrentProcessId()) + L".png");
    struct FormCleanupGuard {
        HMODULE mod = nullptr;
        tools3000::core::IPlugin* plugin = nullptr;
        std::filesystem::path png;
        ~FormCleanupGuard() {
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

    std::vector<uchar> encoded;
    ASSERT_TRUE(cv::imencode(".png", formCanvas, encoded));
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

    ASSERT_TRUE(invokeRpc(3201, "capture.pinImageFile", {
        {"path", tools3000::core::WinUtils::wstringToUtf8(tempPng.wstring())}
    }).value("success", false));

    HWND pinHwnd = FindWindowW(L"Tools3000_PinWindow", nullptr);
    ASSERT_NE(pinHwnd, nullptr);

    nlohmann::json mockJson = nlohmann::json::array();
    for (const auto& rb : rawBlocks) {
        mockJson.push_back({
            {"text", rb.text},
            {"x", rb.box.x},
            {"y", rb.box.y},
            {"w", rb.box.width},
            {"h", rb.box.height}
        });
    }
    ASSERT_TRUE(invokeRpc(3202, "capture.inspectPinOcrState", {{"mockBlocks", mockJson}}).value("success", false));

    RECT clientRc{};
    ASSERT_TRUE(GetClientRect(pinHwnd, &clientRc));
    const double scaleX = static_cast<double>(clientRc.right - clientRc.left) / static_cast<double>(formCanvas.cols);
    const double scaleY = static_cast<double>(clientRc.bottom - clientRc.top) / static_cast<double>(formCanvas.rows);

    const int startCx = static_cast<int>(std::lround(178.0 * scaleX));
    const int startCy = static_cast<int>(std::lround(812.0 * scaleY));
    const int endCx = static_cast<int>(std::lround(306.0 * scaleX));
    const int endCy = static_cast<int>(std::lround(829.0 * scaleY));

    SendMessageW(pinHwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(startCx, startCy));
    SendMessageW(pinHwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(endCx, endCy));
    SendMessageW(pinHwnd, WM_LBUTTONUP, 0, MAKELPARAM(endCx, endCy));

    const auto dragRes = invokeRpc(3203, "capture.inspectPinOcrState", nlohmann::json::object());
    EXPECT_EQ(dragRes.value("selectedText", ""), "基于 采购申请 12700. 基于\r\n采购订单 38604.");

    SendMessageW(pinHwnd, WM_KEYDOWN, VK_DELETE, 0);
    capturePlugin->shutdown();
    guard.plugin = nullptr;
    EXPECT_TRUE(FreeLibrary(hMod));
    guard.mod = nullptr;
}

TEST(LiveTextOcrTest, AxisAlignedZeroMatrixCharProjectionPreservesVariableWidthSpans) {
    using namespace tools3000::ocr;

    const std::string text = "AI极速";
    std::vector<CtcDecodedChar> decoded = {
        {"A",  0.99f, 0, 1, 2, 0.00f, 0.12f},
        {"I",  0.99f, 3, 3, 4, 0.12f, 0.22f},
        {"极", 0.98f, 6, 8, 10, 0.25f, 0.62f},
        {"速", 0.98f, 11, 13, 15, 0.62f, 1.00f},
    };
    const cv::Rect lineBox(100, 50, 200, 28);
    const cv::Mat emptyInvPersp;

    const auto boxes = DeepOcrEngine::projectCharSpansToImage(
        text, decoded, 200, 28, false, false, emptyInvPersp, lineBox, 800, 600);
    ASSERT_EQ(boxes.size(), 4u);
    EXPECT_NEAR(boxes[0].x, 100, 1);
    EXPECT_NEAR(boxes[0].width, 24, 1);
    EXPECT_NEAR(boxes[1].x, 124, 1);
    EXPECT_NEAR(boxes[1].width, 20, 1);
    EXPECT_NEAR(boxes[2].x, 150, 1);
    EXPECT_NEAR(boxes[2].width, 74, 1);
    EXPECT_NEAR(boxes[3].x, 224, 1);
    EXPECT_NEAR(boxes[3].x + boxes[3].width, 300, 1);
    for (const auto& rc : boxes) {
        EXPECT_EQ(rc.y, 50);
        EXPECT_EQ(rc.height, 28);
    }
}

TEST(LiveTextOcrTest, OcrEngineImageFingerprintCacheHitAndPixelInvalidation) {
    using namespace tools3000::ocr;
    OcrEngine::instance().initialize();

    cv::Mat imgA(160, 520, CV_8UC3, cv::Scalar(255, 255, 255));
    cv::putText(imgA, "CACHE-HIT-2026", cv::Point(28, 96),
                cv::FONT_HERSHEY_SIMPLEX, 1.35, cv::Scalar(16, 16, 16), 3, cv::LINE_AA);

    const auto firstPage = OcrEngine::instance().extractPage(imgA);
    ASSERT_FALSE(firstPage.lines.empty());

    const auto t0 = std::chrono::steady_clock::now();
    const auto cachedPage = OcrEngine::instance().extractPage(imgA);
    const auto t1 = std::chrono::steady_clock::now();
    const double cachedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    ASSERT_EQ(cachedPage.lines.size(), firstPage.lines.size());
    EXPECT_EQ(cachedPage.lines.front().text, firstPage.lines.front().text);
    EXPECT_LT(cachedMs, 35.0);

    cv::Mat imgB(160, 520, CV_8UC3, cv::Scalar(255, 255, 255));
    cv::putText(imgB, "DELTA-NEW-9080", cv::Point(28, 96),
                cv::FONT_HERSHEY_SIMPLEX, 1.35, cv::Scalar(16, 16, 16), 3, cv::LINE_AA);
    const auto secondPage = OcrEngine::instance().extractPage(imgB);
    ASSERT_FALSE(secondPage.lines.empty());
    EXPECT_NE(secondPage.lines.front().text, firstPage.lines.front().text);
}

TEST(LiveTextOcrTest, PinZeroDuplicateOcrReuseAndSideDockingViaPlugin) {
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
                         (L"tools3000_ocr_reuse_dock_" + std::to_wstring(GetCurrentProcessId()) + L".png");

    auto& bridge = tools3000::core::MessageBridge::instance();
    auto invokeRpc = [&](int id, const char* method, const nlohmann::json& params) -> nlohmann::json {
        nlohmann::json req = {{"id", id}, {"method", method}, {"params", params}};
        const std::string raw = bridge.handleMessage(req.dump());
        const auto parsed = nlohmann::json::parse(raw, nullptr, false);
        return parsed.is_object() && parsed.contains("result") ? parsed["result"] : parsed;
    };

    struct PluginCleanupGuard {
        HMODULE mod = nullptr;
        tools3000::core::IPlugin* plugin = nullptr;
        std::filesystem::path png;
        ~PluginCleanupGuard() {
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

    // 1. 验证 OcrResultWindow 锚点侧翼避让停靠（右侧空间充足时吸附在锚点右侧，绝不居中遮挡原图）
    HMONITOR mon = MonitorFromPoint(POINT{100, 100}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    ASSERT_TRUE(GetMonitorInfoW(mon, &mi));
    if ((mi.rcWork.right - mi.rcWork.left) >= 960) {
        const int anchorLeft = mi.rcWork.left + 60;
        const int anchorRight = anchorLeft + 260;
        const auto dockRes = invokeRpc(3301, "ocr.openResultWindow", {
            {"text", "对照首行文本\n对照次行文本"},
            {"anchorRect", {
                {"left", anchorLeft},
                {"top", static_cast<int>(mi.rcWork.top + 80)},
                {"right", anchorRight},
                {"bottom", static_cast<int>(mi.rcWork.top + 320)}
            }}
        });
        ASSERT_TRUE(dockRes.value("success", false));
        EXPECT_GE(dockRes.value("windowLeft", 0), anchorRight);
        invokeRpc(3302, "ocr.closeResultWindow", nlohmann::json::object());
    }

    // 2. 验证 PinWindow 已就绪 OCR 数据调用 extractText() 时 0ms 零二次推理复用与列对齐文本回填
    cv::Mat canvas(180, 360, CV_8UC3, cv::Scalar(250, 252, 255));
    std::vector<uchar> encoded;
    ASSERT_TRUE(cv::imencode(".png", canvas, encoded));
    {
        std::ofstream ofs(tempPng, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    }

    ASSERT_TRUE(invokeRpc(3303, "capture.pinImageFile", {
        {"path", tools3000::core::WinUtils::wstringToUtf8(tempPng.wstring())}
    }).value("success", false));

    nlohmann::json mockBlocks = nlohmann::json::array({
        {{"text", "项目编码"}, {"x", 20},  {"y", 30}, {"w", 88}, {"h", 24}, {"layoutRegionId", 0}, {"alignedRowId", 0}},
        {{"text", "A-1024"},   {"x", 180}, {"y", 30}, {"w", 76}, {"h", 24}, {"layoutRegionId", 0}, {"alignedRowId", 0}},
        {{"text", "状态正常"}, {"x", 20},  {"y", 72}, {"w", 88}, {"h", 24}, {"layoutRegionId", 0}, {"alignedRowId", 1}}
    });

    const auto reuseRes = invokeRpc(3304, "capture.inspectPinOcrState", {
        {"mockBlocks", mockBlocks},
        {"extractText", true},
        {"openComparisonPanel", true}
    });
    ASSERT_TRUE(reuseRes.value("success", false));
    EXPECT_TRUE(reuseRes.value("ocrReady", false));
    EXPECT_EQ(reuseRes.value("ocrRunCount", 99u), 0u);
    std::string panelText = reuseRes.value("ocrPanelText", "");
    panelText.erase(std::remove(panelText.begin(), panelText.end(), '\r'), panelText.end());
    EXPECT_EQ(panelText, "项目编码\tA-1024\n状态正常");

    invokeRpc(3305, "ocr.closeResultWindow", nlohmann::json::object());
    while (HWND existing = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
        SendMessageW(existing, WM_KEYDOWN, VK_DELETE, 0);
        if (IsWindow(existing)) DestroyWindow(existing);
    }
    capturePlugin->shutdown();
    guard.plugin = nullptr;
    EXPECT_TRUE(FreeLibrary(hMod));
    guard.mod = nullptr;
}

TEST(LiveTextOcrTest, TextAnnotationCoalescesSameRowAndPreservesColumnGap) {
    using namespace tools3000::capture;

    struct MockBlk {
        std::string text;
        cv::Rect origBox;
        std::vector<cv::Rect> charBoxes;
        int layoutRegionId = 0;
        int alignedRowId = -1;
    };

    auto makeBlk = [](const std::string& txt, int x, int y, int w, int h, int reg, int row) {
        MockBlk b;
        b.text = txt;
        b.origBox = cv::Rect(x, y, w, h);
        b.charBoxes = tools3000::ocr::OcrEngine::buildProportionalCharBoxes(txt, b.origBox);
        b.layoutRegionId = reg;
        b.alignedRowId = row;
        return b;
    };

    // 同行内的普通文本 + 代码胶囊 `960x640` 紧邻（间隔 8px），应合并为单条连贯行跨度；
    // 右侧另一分栏（间隔 160px）或下一行则保持独立跨度
    std::vector<MockBlk> blocks = {
        makeBlk("固定形状多桶：", 24, 30, 120, 22, 0, 0),
        makeBlk("960x640",       152, 30,  78, 22, 0, 0),
        makeBlk("右侧独立分栏",   420, 30, 110, 22, 1, 0),
        makeBlk("第二行正文内容",  24, 62, 140, 22, 0, 1),
    };

    std::vector<size_t> selIdx = {0, 1, 2, 3};
    std::vector<SelectedBlockCharSpan> emptyCharSpans;
    const auto coalesced = TextAnnotationHelper::coalesceSelectedTextSpans(blocks, selIdx, emptyCharSpans);
    ASSERT_EQ(coalesced.size(), 3u);
    EXPECT_EQ(coalesced[0].x, 24);
    EXPECT_EQ(coalesced[0].width, (152 + 78) - 24); // 第 0 块与第 1 块无缝桥接
    EXPECT_EQ(coalesced[1].x, 420);                 // 跨栏块保持物理隔离
    EXPECT_EQ(coalesced[2].y, 62);                  // 第二行独立跨度
}

TEST(LiveTextOcrTest, TextMarkupHighlightPreservesCrispDarkTextAndSupportsWavyAndUnderline) {
    using namespace tools3000::capture;

    initializeMarkupTextRenderer();

    // 构造白底黑字 + 灰底代码胶囊模拟图
    cv::Mat base(180, 480, CV_8UC3, cv::Scalar(255, 255, 255));
    cv::rectangle(base, cv::Rect(210, 26, 110, 26), cv::Scalar(240, 242, 245), cv::FILLED);
    // 写入浓黑笔画像素 (BGR = 18, 18, 18)
    cv::rectangle(base, cv::Rect(50, 32, 12, 14), cv::Scalar(18, 18, 18), cv::FILLED);

    MarkupEngine engine;
    engine.setBaseImage(base);

    const std::vector<cv::Rect> twoLineSpans = {
        cv::Rect(36, 28, 300, 22),
        cv::Rect(36, 52, 260, 22) // 与上一行紧邻，验证单通道联合圆角蒙版无双倍重影横线
    };
    const MarkupColor coralPink{244, 63, 94, 255};
    ASSERT_NE(engine.drawTextHighlightSpans(twoLineSpans, coralPink), nullptr);
    ASSERT_NE(engine.drawTextUnderlineSpans({cv::Rect(36, 100, 240, 22)}, coralPink, LineStyle::Wavy), nullptr);
    ASSERT_NE(engine.drawTextUnderlineSpans({cv::Rect(36, 138, 240, 22)}, MarkupColor::Blue(), LineStyle::Solid), nullptr);

    const cv::Mat comp = engine.getCompositeImage(false);
    ASSERT_FALSE(comp.empty());

    // 1) 白底位置应呈现清透粉红（R 高、G/B 适中下降，绝非不透明死红）
    const cv::Vec3b whiteHighlighted = comp.at<cv::Vec3b>(38, 100);
    EXPECT_GT(whiteHighlighted[2], 235);
    EXPECT_LT(whiteHighlighted[1], 200);
    EXPECT_GT(whiteHighlighted[1], 130);

    // 2) 黑字笔画中心必须依然保持浓黑锐利（正片叠底不泛白雾）
    const cv::Vec3b darkGlyph = comp.at<cv::Vec3b>(38, 55);
    EXPECT_LT(darkGlyph[0], 32);
    EXPECT_LT(darkGlyph[1], 32);
    EXPECT_LT(darkGlyph[2], 32);

    // 3) 灰底代码胶囊应呈现比白底略深的自然层次
    const cv::Vec3b codePillPx = comp.at<cv::Vec3b>(38, 240);
    EXPECT_LT(codePillPx[1], whiteHighlighted[1]);

    // 4) 手绘波浪线与手绘直线区域下方应真实渲染出醒目彩色下划线像素
    bool wavyRendered = false;
    for (int y = 120; y <= 132 && !wavyRendered; ++y) {
        for (int x = 50; x <= 200; ++x) {
            const cv::Vec3b p = comp.at<cv::Vec3b>(y, x);
            if (p[2] > 200 && p[1] < 160) {
                wavyRendered = true;
                break;
            }
        }
    }
    EXPECT_TRUE(wavyRendered);

    bool straightRendered = false;
    for (int y = 158; y <= 168 && !straightRendered; ++y) {
        const cv::Vec3b p = comp.at<cv::Vec3b>(y, 120);
        if (p[0] > 200 && p[2] < 140) {
            straightRendered = true;
            break;
        }
    }
    EXPECT_TRUE(straightRendered);

    // 5) 单次 undo 原子撤销整条图元
    EXPECT_EQ(engine.elementCount(), 3u);
    EXPECT_TRUE(engine.undo());
    EXPECT_EQ(engine.elementCount(), 2u);
}

TEST(LiveTextOcrTest, PinInPlaceTextAnnotationSupportsStackingColorSwitchAndUndoZeroOcrRerun) {
    wchar_t exePathBuf[MAX_PATH]{};
    ASSERT_GT(GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH), 0u);
    const std::filesystem::path exeDir = std::filesystem::path(exePathBuf).parent_path();
    const std::filesystem::path pluginDll =
        exeDir.parent_path() / L"plugins" / exeDir.filename() / L"Plugin_Capture.dll";
    ASSERT_TRUE(std::filesystem::exists(pluginDll));

    HMODULE hMod = LoadLibraryExW(
        pluginDll.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    ASSERT_NE(hMod, nullptr);
    using CreateFn = tools3000::core::IPlugin* (*)();
    auto createFn = reinterpret_cast<CreateFn>(GetProcAddress(hMod, "CreatePlugin"));
    ASSERT_NE(createFn, nullptr);

    tools3000::core::IPlugin* capturePlugin = createFn();
    ASSERT_NE(capturePlugin, nullptr);
    ASSERT_TRUE(capturePlugin->initialize());

    auto& bridge = tools3000::core::MessageBridge::instance();
    const std::filesystem::path tempPng = std::filesystem::temp_directory_path() /
        std::format(L"tools3000_pin_text_annot_{}.png", GetCurrentProcessId());

    auto invokeRpc = [&](int id, const char* method, const nlohmann::json& params) -> nlohmann::json {
        nlohmann::json req = {{"id", id}, {"method", method}, {"params", params}};
        const std::string raw = bridge.handleMessage(req.dump());
        const auto parsed = nlohmann::json::parse(raw, nullptr, false);
        return parsed.is_object() && parsed.contains("result") ? parsed["result"] : parsed;
    };

    struct PluginCleanupGuard {
        HMODULE mod = nullptr;
        tools3000::core::IPlugin* plugin = nullptr;
        std::filesystem::path png;
        ~PluginCleanupGuard() {
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

    cv::Mat canvas(220, 520, CV_8UC3, cv::Scalar(255, 255, 255));
    std::vector<uchar> encoded;
    ASSERT_TRUE(cv::imencode(".png", canvas, encoded));
    {
        std::ofstream ofs(tempPng, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    }

    ASSERT_TRUE(invokeRpc(3401, "capture.pinImageFile", {
        {"path", tools3000::core::WinUtils::wstringToUtf8(tempPng.wstring())}
    }).value("success", false));

    nlohmann::json mockBlocks = nlohmann::json::array({
        {{"text", "高精度几何装箱与抗水印干扰"}, {"x", 30}, {"y", 40}, {"w", 320}, {"h", 26}, {"layoutRegionId", 0}, {"alignedRowId", 0}},
        {{"text", "零拷贝切片与降序最佳装箱"},   {"x", 30}, {"y", 82}, {"w", 290}, {"h", 24}, {"layoutRegionId", 0}, {"alignedRowId", 1}}
    });

    // 1. 全选文字，检查悬浮舱荧光笔/波浪线/直线按钮均可见
    const auto selRes = invokeRpc(3402, "capture.inspectPinOcrState", {
        {"mockBlocks", mockBlocks},
        {"selectAll", true},
        {"forceRender", true}
    });
    ASSERT_TRUE(selRes.value("success", false));
    EXPECT_TRUE(selRes.value("highlightBtnVisible", false));
    EXPECT_TRUE(selRes.value("wavyBtnVisible", false));
    EXPECT_TRUE(selRes.value("underlineBtnVisible", false));

    // 2. 施加珊瑚粉荧光笔 + 同选区叠加手绘波浪线，验证原图像素变粉且 ocrRunCount == 0（零重跑 OCR）
    const auto hlRes = invokeRpc(3403, "capture.inspectPinOcrState", {
        {"annotateMode", "highlight"},
        {"sampleX", 100},
        {"sampleY", 50}
    });
    ASSERT_TRUE(hlRes.value("success", false));
    EXPECT_EQ(hlRes.value("ocrRunCount", 99u), 0u);
    EXPECT_GT(hlRes.value("sampleR", 0), 235);
    EXPECT_LT(hlRes.value("sampleG", 255), 205);

    const auto wavyRes = invokeRpc(3404, "capture.inspectPinOcrState", {
        {"annotateMode", "wavy"},
        {"forceRender", true}
    });
    ASSERT_TRUE(wavyRes.value("success", false));
    EXPECT_TRUE(wavyRes.value("canUndoPinMarkup", false));
    EXPECT_TRUE(wavyRes.value("undoBtnVisible", false));

    // 3. 切换为第 3 号色球（晴空宝蓝 #3B82F6），验证已标荧光笔实时切换为蓝色调
    const auto blueRes = invokeRpc(3405, "capture.inspectPinOcrState", {
        {"setMarkupColorIndex", 3},
        {"sampleX", 100},
        {"sampleY", 50}
    });
    ASSERT_TRUE(blueRes.value("success", false));
    EXPECT_GT(blueRes.value("sampleB", 0), blueRes.value("sampleR", 255));
    EXPECT_EQ(blueRes.value("ocrRunCount", 99u), 0u);

    // 4. 连续三次撤销退回纯白原图，再重做恢复
    invokeRpc(3406, "capture.inspectPinOcrState", {{"undoMarkup", true}});
    invokeRpc(3407, "capture.inspectPinOcrState", {{"undoMarkup", true}});
    const auto cleanRes = invokeRpc(3408, "capture.inspectPinOcrState", {
        {"undoMarkup", true},
        {"sampleX", 100},
        {"sampleY", 50}
    });
    EXPECT_EQ(cleanRes.value("sampleR", 0), 255);
    EXPECT_EQ(cleanRes.value("sampleG", 0), 255);
    EXPECT_EQ(cleanRes.value("sampleB", 0), 255);

    const auto redoRes = invokeRpc(3409, "capture.inspectPinOcrState", {
        {"redoMarkup", true},
        {"sampleX", 100},
        {"sampleY", 50}
    });
    EXPECT_LT(redoRes.value("sampleG", 255), 220);

    while (HWND existing = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
        SendMessageW(existing, WM_KEYDOWN, VK_DELETE, 0);
        if (IsWindow(existing)) DestroyWindow(existing);
    }
    capturePlugin->shutdown();
    guard.plugin = nullptr;
    EXPECT_TRUE(FreeLibrary(hMod));
    guard.mod = nullptr;
}

TEST(LiveTextOcrTest, HandDrawnWavyAndStraightStrokesAreOrganicBoldAndDeterministic) {
    using namespace tools3000::capture;

    // 1. 验证高定斜角书法丝带波浪线参数：宽舒大波长 (>= 30px)、克制优雅振幅 [1.8, 3.2] 与黄金呼吸间距绝不压字
    float w = 0.0f, amp = 0.0f, halfW = 0.0f;
    HandDrawnStrokeGenerator::resolveTextWavyMetrics(24.0f, 0.0f, w, amp, halfW);
    EXPECT_GE(w, 2.8f);
    EXPECT_GE(amp, 1.8f);
    EXPECT_LE(amp, 3.2f);
    EXPECT_GE(halfW * 2.0f, 30.0f);
    const float baseY = HandDrawnStrokeGenerator::resolveTextWavyBaselineY(120.0f, 24.0f, amp);
    EXPECT_GT(baseY - amp, 121.5f); // 波峰最高点严格位于文字底边下方留白区，100% 绝不切字

    // 2. 验证斜角书法笔锋动态宽窄韵律（下按饱满、上挑纤细）、确定性及收笔尖锋
    const auto wavy1 = HandDrawnStrokeGenerator::buildWavyStroke(40.0f, baseY, 320.0f, baseY, w, amp, halfW, true);
    const auto wavy2 = HandDrawnStrokeGenerator::buildWavyStroke(40.0f, baseY, 320.0f, baseY, w, amp, halfW, true);
    ASSERT_GT(wavy1.spine.size(), 40u);
    ASSERT_EQ(wavy1.outline.size(), wavy2.outline.size());
    EXPECT_FLOAT_EQ(wavy1.spine[wavy1.spine.size() / 2].y, wavy2.spine[wavy2.spine.size() / 2].y);
    float minMidHw = 999.0f, maxMidHw = 0.0f;
    for (size_t i = wavy1.spine.size() / 5; i < (wavy1.spine.size() * 4) / 5; ++i) {
        minMidHw = (std::min)(minMidHw, wavy1.spine[i].halfWidth);
        maxMidHw = (std::max)(maxMidHw, wavy1.spine[i].halfWidth);
    }
    EXPECT_GT(maxMidHw, minMidHw * 1.25f); // 斜角书法笔尖产生清晰的下粗上细丝带层次
    EXPECT_LT(wavy1.spine.back().halfWidth, maxMidHw * 0.65f);

    const float straightW = HandDrawnStrokeGenerator::resolveTextStraightWidth(24.0f, 0.0f);
    EXPECT_GE(straightW, 3.1f);
    const auto straight = HandDrawnStrokeGenerator::buildStraightStroke(40.0f, 160.0f, 280.0f, 160.0f, straightW, true);
    ASSERT_GT(straight.spine.size(), 20u);
    ASSERT_GT(straight.outline.size(), 40u);
    EXPECT_LT(straight.spine.back().halfWidth, straight.spine[straight.spine.size() / 2].halfWidth * 0.72f);
    float maxDev = 0.0f;
    for (const auto& s : straight.spine) {
        maxDev = (std::max)(maxDev, std::abs(s.y - 160.0f));
    }
    EXPECT_GT(maxDev, 0.45f);
    EXPECT_LT(maxDev, 3.8f);
}

TEST(OcrResultWindowCloseTest, TopCloseBottomCloseSingleEscWmCloseAndSecondRefreshAlphaSafe) {
    auto& win = tools3000::ocr::OcrResultWindow::instance();
    win.showResult("正在识别...");
    ASSERT_NE(win.hwnd(), nullptr);
    ASSERT_TRUE(IsWindowVisible(win.hwnd()));

    // 1. 二次刷新正文时 m_currentAlpha 必须满额 255.0f，绝不退化为 1.0f 透明幽灵窗
    win.showResult("第一行文字\n第二行文字\n第三行文字");
    EXPECT_TRUE(IsWindowVisible(win.hwnd()));
    EXPECT_GE(win.currentAlpha(), 254.0f);

    // 2. 右上角 × 按钮可见且点击即关闭
    const D2D1_RECT_F topCloseRc = win.topCloseButtonRect();
    ASSERT_GT(topCloseRc.right - topCloseRc.left, 16.0f);
    const int tx = static_cast<int>((topCloseRc.left + topCloseRc.right) * 0.5f);
    const int ty = static_cast<int>((topCloseRc.top + topCloseRc.bottom) * 0.5f);
    SendMessageW(win.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(tx, ty));
    SendMessageW(win.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(tx, ty));
    EXPECT_FALSE(IsWindowVisible(win.hwnd()));

    // 3. 重新打开后验证右下角关闭按钮点击即关闭
    win.showResult("第一行文字\n第二行文字");
    ASSERT_TRUE(IsWindowVisible(win.hwnd()));
    const D2D1_RECT_F botCloseRc = win.bottomCloseButtonRect();
    const int bx = static_cast<int>((botCloseRc.left + botCloseRc.right) * 0.5f);
    const int by = static_cast<int>((botCloseRc.top + botCloseRc.bottom) * 0.5f);
    SendMessageW(win.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(bx, by));
    SendMessageW(win.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(bx, by));
    EXPECT_FALSE(IsWindowVisible(win.hwnd()));

    // 4. 有选区时第 1 次 Esc 一步清空选区与光标，第 2 次 Esc 立即关闭（绝不需连按 3 次）
    win.showResult("Hello World\nSecond Line");
    ASSERT_TRUE(IsWindowVisible(win.hwnd()));
    win.selectAll();
    ASSERT_TRUE(win.hasSelection());
    SendMessageW(win.hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
    EXPECT_FALSE(win.hasSelection());
    EXPECT_TRUE(IsWindowVisible(win.hwnd()));
    SendMessageW(win.hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
    EXPECT_FALSE(IsWindowVisible(win.hwnd()));

    // 5. WM_CLOSE 安全隐藏且窗口句柄可再次复用不崩溃
    win.showResult("WM_CLOSE Reuse Test");
    ASSERT_TRUE(IsWindowVisible(win.hwnd()));
    SendMessageW(win.hwnd(), WM_CLOSE, 0, 0);
    EXPECT_FALSE(IsWindowVisible(win.hwnd()));
    EXPECT_TRUE(IsWindow(win.hwnd()));

    win.cleanup();
}
