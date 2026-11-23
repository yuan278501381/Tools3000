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

    // (D) 空白无字且远离悬浮舱与顶部悬停栏的底部空白区 (40, 235) 按住左键拖拽：清除文字选区并 100% 正常移动贴图窗口
    RECT beforeDragRc{};
    ASSERT_TRUE(GetWindowRect(pinHwnd, &beforeDragRc));
    SendMessageW(pinHwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 235));
    SendMessageW(pinHwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(78, 259));
    SendMessageW(pinHwnd, WM_LBUTTONUP, 0, MAKELPARAM(78, 259));

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

    // 2B. 已施加批注进入清透预览态后：点击悬浮舱首钮 [复制图]（附 5px 手抖模拟）必须将带标注图片写入剪贴板而非纯文本！
    {
        HWND pinHwnd = FindWindowW(L"Tools3000_PinWindow", nullptr);
        ASSERT_NE(pinHwnd, nullptr);
        const int cfx = wavyRes.value("copyFloatingCenterX", 0);
        const int cfy = wavyRes.value("copyFloatingCenterY", 0);
        ASSERT_GT(cfx, 0);
        ASSERT_GT(cfy, 0);
        SendMessageW(pinHwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cfx, cfy));
        SendMessageW(pinHwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(cfx + 5, cfy + 2));
        SendMessageW(pinHwnd, WM_LBUTTONUP, 0, MAKELPARAM(cfx + 5, cfy + 2));

        ASSERT_TRUE(OpenClipboard(nullptr));
        const bool hasImage = (IsClipboardFormatAvailable(CF_DIB) != FALSE) ||
                              (IsClipboardFormatAvailable(CF_BITMAP) != FALSE);
        const bool hasText = (IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE);
        CloseClipboard();
        EXPECT_TRUE(hasImage) << "Post-markup copy pill must copy annotated image to clipboard";
        EXPECT_FALSE(hasText) << "Post-markup copy pill must not overwrite clipboard with plain text";
    }

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

TEST(LiveTextOcrTest, CompactLineSpacingInkBehindTextPreservesLowerLineLegibility) {
    using namespace tools3000::capture;

    // 1. 验证自适应行距求解器在紧凑 2px 行距下精确锁死两行走廊中线并收束振幅与笔宽
    const auto tightWavy = HandDrawnStrokeGenerator::resolveAdaptiveWavyPlacement(
        100.0f, 14.0f, 2.5f, true, 0.0f);
    EXPECT_TRUE(tightWavy.compactCorridor);
    EXPECT_NEAR(tightWavy.baselineY, 101.25f, 0.1f);
    EXPECT_LT(tightWavy.amplitude, 1.3f);
    EXPECT_LT(tightWavy.strokeWidth, 2.0f);

    const auto looseWavy = HandDrawnStrokeGenerator::resolveAdaptiveWavyPlacement(
        100.0f, 18.0f, 16.0f, true, 0.0f);
    EXPECT_FALSE(looseWavy.compactCorridor);
    EXPECT_GT(looseWavy.amplitude, 1.8f);

    // 2. 暗色终端极限紧凑两行像素实测（行距仅 2px）：验证画波浪线后空白走廊呈明显红色且第 2 行白字笔画零被盖糊
    cv::Mat darkTerminal(80, 360, CV_8UC3, cv::Scalar(32, 30, 28));
    // 第 1 行白字模拟横笔区 y=[18..30]，第 2 行白字模拟横笔区 y=[33..45]（真实行间走廊仅 2px：y=31..32）
    cv::rectangle(darkTerminal, cv::Rect(24, 19, 280, 11), cv::Scalar(225, 228, 232), cv::FILLED);
    cv::rectangle(darkTerminal, cv::Rect(24, 33, 280, 11), cv::Scalar(225, 228, 232), cv::FILLED);

    cv::Mat annotated = darkTerminal.clone();
    MarkupEngine::renderTextUnderlinePreview(
        annotated,
        {cv::Rect(24, 18, 280, 13)},
        MarkupColor{239, 68, 68, 255},
        LineStyle::Wavy,
        0.0f);

    // 走廊中线 (y=31..32) 必须出现醒目红色波浪像素
    int redCorridorPixels = 0;
    for (int y = 30; y <= 32; ++y) {
        for (int x = 30; x < 300; ++x) {
            const cv::Vec3b px = annotated.at<cv::Vec3b>(y, x);
            if (static_cast<int>(px[2]) - static_cast<int>(px[0]) >= 55) {
                ++redCorridorPixels;
            }
        }
    }
    EXPECT_GT(redCorridorPixels, 40) << "Wavy stroke must remain vivid in the inter-line corridor";

    // 第 2 行核心白字区域（y=34..43）必须受 Ink-Behind-Text 保护，保持 >= 195 清晰亮白，绝不被红线盖住！
    int protectedWhitePixels = 0;
    int totalRow2Pixels = 0;
    for (int y = 34; y <= 43; ++y) {
        for (int x = 30; x < 300; ++x) {
            const cv::Vec3b px = annotated.at<cv::Vec3b>(y, x);
            const int lum = (static_cast<int>(px[2]) * 77 + static_cast<int>(px[1]) * 150 + static_cast<int>(px[0]) * 29) >> 8;
            ++totalRow2Pixels;
            if (lum >= 195) ++protectedWhitePixels;
        }
    }
    EXPECT_EQ(protectedWhitePixels, totalRow2Pixels)
        << "Second line bright text strokes must remain crisp and unobstructed via Ink-Behind-Text compositing";
}

TEST(LiveTextOcrTest, EnvRealImageOcrBenchmark) {
    wchar_t envBuf[1024]{};
    const DWORD len = GetEnvironmentVariableW(L"TOOLS3000_OCR_BENCH_IMAGE", envBuf, static_cast<DWORD>(std::size(envBuf)));
    if (len == 0 || len >= std::size(envBuf)) {
        GTEST_SKIP() << "Set TOOLS3000_OCR_BENCH_IMAGE to run real-image OCR latency benchmark";
    }
    const std::string pathUtf8 = tools3000::core::WinUtils::wstringToUtf8(std::wstring(envBuf, len));
    const cv::Mat img = cv::imread(pathUtf8, cv::IMREAD_COLOR);
    ASSERT_FALSE(img.empty()) << "Failed to load benchmark image: " << pathUtf8;

    auto& deep = tools3000::ocr::DeepOcrEngine::instance();
    ASSERT_TRUE(deep.getModelStatus().installed) << "DeepOcrEngine models not installed";

    wchar_t tierEnv[32]{};
    const DWORD tierLen = GetEnvironmentVariableW(L"TOOLS3000_OCR_BENCH_TIER", tierEnv, static_cast<DWORD>(std::size(tierEnv)));
    std::filesystem::path tmpCfgDir;
    if (tierLen > 0 && tierLen < std::size(tierEnv)) {
        const std::string tierStr = tools3000::core::WinUtils::wstringToUtf8(std::wstring(tierEnv, tierLen));
        tmpCfgDir = std::filesystem::temp_directory_path() / L"tools3000_ocr_bench_cfg";
        std::error_code cfgEc;
        std::filesystem::create_directories(tmpCfgDir, cfgEc);
        tools3000::core::ConfigManager::instance().initialize(tmpCfgDir);
        tools3000::core::ConfigManager::instance().mergePatch({{"ocr", {{"modelTier", tierStr}}}}, "/ocr");
        deep.releaseLoadedModels();
    }

    // 预热一次固定桶（模拟主程序启动后的常驻暖机状态），再测一次真实运行耗时
    const auto tCold0 = std::chrono::steady_clock::now();
    const auto warmRes = deep.extractText(img);
    const auto coldMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tCold0).count();

    const auto tWarm0 = std::chrono::steady_clock::now();
    const auto hotRes = deep.extractText(img);
    const auto hotMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tWarm0).count();

    size_t totalChars = 0;
    wchar_t dumpFlag[8]{};
    const bool doDump = GetEnvironmentVariableW(L"TOOLS3000_OCR_BENCH_DUMP", dumpFlag, 8) > 0 && dumpFlag[0] == L'1';
    for (size_t i = 0; i < hotRes.size(); ++i) {
        const auto& r = hotRes[i];
        totalChars += tools3000::ocr::OcrEngine::splitUtf8Codepoints(r.text).size();
        if (doDump) {
            std::printf("  [%02zu] (%d,%d %dx%d) conf=%.3f | %s\n",
                        i, r.box.x, r.box.y, r.box.width, r.box.height, r.confidence, r.text.c_str());
        }
    }
    const auto st = deep.lastRunStats();
    std::printf("[OCR-BENCH] image=%dx%d tier=%s sessions=%zu lines=%zu chars=%zu firstRun=%lldms warmRun=%lldms (det=%lld pack=%lld rec=%lld refine=%lld srvN=%zu) [%s]\n",
                img.cols, img.rows, deep.effectiveModelTier().c_str(), st.activeSessions,
                hotRes.size(), totalChars, coldMs, hotMs,
                st.detMs, st.packMs, st.recMs, st.refineMs, st.serverRefinedCount,
                st.bucketProfile.c_str());
    EXPECT_GT(hotRes.size(), 0u);

    wchar_t outDirBuf[1024]{};
    const DWORD outDirLen = GetEnvironmentVariableW(L"TOOLS3000_OCR_VERIFY_OUT_DIR", outDirBuf, static_cast<DWORD>(std::size(outDirBuf)));
    if (outDirLen > 0 && outDirLen < std::size(outDirBuf)) {
        using namespace tools3000::capture;
        const std::filesystem::path outDir(std::wstring(outDirBuf, outDirLen));
        std::filesystem::create_directories(outDir);

        // 1. 精准命中包含 "剩余" 且含 "5%" 的行（无匹配时回退首个含 "5%" 的行），原汁原味复现贴图局部划选 "5%" -> "10%" 并输出 4x 放大对比图
        int targetLineIdx = -1;
        for (size_t bi = 0; bi < hotRes.size(); ++bi) {
            if (hotRes[bi].text.find("5%") == std::string::npos) continue;
            if (targetLineIdx < 0) targetLineIdx = static_cast<int>(bi);
            if (hotRes[bi].text.find("\xE5\x89\xA9\xE4\xBD\x99") != std::string::npos) { // "剩余" UTF-8
                targetLineIdx = static_cast<int>(bi);
                break;
            }
        }
        if (targetLineIdx >= 0) {
            const auto& blk = hotRes[static_cast<size_t>(targetLineIdx)];
            const auto cps = tools3000::ocr::OcrEngine::splitUtf8Codepoints(blk.text);
            int idx5 = -1;
            for (size_t c = 0; c + 1 < cps.size(); ++c) {
                if (cps[c] == "5" && cps[c + 1] == "%") {
                    idx5 = static_cast<int>(c);
                    break;
                }
            }
            const auto charBoxes = (blk.charBoxes.size() == cps.size())
                ? blk.charBoxes
                : tools3000::ocr::OcrEngine::buildProportionalCharBoxes(blk.text, blk.box);
            if (idx5 >= 0 && static_cast<size_t>(idx5 + 1) < charBoxes.size()) {
                const cv::Rect selRect = charBoxes[static_cast<size_t>(idx5)] | charBoxes[static_cast<size_t>(idx5 + 1)];
                cv::Rect tailRect;
                for (size_t c = static_cast<size_t>(idx5 + 2); c < charBoxes.size(); ++c) {
                    tailRect = tailRect.empty() ? charBoxes[c] : (tailRect | charBoxes[c]);
                }
                std::printf("[VERIFY-REPLACE] line=\"%s\" sel=(%d,%d %dx%d) tail=(%d,%d %dx%d)\n",
                            blk.text.c_str(), selRect.x, selRect.y, selRect.width, selRect.height,
                            tailRect.x, tailRect.y, tailRect.width, tailRect.height);

                TextReplaceEngine::clearPatchCache();
                TextReplaceRenderSpec spec;
                spec.sourceRect = selRect;
                spec.charBoxes = {charBoxes[static_cast<size_t>(idx5)], charBoxes[static_cast<size_t>(idx5 + 1)]};
                spec.originalTextUtf8 = "5%";
                spec.newTextUtf8 = "10%";
                spec.bgMode = TextReplaceBgMode::Auto;
                spec.reflowRightTail = !tailRect.empty();
                spec.rightTailRect = tailRect;

                cv::Mat replaced = img.clone();
                TextReplaceEngine::renderReplacement(replaced, spec);
                cv::imwrite((outDir / "ppt_replaced_10pct.png").string(), replaced);

                // 生成 4x 放大上下对比特写（上：原图，下：改为 10% 后）
                const cv::Rect cropRc = cv::Rect(
                    (std::max)(0, blk.box.x - 10), (std::max)(0, blk.box.y - 16),
                    (std::min)(img.cols - (std::max)(0, blk.box.x - 10), blk.box.width + 44),
                    (std::min)(img.rows - (std::max)(0, blk.box.y - 16), blk.box.height + 32));
                cv::Mat topUp, botUp;
                cv::resize(img(cropRc), topUp, cv::Size(cropRc.width * 4, cropRc.height * 4), 0, 0, cv::INTER_NEAREST);
                cv::resize(replaced(cropRc), botUp, cv::Size(cropRc.width * 4, cropRc.height * 4), 0, 0, cv::INTER_NEAREST);
                cv::Mat sep(6, topUp.cols, CV_8UC3, cv::Scalar(40, 120, 240));
                cv::Mat cmp;
                cv::vconcat(std::vector<cv::Mat>{topUp, sep, botUp}, cmp);
                cv::imwrite((outDir / "ppt_shengyu_5_to_10_closeup.png").string(), cmp);
            }
        }

        // 1B. 若命中包含 "分摊" 的行（如 Excel 表格 "制造费用分摊（2021-09）"），原汁原味复现把 "分" 改为 "不分" 并输出 4x 放大对比图
        for (const auto& blk : hotRes) {
            if (blk.text.find("\xE5\x88\x86\xE6\x91\x8A") == std::string::npos) continue; // "分摊"
            const auto cps = tools3000::ocr::OcrEngine::splitUtf8Codepoints(blk.text);
            int idxFen = -1;
            for (size_t c = 0; c + 1 < cps.size(); ++c) {
                if (cps[c] == "\xE5\x88\x86" && cps[c + 1] == "\xE6\x91\x8A") {
                    idxFen = static_cast<int>(c);
                    break;
                }
            }
            const auto charBoxes = (blk.charBoxes.size() == cps.size())
                ? blk.charBoxes
                : tools3000::ocr::OcrEngine::buildProportionalCharBoxes(blk.text, blk.box);
            if (idxFen < 0 || static_cast<size_t>(idxFen + 1) >= charBoxes.size()) continue;

            const cv::Rect selRect = charBoxes[static_cast<size_t>(idxFen)];
            cv::Rect tailRect;
            int prevRight = selRect.x + selRect.width;
            for (size_t c = static_cast<size_t>(idxFen + 1); c < charBoxes.size(); ++c) {
                if (cps[c] == "\t") break;
                if (charBoxes[c].x - prevRight > (std::max)(20, selRect.height * 2)) break;
                tailRect = tailRect.empty() ? charBoxes[c] : (tailRect | charBoxes[c]);
                prevRight = (std::max)(prevRight, charBoxes[c].x + charBoxes[c].width);
            }
            std::printf("[VERIFY-FEN] line=\"%s\" sel=(%d,%d %dx%d) tail=(%d,%d %dx%d)\n",
                        blk.text.c_str(), selRect.x, selRect.y, selRect.width, selRect.height,
                        tailRect.x, tailRect.y, tailRect.width, tailRect.height);

            TextReplaceEngine::clearPatchCache();
            const auto fenDna = TextReplaceEngine::extractStyle(img, selRect, {selRect}, "\xE5\x88\x86");
            std::printf("[VERIFY-FEN] complex=%d bgCompl=%.2f fontSz=%.1f ink=[%.1f..%.1f]\n",
                        fenDna.complexBackground ? 1 : 0, fenDna.bgPlane.complexity,
                        fenDna.fontSizePx, fenDna.inkLeftX, fenDna.inkRightX);
            TextReplaceRenderSpec spec;
            spec.sourceRect = selRect;
            spec.charBoxes = {selRect};
            spec.originalTextUtf8 = "\xE5\x88\x86"; // "分"
            spec.newTextUtf8 = "\xE4\xB8\x8D\xE5\x88\x86"; // "不分"
            spec.bgMode = TextReplaceBgMode::Auto;
            spec.reflowRightTail = !tailRect.empty();
            spec.rightTailRect = tailRect;

            cv::Mat replaced = img.clone();
            TextReplaceEngine::renderReplacement(replaced, spec);
            const cv::Rect focusRc = (selRect | tailRect);
            const cv::Rect cropRc = cv::Rect(
                (std::max)(0, selRect.x - 64), (std::max)(0, focusRc.y - 10),
                (std::min)(img.cols - (std::max)(0, selRect.x - 64), focusRc.width + 110),
                (std::min)(img.rows - (std::max)(0, focusRc.y - 10), focusRc.height + 20));
            cv::Mat topUp, botUp;
            cv::resize(img(cropRc), topUp, cv::Size(cropRc.width * 4, cropRc.height * 4), 0, 0, cv::INTER_NEAREST);
            cv::resize(replaced(cropRc), botUp, cv::Size(cropRc.width * 4, cropRc.height * 4), 0, 0, cv::INTER_NEAREST);
            cv::Mat sep(6, topUp.cols, CV_8UC3, cv::Scalar(40, 120, 240));
            cv::Mat cmp;
            cv::vconcat(std::vector<cv::Mat>{topUp, sep, botUp}, cmp);
            cv::imwrite((outDir / "excel_fen_to_bufen_closeup.png").string(), cmp);
            break;
        }

        // 2. 在真实图上压印企业双行防泄密水印（首行 admin，次行当前日期时间）
        WatermarkConfig tiledWm;
        tiledWm.enabled = true;
        tiledWm.mode = WatermarkMode::TiledTraceability;
        tiledWm.textTemplate = "admin\\n{date} {time}";
        tiledWm.scale = 1.0f;
        tiledWm.opacity = 0.22f;
        cv::Mat wmTiled = img.clone();
        WatermarkRenderer::applyInPlace(wmTiled, tiledWm);
        cv::imwrite((outDir / "watermark_dual_tiled.png").string(), wmTiled);

        WatermarkConfig cornerWm = tiledWm;
        cornerWm.mode = WatermarkMode::CornerText;
        cornerWm.opacity = 0.88f;
        cv::Mat wmCorner = img.clone();
        WatermarkRenderer::applyInPlace(wmCorner, cornerWm);
        cv::imwrite((outDir / "watermark_dual_corner.png").string(), wmCorner);
    }
}

TEST(TextReplaceEngineTest, StyleDnaExtractionAndTableBorderPreservingReplacement) {
    using namespace tools3000::capture;

    initializeMarkupTextRenderer();
    TextReplaceEngine::clearPatchCache();

    // 1. 浅色表格单元格（含紧贴上/下/左/右的深灰表格边框线）：验证 6 维 DNA 克隆与改字绝不破坏表格线
    cv::Mat tableImg(140, 420, CV_8UC3, cv::Scalar(246, 248, 250));
    const cv::Rect cellBorder(24, 24, 240, 44);
    cv::rectangle(tableImg, cellBorder, cv::Scalar(160, 165, 172), 1, cv::LINE_8);
    cv::putText(tableImg, "1280.00", cv::Point(44, 54),
                cv::FONT_HERSHEY_SIMPLEX, 0.78, cv::Scalar(24, 28, 34), 2, cv::LINE_AA);

    const cv::Rect textBox(42, 34, 120, 24);
    const std::vector<cv::Rect> propBoxes =
        tools3000::ocr::OcrEngine::buildProportionalCharBoxes("1280.00", textBox);
    const auto lightStyle = TextReplaceEngine::extractStyle(tableImg, textBox, propBoxes, "1280.00");
    EXPECT_FALSE(lightStyle.complexBackground);
    EXPECT_LT(static_cast<int>(lightStyle.textBgr[2]), 85);
    EXPECT_LT(static_cast<int>(lightStyle.textBgr[1]), 85);
    EXPECT_LT(static_cast<int>(lightStyle.textBgr[0]), 85);
    EXPECT_GT(lightStyle.fontSizePx, 12.0f);
    EXPECT_LT(lightStyle.fontSizePx, 36.0f);

    // 通过无损 MarkupEngine::TextReplace 图元将 "1280.00" 原位无痕改为 "9980.00"
    MarkupEngine engine;
    engine.setBaseImage(tableImg);
    MarkupElement trElem;
    trElem.id = 1;
    trElem.tool = MarkupTool::TextReplace;
    trElem.text = "9980.00";
    trElem.replaceOriginalText = "1280.00";
    trElem.replaceSourceRect = textBox;
    trElem.replaceCharBoxes = propBoxes;
    trElem.startPt = cv::Point(textBox.x, textBox.y);
    trElem.endPt = cv::Point(textBox.x + textBox.width, textBox.y + textBox.height);
    trElem.color = MarkupColor{lightStyle.textBgr[2], lightStyle.textBgr[1], lightStyle.textBgr[0], 255};
    trElem.fontSize = lightStyle.fontSizePx;
    trElem.replaceFontClass = static_cast<int>(lightStyle.fontClass);
    trElem.replaceFontWeight = lightStyle.fontWeight;
    trElem.replaceStyleInitialized = true;
    engine.importElements({trElem});

    const cv::Mat replaced = engine.getCompositeImage(false);
    ASSERT_FALSE(replaced.empty());

    // 验证表格上/下边框线像素 100% 未受损
    const cv::Vec3b topBorderPx = replaced.at<cv::Vec3b>(24, 100);
    const cv::Vec3b botBorderPx = replaced.at<cv::Vec3b>(24 + 44 - 1, 100);
    EXPECT_NEAR(topBorderPx[0], 160, 2);
    EXPECT_NEAR(botBorderPx[0], 160, 2);

    // 验证原位文字区已绘制出深色新字且与旧图像素产生真实变化
    int darkNewInkPixels = 0;
    int diffPixels = 0;
    for (int y = textBox.y; y < textBox.y + textBox.height; ++y) {
        for (int x = textBox.x; x < textBox.x + textBox.width; ++x) {
            const cv::Vec3b pNew = replaced.at<cv::Vec3b>(y, x);
            const cv::Vec3b pOld = tableImg.at<cv::Vec3b>(y, x);
            if (pNew[0] < 120 && pNew[1] < 120 && pNew[2] < 120) ++darkNewInkPixels;
            if (std::abs(int(pNew[0]) - int(pOld[0])) + std::abs(int(pNew[1]) - int(pOld[1])) > 30) ++diffPixels;
        }
    }
    EXPECT_GT(darkNewInkPixels, 35);
    EXPECT_GT(diffPixels, 20);

    // 2. 暗色代码终端等宽字自动识别：验证等宽 charBoxes -> Monospace + 亮色字色提取
    cv::Mat darkTerm(100, 340, CV_8UC3, cv::Scalar(34, 30, 28));
    cv::putText(darkTerm, "PORT=8080", cv::Point(30, 60),
                cv::FONT_HERSHEY_SIMPLEX, 0.72, cv::Scalar(230, 234, 240), 2, cv::LINE_AA);
    const cv::Rect monoBox(28, 38, 144, 24);
    std::vector<cv::Rect> monoChars;
    for (int i = 0; i < 9; ++i) {
        monoChars.emplace_back(28 + i * 16, 38, 16, 24);
    }
    const auto darkStyle = TextReplaceEngine::extractStyle(darkTerm, monoBox, monoChars, "PORT=8080");
    EXPECT_EQ(darkStyle.fontClass, TextReplaceFontClass::Monospace);
    EXPECT_GT(static_cast<int>(darkStyle.textBgr[2]), 180);
    EXPECT_GT(static_cast<int>(darkStyle.textBgr[1]), 180);

    // 3. 高频复杂底纹图片：验证强制纹理修复模式在无 AI 模型时优雅降级 OpenCV Telea 零崩溃
    cv::Mat complexImg(140, 320, CV_8UC3);
    for (int y = 0; y < complexImg.rows; ++y) {
        for (int x = 0; x < complexImg.cols; ++x) {
            const uint8_t v = static_cast<uint8_t>(((x / 8 + y / 8) % 2 == 0) ? 220 : 120);
            complexImg.at<cv::Vec3b>(y, x) = cv::Vec3b(v, static_cast<uint8_t>(255 - v / 2), 180);
        }
    }
    cv::putText(complexImg, "SALE", cv::Point(40, 78),
                cv::FONT_HERSHEY_SIMPLEX, 0.9, cv::Scalar(18, 22, 235), 2, cv::LINE_AA);
    const cv::Rect saleBox(38, 52, 86, 30);
    const auto complexStyle = TextReplaceEngine::extractStyle(complexImg, saleBox, {}, "SALE");
    EXPECT_TRUE(complexStyle.complexBackground);

    TextReplaceRenderSpec spec;
    spec.originalTextUtf8 = "SALE";
    spec.newTextUtf8 = "FREE";
    spec.sourceRect = saleBox;
    spec.textBgr = complexStyle.textBgr;
    spec.fontSizePx = complexStyle.fontSizePx;
    spec.fontWeight = complexStyle.fontWeight;
    spec.fontClass = complexStyle.fontClass;
    spec.styleInitialized = true;
    spec.bgMode = TextReplaceBgMode::AiInpaint;
    const cv::Rect covered = TextReplaceEngine::renderReplacement(complexImg, spec);
    EXPECT_GT(covered.width, 0);
    EXPECT_GT(covered.height, 0);
}

TEST(TextReplaceEngineTest, RenderAndMatchCalibrationZeroVerticalDriftAndEditOverlayCaretHitTest) {
    using namespace tools3000::capture;

    TextReplaceEngine::clearPatchCache();

    // 复刻用户真实深色界面场景：暗灰背景 (#282B33) + 18px 微软雅黑柔和灰白中文字 "回归测试"
    cv::Mat darkUi(120, 420, CV_8UC3, cv::Scalar(51, 43, 40));
    TextReplaceRenderSpec seedSpec;
    seedSpec.sourceRect = cv::Rect(80, 40, 80, 26);
    seedSpec.originalTextUtf8 = "回归测试";
    seedSpec.newTextUtf8 = "回归测试";
    seedSpec.textBgr = cv::Vec3b(224, 220, 214); // 柔和灰白
    seedSpec.fontSizePx = 18.0f;
    seedSpec.fontWeight = 500;
    seedSpec.fontClass = TextReplaceFontClass::UiSans;
    seedSpec.styleInitialized = true;
    seedSpec.bgMode = TextReplaceBgMode::CleanPlane;
    TextReplaceEngine::renderReplacement(darkUi, seedSpec);

    // 测出种子图真实文字墨水垂直中心（作为权威 Ground Truth）
    auto measureInkCenterY = [&](const cv::Mat& img, const cv::Rect& roi) -> float {
        double wSum = 0.0, ySum = 0.0;
        for (int y = roi.y; y < roi.y + roi.height; ++y) {
            for (int x = roi.x; x < roi.x + roi.width; ++x) {
                const cv::Vec3b p = img.at<cv::Vec3b>(y, x);
                const int d = (std::max)({std::abs(int(p[0]) - 51), std::abs(int(p[1]) - 43), std::abs(int(p[2]) - 40)});
                if (d >= 45) {
                    wSum += d;
                    ySum += static_cast<double>(y) * d;
                }
            }
        }
        return (wSum > 1.0) ? static_cast<float>(ySum / wSum) : -1.0f;
    };

    const cv::Rect probeBox(74, 34, 96, 38);
    const float truthCenterY = measureInkCenterY(darkUi, probeBox);
    ASSERT_GT(truthCenterY, 35.0f);

    // 1. 验证 Render-and-Match 闭环自校准反解字号精准命中 18.0px（绝不再虚胖到 21.5px！）
    const ExtractedTextStyle dna = TextReplaceEngine::extractStyle(darkUi, probeBox, {}, "回归测试");
    EXPECT_TRUE(dna.darkBackground);
    EXPECT_NEAR(dna.fontSizePx, 18.0f, 1.5f) << "Render-and-Match must recover true ~18px font size without inflation";
    // 深底字色应忠实还原柔和灰白，不过曝成纯白 255
    EXPECT_GE(static_cast<int>(dna.textBgr[2]), 185);
    EXPECT_LE(static_cast<int>(dna.textBgr[2]), 240);

    // 2. 执行原位改字 "回归测试" -> "回没测试"，验证新字垂直重心与原字严格齐平（偏差 <= 1.5px，零向上飘移！）
    cv::Mat replaced = darkUi.clone();
    TextReplaceRenderSpec repSpec;
    repSpec.sourceRect = probeBox;
    repSpec.originalTextUtf8 = "回归测试";
    repSpec.newTextUtf8 = "回没测试";
    repSpec.textBgr = dna.textBgr;
    repSpec.fontSizePx = dna.fontSizePx;
    repSpec.fontWeight = dna.fontWeight;
    repSpec.fontClass = dna.fontClass;
    repSpec.styleInitialized = true;
    repSpec.bgMode = TextReplaceBgMode::Auto;
    TextReplaceEngine::renderReplacement(replaced, repSpec);

    const float replacedCenterY = measureInkCenterY(replaced, probeBox);
    ASSERT_GT(replacedCenterY, 35.0f);
    EXPECT_NEAR(replacedCenterY, truthCenterY, 1.5f) << "Replaced CJK text must align vertically with original ink";

    // 3. 验证编辑态 GPU 顶层焦框、选中高亮矩形与光标位置计算，以及鼠标点击精准落点字符索引
    repSpec.isEditing = true;
    repSpec.selAnchorUtf16 = 0;
    repSpec.caretUtf16 = 4; // 全选 4 个汉字
    const auto ov = TextReplaceEngine::computeEditOverlayMetrics(darkUi, repSpec);
    EXPECT_TRUE(ov.valid);
    EXPECT_TRUE(ov.darkBackground);
    EXPECT_GT(ov.boxRight - ov.boxLeft, 50.0f);
    EXPECT_GT(ov.caretBottom - ov.caretTop, 12.0f);
    EXPECT_FALSE(ov.selectionRects.empty());
    EXPECT_GT(ov.caretX, ov.drawTextX + 40.0f);

    // 点击最左侧应落点索引 0，点击最右侧应落点索引 4，点击中点应落点索引 2
    const float midY = (ov.caretTop + ov.caretBottom) * 0.5f;
    EXPECT_EQ(TextReplaceEngine::hitTestCaretIndex(darkUi, repSpec, ov.drawTextX - 4.0f, midY), 0u);
    EXPECT_EQ(TextReplaceEngine::hitTestCaretIndex(darkUi, repSpec, ov.caretX + 12.0f, midY), 4u);
    const size_t midIdx = TextReplaceEngine::hitTestCaretIndex(
        darkUi, repSpec, (ov.drawTextX + ov.caretX) * 0.5f, midY);
    EXPECT_GE(midIdx, 1u);
    EXPECT_LE(midIdx, 3u);
}

TEST(TextReplaceEngineTest, AiInpaintModelStatusLifecycle) {
    using namespace tools3000::capture;

    auto& engine = AiInpaintEngine::instance();
    const auto status = engine.getModelStatus();
    EXPECT_FALSE(status.modelDir.empty());
    EXPECT_EQ(status.modelFile, "migan_pipeline_v2.onnx");
    EXPECT_EQ(status.totalBytes, 28079181ULL);
    EXPECT_FALSE(status.downloading);
    engine.cancelModelDownload();
    engine.releaseResources();
}

TEST(TextReplaceEngineTest, PinEndToEndInPlaceTextReplaceEditCommitAndUndoViaPlugin) {
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
                         (L"tools3000_text_replace_e2e_" + std::to_wstring(GetCurrentProcessId()) + L".png");

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

    // 验证 AI 修复模型 IPC 契约在线可达
    const auto modelSt = invokeRpc(4100, "capture.getInpaintModelStatus", nlohmann::json::object());
    EXPECT_EQ(modelSt.value("modelFile", ""), "migan_pipeline_v2.onnx");
    EXPECT_FALSE(modelSt.value("modelDir", "").empty());

    // 构造清晰白底数字贴图
    cv::Mat img(160, 420, CV_8UC3, cv::Scalar(252, 253, 255));
    cv::putText(img, "2025", cv::Point(44, 72),
                cv::FONT_HERSHEY_SIMPLEX, 0.95, cv::Scalar(28, 30, 36), 2, cv::LINE_AA);
    std::vector<uchar> encoded;
    ASSERT_TRUE(cv::imencode(".png", img, encoded));
    {
        std::ofstream ofs(tempPng, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    }

    ASSERT_TRUE(invokeRpc(4101, "capture.pinImageFile", {
        {"path", tools3000::core::WinUtils::wstringToUtf8(tempPng.wstring())}
    }).value("success", false));

    nlohmann::json mockBlocks = nlohmann::json::array({
        {{"text", "2025"}, {"x", 40}, {"y", 46}, {"w", 86}, {"h", 28}, {"layoutRegionId", 0}, {"alignedRowId", 0}}
    });

    // 1) 选字后悬浮舱必须显示 [改字] 按钮
    const auto selRes = invokeRpc(4102, "capture.inspectPinOcrState", {
        {"mockBlocks", mockBlocks},
        {"selectAll", true},
        {"forceRender", true}
    });
    ASSERT_TRUE(selRes.value("success", false));
    EXPECT_TRUE(selRes.value("textReplaceBtnVisible", false));
    EXPECT_FALSE(selRes.value("editingTextReplace", true));

    // 2) 进入原位改字 -> 直接键入 "2026" 替换默认全选旧文字 -> 拖动左侧 6 点微晶抓手移动文字位置 -> 提交保存
    const auto editRes = invokeRpc(4103, "capture.inspectPinOcrState", {
        {"startTextReplace", true},
        {"replaceTypeText", "2026"}
    });
    ASSERT_TRUE(editRes.value("success", false));
    EXPECT_TRUE(editRes.value("editingTextReplace", false));
    EXPECT_EQ(editRes.value("latestTextReplaceText", ""), "2026");
    EXPECT_TRUE(editRes.value("trDragHandleVisible", false));

    HWND pinHwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(editRes.value("pinHwnd", std::uint64_t{0})));
    ASSERT_NE(pinHwnd, nullptr);
    ASSERT_TRUE(IsWindow(pinHwnd));
    const int gx = editRes.value("trDragHandleCenterX", 0);
    const int gy = editRes.value("trDragHandleCenterY", 0);
    SendMessageW(pinHwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(gx, gy));
    SendMessageW(pinHwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(gx + 9, gy - 4));
    SendMessageW(pinHwnd, WM_LBUTTONUP, 0, MAKELPARAM(gx + 9, gy - 4));

    const auto movedRes = invokeRpc(41031, "capture.inspectPinOcrState", nlohmann::json::object());
    EXPECT_TRUE(movedRes.value("editingTextReplace", false));
    EXPECT_NEAR(movedRes.value("latestTextReplaceOffsetX", 0.0), 9.0, 0.6);
    EXPECT_NEAR(movedRes.value("latestTextReplaceOffsetY", 0.0), -4.0, 0.6);

    const auto commitRes = invokeRpc(4104, "capture.inspectPinOcrState", {
        {"commitTextReplace", true}
    });
    ASSERT_TRUE(commitRes.value("success", false));
    EXPECT_FALSE(commitRes.value("editingTextReplace", true));
    EXPECT_EQ(commitRes.value("markupCount", 0), 1);
    EXPECT_TRUE(commitRes.value("canUndoPinMarkup", false));
    EXPECT_EQ(commitRes.value("ocrRunCount", 99u), 0u);

    // 3) Ctrl+Z 一步无损还原：图元清零且无二次 OCR 重跑
    const auto undoRes = invokeRpc(4105, "capture.inspectPinOcrState", {
        {"undoMarkup", true}
    });
    ASSERT_TRUE(undoRes.value("success", false));
    EXPECT_EQ(undoRes.value("markupCount", 99), 0);
    EXPECT_TRUE(undoRes.value("canRedoPinMarkup", false));
    EXPECT_EQ(undoRes.value("ocrRunCount", 99u), 0u);

    while (HWND existing = FindWindowW(L"Tools3000_PinWindow", nullptr)) {
        SendMessageW(existing, WM_KEYDOWN, VK_DELETE, 0);
        if (IsWindow(existing)) DestroyWindow(existing);
    }
    capturePlugin->shutdown();
    guard.plugin = nullptr;
    EXPECT_TRUE(FreeLibrary(hMod));
    guard.mod = nullptr;
}

TEST(TextReplaceEngineTest, CompactDenseDarkLinesNoTeleaGhostAndSameLineRightReflowAndRigidDragBox) {
    using namespace tools3000::capture;

    // 复刻深色紧凑多行界面实拍场景：#282B33 纯深灰底，上下邻行亮白字间距仅 3px，
    // 左侧紧贴 '+' 号，中间为待改短词，右侧紧邻同行尾巴文字
    const cv::Scalar darkBg(51, 43, 40); // BGR = (51, 43, 40) -> #282B33
    const cv::Scalar brightFg(230, 224, 218);
    cv::Mat base(180, 560, CV_8UC3, darkBg);

    // 上一行与下一行紧邻亮白字（距目标行上下仅 3px，旧版会污染外围采样环带导致误判复杂纹理送入 Telea）
    cv::putText(base, "Upper tight white line text 1234567890", cv::Point(28, 42),
                cv::FONT_HERSHEY_SIMPLEX, 0.56, brightFg, 1, cv::LINE_AA);
    cv::putText(base, "Lower tight white line text 1234567890", cv::Point(28, 88),
                cv::FONT_HERSHEY_SIMPLEX, 0.56, brightFg, 1, cv::LINE_AA);

    // 目标行：左侧 '+' (x=80..94)，中间待改短词 "AB" (x=98..126)，右侧原文 "TAIL" (x=134..190)
    const cv::Rect plusRect(80, 48, 14, 18);
    cv::line(base, cv::Point(87, 50), cv::Point(87, 64), brightFg, 2);
    cv::line(base, cv::Point(81, 57), cv::Point(93, 57), brightFg, 2);

    const cv::Rect targetRoi(98, 47, 30, 20);
    cv::putText(base, "AB", cv::Point(99, 64),
                cv::FONT_HERSHEY_SIMPLEX, 0.60, brightFg, 2, cv::LINE_AA);

    const cv::Rect rightTailRect(136, 47, 64, 20);
    cv::putText(base, "TAIL", cv::Point(138, 64),
                cv::FONT_HERSHEY_SIMPLEX, 0.60, brightFg, 2, cv::LINE_AA);

    // 1) 纯深灰紧凑多行必须保持 complexBackground == false（绝不可被上下邻行白字拉高方差误送 Telea）
    const ExtractedTextStyle dna = TextReplaceEngine::extractStyle(base, targetRoi, {}, "AB");
    EXPECT_FALSE(dna.complexBackground);

    // 2) 将短词 "AB" 改为更长的新词 "ABCDEFGH" 并开启同行右侧顺延让位
    TextReplaceRenderSpec spec;
    spec.sourceRect = targetRoi;
    spec.originalTextUtf8 = "AB";
    spec.newTextUtf8 = "ABCDEFGH";
    spec.textBgr = dna.textBgr;
    spec.fontSizePx = dna.fontSizePx;
    spec.fontWeight = dna.fontWeight;
    spec.fontClass = dna.fontClass;
    spec.bgMode = TextReplaceBgMode::Auto;
    spec.styleInitialized = true;
    spec.reflowRightTail = true;
    spec.rightTailRect = rightTailRect;

    cv::Mat canvas = base.clone();
    const cv::Rect renderRoi = TextReplaceEngine::renderReplacement(canvas, spec);
    ASSERT_GT(renderRoi.width, 0);
    ASSERT_GT(renderRoi.height, 0);

    // 2a) 左侧紧邻 '+' 号像素必须 100% 完好无损（不得被旧字擦除越界咬伤）
    int plusBrightPx = 0;
    for (int y = plusRect.y; y < plusRect.y + plusRect.height; ++y) {
        for (int x = plusRect.x; x < plusRect.x + plusRect.width; ++x) {
            const cv::Vec3b p = canvas.at<cv::Vec3b>(y, x);
            if (p[0] > 180 && p[1] > 180 && p[2] > 180) {
                ++plusBrightPx;
            }
        }
    }
    EXPECT_GE(plusBrightPx, 30);

    // 2b) 原右侧尾巴 "TAIL" 应已顺延至 renderRoi 右侧之后，在新区域内存在清晰亮白笔画
    int shiftedTailBrightPx = 0;
    const int tailSearchX0 = (std::min)(canvas.cols - 1, renderRoi.x + renderRoi.width + 1);
    const int tailSearchX1 = (std::min)(canvas.cols, tailSearchX0 + rightTailRect.width + 24);
    for (int y = rightTailRect.y; y < rightTailRect.y + rightTailRect.height; ++y) {
        for (int x = tailSearchX0; x < tailSearchX1; ++x) {
            const cv::Vec3b p = canvas.at<cv::Vec3b>(y, x);
            if (p[0] > 160 && p[1] > 160 && p[2] > 160) {
                ++shiftedTailBrightPx;
            }
        }
    }
    EXPECT_GE(shiftedTailBrightPx, 45);

    // 3) 拖动新文字至下方新位置 (offsetX=+50, offsetY=+48)：
    //    编辑框与 renderRoi 必须 1:1 刚体平移紧贴新文字，绝不再与旧擦除框 targetRoi 拉扯成巨型框；
    //    同时旧擦除位置 targetRoi 中心必须被干净抹回纯深灰底，无 Telea 灰白鬼影！
    spec.offsetX = 50.0f;
    spec.offsetY = 48.0f;
    spec.isEditing = true;
    spec.caretUtf16 = 8;
    spec.selAnchorUtf16 = 8;

    cv::Mat movedCanvas = base.clone();
    const cv::Rect movedRoi = TextReplaceEngine::renderReplacement(movedCanvas, spec);
    ASSERT_GT(movedRoi.width, 0);
    const auto ov = TextReplaceEngine::computeEditOverlayMetrics(base, spec);
    ASSERT_TRUE(ov.valid);

    // 编辑框整体刚体平移：顶边与左边都必须远离原 targetRoi，高度保持单行紧致
    EXPECT_GT(ov.boxTop, static_cast<float>(targetRoi.y) + 32.0f);
    EXPECT_GT(ov.boxLeft, static_cast<float>(targetRoi.x) + 35.0f);
    EXPECT_LT(ov.boxBottom - ov.boxTop, static_cast<float>(targetRoi.height) * 2.2f);
    EXPECT_GT(movedRoi.y, targetRoi.y + 30);

    // 旧擦除框内部无任何灰白脏鬼影（最大像素偏差 <= 12）
    int maxGhostDiff = 0;
    for (int y = targetRoi.y + 2; y < targetRoi.y + targetRoi.height - 2; ++y) {
        for (int x = targetRoi.x + 2; x < targetRoi.x + targetRoi.width - 2; ++x) {
            const cv::Vec3b p = movedCanvas.at<cv::Vec3b>(y, x);
            const int diff = std::abs(static_cast<int>(p[0]) - 51) +
                             std::abs(static_cast<int>(p[1]) - 43) +
                             std::abs(static_cast<int>(p[2]) - 40);
            maxGhostDiff = (std::max)(maxGhostDiff, diff);
        }
    }
    EXPECT_LE(maxGhostDiff, 18);

    // 4) 同行顺延抗削顶与独立局部底色验证：
    //    模拟真实 OCR 紧致框比文字顶部低 3px（裁在 "立/i" 悬空顶头点下方 1px 空隙处），
    //    且左侧被改原词位于浅灰代码胶囊内、右侧顺延文字位于深灰页面主底色上
    cv::Mat reflowImg(160, 560, CV_8UC3, darkBg);
    // 上一行正文（间隔 6px 空白走廊，验证绝不误吸上一行）
    cv::putText(reflowImg, "Previous line must stay untouched", cv::Point(20, 32),
                cv::FONT_HERSHEY_SIMPLEX, 0.52, brightFg, 1, cv::LINE_AA);
    // 左侧深灰代码胶囊 (#383D47) 内待改短词 "OK"
    const cv::Scalar pillBg(71, 61, 56);
    cv::rectangle(reflowImg, cv::Rect(30, 44, 54, 26), pillBg, cv::FILLED);
    const cv::Rect pillWordRoi(36, 48, 38, 18);
    cv::putText(reflowImg, "OK", cv::Point(37, 63),
                cv::FONT_HERSHEY_SIMPLEX, 0.56, brightFg, 2, cv::LINE_AA);

    // 右侧同行文字主体 (y=49..65, 短词 "HI" x=96..124) + 刻意位于 OCR 框四向边缘外的像素：
    //   顶部 y=46..47 悬空顶头点（模拟 立/i）
    //   底部 y=67..69 悬空下伸部（模拟 g/y/下点）
    //   左侧 x=92..93（与 x=96 隔 2px 缝，模拟 氵/“ 偏旁）
    //   右侧 x=128..131（与 x=124 隔 4px 全角留白，模拟行尾全角句号/高右括号且高出顶部至 y=47）
    const cv::Rect tightOcrTailRoi(96, 49, 28, 17);
    cv::putText(reflowImg, "HI", cv::Point(98, 64),
                cv::FONT_HERSHEY_SIMPLEX, 0.56, brightFg, 2, cv::LINE_AA);
    cv::rectangle(reflowImg, cv::Rect(106, 46, 3, 2), brightFg, cv::FILLED); // 顶部悬空点
    cv::rectangle(reflowImg, cv::Rect(112, 67, 3, 3), brightFg, cv::FILLED); // 底部下伸部
    cv::rectangle(reflowImg, cv::Rect(92, 52, 2, 10), brightFg, cv::FILLED); // 左侧隔 2px 字缝偏旁
    cv::rectangle(reflowImg, cv::Rect(128, 47, 4, 17), brightFg, cv::FILLED); // 右侧隔 4px 全角留白高标点

    TextReplaceRenderSpec rfSpec;
    rfSpec.sourceRect = pillWordRoi;
    rfSpec.originalTextUtf8 = "OK";
    rfSpec.newTextUtf8 = "LONG_REFLOW_WORD";
    rfSpec.bgMode = TextReplaceBgMode::Auto;
    rfSpec.reflowRightTail = true;
    rfSpec.rightTailRect = tightOcrTailRoi;

    cv::Mat rfCanvas = reflowImg.clone();
    const cv::Rect rfRoi = TextReplaceEngine::renderReplacement(rfCanvas, rfSpec);
    ASSERT_GT(rfRoi.width, 0);

    // 4a) 顶/底/右高标点顶边原位边缘点必须全部清平归零（贴合页面深灰底而非浅灰胶囊底，0 残点！）
    auto expectClearedToPageBg = [&](const cv::Rect& r) {
        for (int y = r.y; y < r.y + r.height; ++y) {
            for (int x = r.x; x < r.x + r.width; ++x) {
                const cv::Vec3b p = rfCanvas.at<cv::Vec3b>(y, x);
                const int diffFromPageBg = std::abs(static_cast<int>(p[0]) - 51) +
                                           std::abs(static_cast<int>(p[1]) - 43) +
                                           std::abs(static_cast<int>(p[2]) - 40);
                EXPECT_LE(diffFromPageBg, 18);
            }
        }
    };
    expectClearedToPageBg(cv::Rect(106, 46, 3, 2)); // 原顶部悬空点已清净
    expectClearedToPageBg(cv::Rect(112, 67, 3, 3)); // 原底部下伸部已清净
    expectClearedToPageBg(cv::Rect(128, 47, 4, 1)); // 原右侧 4px 全角留白高标点顶边已连根搬走

    // 4b) 平移后的目标区域顶部 (y=46..47)、底部 (y=67..69) 与右端高标点全部完整保留（四向零削切！）
    int movedTopDotBrightPx = 0;
    int movedBotDescBrightPx = 0;
    int movedRightPunctBrightPx = 0;
    const int movedStartX = rfRoi.x + rfRoi.width;
    for (int x = movedStartX; x < rfCanvas.cols - 4; ++x) {
        for (int y = 46; y <= 47; ++y) {
            const cv::Vec3b p = rfCanvas.at<cv::Vec3b>(y, x);
            if (p[0] > 180 && p[1] > 180 && p[2] > 180) ++movedTopDotBrightPx;
        }
        for (int y = 67; y <= 69; ++y) {
            const cv::Vec3b p = rfCanvas.at<cv::Vec3b>(y, x);
            if (p[0] > 180 && p[1] > 180 && p[2] > 180) ++movedBotDescBrightPx;
        }
        if (x >= movedStartX + 32) {
            for (int y = 47; y <= 63; ++y) {
                const cv::Vec3b p = rfCanvas.at<cv::Vec3b>(y, x);
                if (p[0] > 180 && p[1] > 180 && p[2] > 180) ++movedRightPunctBrightPx;
            }
        }
    }
    EXPECT_GE(movedTopDotBrightPx, 8);
    EXPECT_GE(movedBotDescBrightPx, 6);
    EXPECT_GE(movedRightPunctBrightPx, 40);
}

TEST(TextReplaceEngineTest, NeighborGlyphOwnershipGuardPreservesLeftDotCrossBoundaryAndReflow) {
    using namespace tools3000::capture;

    initializeMarkupTextRenderer();
    TextReplaceEngine::clearPatchCache();

    // 场景 1：模拟 PPT 屏摄浅灰白底上 "剩余5%待" —— 选区 "5%" 左边界刻意偏左 6px，
    // 同时切进左侧 "余" 字的人字头右捺（跨左界连笔）并把 "余" 字右下角独立小点整颗包入选区内部！
    // 同时右侧选区也稍微切进右侧邻字 "待" 的左偏旁 2px，并叠加紧邻上一行下伸部探入顶边 2px。
    cv::Mat img(140, 360, CV_8UC3, cv::Scalar(232, 224, 218));
    const cv::Scalar ink(36, 34, 32);

    // 左侧邻字 "余" 的主体（x=76..103, y=54..78）：
    //   人字头右捺斜向下向右伸到 x=103（跨入 sourceRect.x = 98 达 6px！）
    cv::line(img, cv::Point(88, 54), cv::Point(76, 65), ink, 2, cv::LINE_8);
    cv::line(img, cv::Point(88, 54), cv::Point(103, 64), ink, 2, cv::LINE_8);
    cv::line(img, cv::Point(88, 62), cv::Point(88, 78), ink, 2, cv::LINE_8);
    cv::line(img, cv::Point(80, 68), cv::Point(95, 68), ink, 2, cv::LINE_8);
    //   "余" 字右下角独立小点（完全位于 sourceRect.x=98 右侧的 x=99..102, y=71..75，像素上与主干完全断开！）
    cv::rectangle(img, cv::Rect(99, 71, 4, 5), ink, cv::FILLED);

    // 真正待替换的目标文字 "5%"（x=107..136, y=56..77）
    cv::putText(img, "5%", cv::Point(107, 76), cv::FONT_HERSHEY_SIMPLEX, 0.72, ink, 2, cv::LINE_AA);

    // 上一行探入顶缘 2px 的下伸部尾巴（x=114..118, y=42..55，主体在 y<54 外部）
    cv::rectangle(img, cv::Rect(114, 42, 4, 14), ink, cv::FILLED);

    // 右侧邻字（x=140..158, y=56..76），其左缘 x=140..141 被 sourceRect 右界 (x=142) 轻微切入 2px
    cv::rectangle(img, cv::Rect(140, 56, 3, 20), ink, cv::FILLED);
    cv::rectangle(img, cv::Rect(146, 58, 12, 16), ink, cv::FILLED);

    // 刻意左偏 6px、右偏 2px 的 OCR 字符框
    const cv::Rect biasedSourceRect(98, 54, 44, 24);
    const auto st = TextReplaceEngine::extractStyle(img, biasedSourceRect, {}, "5%");
    // 剔除渗入左界的 "余" 字后，真实 inkLeftX 必须落在 "5" 附近 (>= 105)，绝不被 "余" 的 x=98 拖偏！
    EXPECT_GE(st.inkLeftX, 105.0f);

    // 先验证非顺延替换：把 "5%" 改为 "10%"（空文本擦除测试先精确校验擦除结果）
    TextReplaceRenderSpec eraseOnlySpec;
    eraseOnlySpec.sourceRect = biasedSourceRect;
    eraseOnlySpec.originalTextUtf8 = "5%";
    eraseOnlySpec.newTextUtf8 = "";
    eraseOnlySpec.bgMode = TextReplaceBgMode::Auto;

    cv::Mat erasedCanvas = img.clone();
    TextReplaceEngine::renderReplacement(erasedCanvas, eraseOnlySpec);

    auto countDarkPx = [&](const cv::Mat& m, const cv::Rect& r) -> int {
        int cnt = 0;
        for (int y = r.y; y < r.y + r.height; ++y) {
            for (int x = r.x; x < r.x + r.width; ++x) {
                const cv::Vec3b p = m.at<cv::Vec3b>(y, x);
                if (p[0] < 120 && p[1] < 120 && p[2] < 120) ++cnt;
            }
        }
        return cnt;
    };

    // 1) "余" 字跨左界右捺（x=98..103, y=58..65）必须完好保全，零擦伤！
    const int origNaiPx = countDarkPx(img, cv::Rect(98, 58, 6, 8));
    const int afterNaiPx = countDarkPx(erasedCanvas, cv::Rect(98, 58, 6, 8));
    EXPECT_EQ(afterNaiPx, origNaiPx);

    // 2) "余" 字完全落入选区内部的右下角独立小点（x=99..102, y=71..75）必须 100% 完好保全！
    const int origDotPx = countDarkPx(img, cv::Rect(99, 71, 4, 5));
    const int afterDotPx = countDarkPx(erasedCanvas, cv::Rect(99, 71, 4, 5));
    EXPECT_EQ(afterDotPx, origDotPx);

    // 3) 右侧邻字跨界左偏旁（x=140..141, y=56..75）必须完好保全！
    const int origRightPx = countDarkPx(img, cv::Rect(140, 56, 2, 20));
    const int afterRightPx = countDarkPx(erasedCanvas, cv::Rect(140, 56, 2, 20));
    EXPECT_EQ(afterRightPx, origRightPx);

    // 4) 上一行探入顶缘的下伸部尾巴（x=114..117, y=54..55）必须完好保全！
    const int origTopTailPx = countDarkPx(img, cv::Rect(114, 54, 4, 2));
    const int afterTopTailPx = countDarkPx(erasedCanvas, cv::Rect(114, 54, 4, 2));
    EXPECT_EQ(afterTopTailPx, origTopTailPx);

    // 5) 真正的目标文字 "5%" 区域（x=107..136, y=58..76）必须 100% 擦除干净！
    const int remainingTargetPx = countDarkPx(erasedCanvas, cv::Rect(107, 58, 30, 19));
    EXPECT_EQ(remainingTargetPx, 0);

    // 场景 2：单字符替换安全阀豁免 —— 单独选中一个孤立窄字符 "1"（左侧有邻字），不能把 "1" 误当成邻字点保护掉！
    cv::Mat singleCharImg(100, 220, CV_8UC3, cv::Scalar(242, 244, 246));
    cv::rectangle(singleCharImg, cv::Rect(40, 34, 14, 20), ink, cv::FILLED); // 左侧邻字
    cv::rectangle(singleCharImg, cv::Rect(58, 36, 4, 16), ink, cv::FILLED);  // 单独选中的窄字符 "1"
    TextReplaceRenderSpec singleSpec;
    singleSpec.sourceRect = cv::Rect(57, 34, 7, 20);
    singleSpec.originalTextUtf8 = "1";
    singleSpec.newTextUtf8 = "";
    singleSpec.bgMode = TextReplaceBgMode::Auto;
    cv::Mat singleOut = singleCharImg.clone();
    TextReplaceEngine::renderReplacement(singleOut, singleSpec);
    EXPECT_EQ(countDarkPx(singleOut, cv::Rect(58, 36, 4, 16)), 0);

    // 场景 3：同行顺延原图真实墨水字距保真（如 "分" -> "不分" 紧邻 "摊"）：
    // 禁止按 advanceW + 硬编码 3px 盲推造成半字宽大缝隙，顺延后新字右缘到右侧尾巴首笔左缘应紧贴保持自然字距 (<= 4px)
    cv::Mat tightGapImg(100, 320, CV_8UC3, cv::Scalar(255, 255, 255));
    cv::rectangle(tightGapImg, cv::Rect(60, 40, 12, 16), ink, cv::FILLED); // 原单字 (ink: x=60..71)
    cv::rectangle(tightGapImg, cv::Rect(74, 40, 14, 16), ink, cv::FILLED); // 紧邻右字 (ink: x=74..87, 原字距 = 2px)
    TextReplaceRenderSpec tightGapSpec;
    tightGapSpec.sourceRect = cv::Rect(59, 39, 14, 18);
    tightGapSpec.originalTextUtf8 = "A";
    tightGapSpec.newTextUtf8 = "AB";
    tightGapSpec.bgMode = TextReplaceBgMode::Auto;
    tightGapSpec.reflowRightTail = true;
    tightGapSpec.rightTailRect = cv::Rect(74, 39, 15, 18);

    cv::Mat tightOut = tightGapImg.clone();
    TextReplaceEngine::renderReplacement(tightOut, tightGapSpec);
    // 扫描新渲染区和右移后尾巴之间的最大纯白空白列宽度：原字距 2px，顺延后不应膨胀成 > 4px 的尴尬大缝！
    int newInkMaxX = -1;
    int movedTailMinX = -1;
    std::vector<int> colDark(static_cast<size_t>(tightOut.cols), 0);
    for (int x = 55; x < 150; ++x) {
        for (int y = 36; y < 60; ++y) {
            const cv::Vec3b p = tightOut.at<cv::Vec3b>(y, x);
            if (p[0] < 160 && p[1] < 160 && p[2] < 160) ++colDark[static_cast<size_t>(x)];
        }
    }
    // 尾巴宽度为 14px，从右侧回扫找到尾巴最左墨迹列，再往左找新文字最右墨迹列
    int tailEnd = -1;
    for (int x = 149; x >= 55; --x) {
        if (colDark[static_cast<size_t>(x)] >= 8) { tailEnd = x; break; }
    }
    ASSERT_GT(tailEnd, 87);
    movedTailMinX = tailEnd - 13;
    for (int x = movedTailMinX - 1; x >= 55; --x) {
        if (colDark[static_cast<size_t>(x)] >= 2) { newInkMaxX = x; break; }
    }
    ASSERT_GT(newInkMaxX, 60);
    const int resultingGap = movedTailMinX - newInkMaxX - 1;
    EXPECT_GE(resultingGap, 1);
    EXPECT_LE(resultingGap, 4);
}

TEST(LiveTextOcrTest, HighResLargeFontAspectRatioPreservation) {
    auto& deep = tools3000::ocr::DeepOcrEngine::instance();
    if (!deep.getModelStatus().installed) {
        GTEST_SKIP() << "DeepOcrEngine models not installed";
    }

    // 模拟 2400 万像素手机原图（文字裁剪框高达 ~180px）：
    // 历史上 horizScale = max(1.05f, ...) 会将其横向拉扁 420%，把 "ZZ30003665" 错识成 "乙乙30003665"；
    // 修复为严格等比缩放后必须精准还原全部英文与数字。
    cv::Mat hiRes(520, 2200, CV_8UC3, cv::Scalar(244, 246, 248));
    cv::putText(hiRes, "ZZ30003665", cv::Point(120, 340),
                cv::FONT_HERSHEY_SIMPLEX, 5.4, cv::Scalar(28, 32, 36), 14, cv::LINE_AA);

    const auto lines = deep.extractText(hiRes);
    std::string joined;
    for (const auto& l : lines) joined += l.text;
    EXPECT_NE(joined.find("ZZ30003665"), std::string::npos) << "Recognized text was: " << joined;
}

TEST(LiveTextOcrTest, DeepOcrFragmentCoalescingAndInkEdgeRescue) {
    using namespace tools3000::ocr;

    // 1. 同一水平基线上被 DBNet 切碎的极小标点/短值碎片（模拟 "WddStatus" + ":" + "\"-\""）必须自动合并，
    //    同时跳过右侧独立对称 UI 下拉箭头图标，绝不把按钮旁的下拉箭头吸进标签框。
    cv::Mat canvas(80, 360, CV_8UC3, cv::Scalar(255, 255, 255));
    // 绘制紧邻按钮右侧的一个 9x5 对称向下下拉箭头 V（x=270..278, y=24..28）
    const cv::Vec3b chevCol(120, 120, 120);
    for (int k = 0; k < 5; ++k) {
        canvas.at<cv::Vec3b>(24 + k, 270 + k) = chevCol;
        canvas.at<cv::Vec3b>(24 + k, 278 - k) = chevCol;
    }

    auto makeRectQuad = [](float x, float y, float w, float h, float score = 0.95f) {
        DbDetectedQuad q;
        q.corners = {
            cv::Point2f(x, y),
            cv::Point2f(x + w, y),
            cv::Point2f(x + w, y + h),
            cv::Point2f(x, y + h),
        };
        q.score = score;
        return q;
    };

    std::vector<DbDetectedQuad> quads = {
        makeRectQuad(20.0f, 20.0f, 77.0f, 12.0f),   // "WddStatus"
        makeRectQuad(96.0f, 24.0f, 10.0f, 5.0f),    // ":"
        makeRectQuad(110.0f, 21.0f, 20.0f, 6.0f),   // "\"-\","
        makeRectQuad(220.0f, 18.0f, 38.0f, 18.0f),  // "保存"
        makeRectQuad(268.0f, 24.0f, 13.0f, 5.0f),   // 独立下拉箭头 v
    };
    DeepOcrEngine::coalesceSameLineTinyFragments(quads, canvas);
    ASSERT_EQ(quads.size(), 3u) << "Three JSON fragments should coalesce into 1 quad while chevron stays separate";
    EXPECT_NEAR(quads[0].corners[0].x, 20.0f, 0.5f);
    EXPECT_NEAR(quads[0].corners[1].x, 130.0f, 0.5f);
    EXPECT_NEAR(quads[1].corners[0].x, 220.0f, 0.5f);
    EXPECT_NEAR(quads[1].corners[1].x, 258.0f, 0.5f);

    // 2. 墨水感知防碰撞边界精修：DBNet 盒右缘漏掉 2px 外的逗号 `,` 且左缘漏掉 2px 外的 `{`，
    //    必须精准双向扩张包裹首尾标点，同时严格止步于相邻列框之前。
    cv::Mat jsonStrip(40, 260, CV_8UC3, cv::Scalar(255, 255, 255));
    const cv::Scalar darkInk(20, 20, 20);
    // 左缘外 2px 处的 `{`（x=36..38）
    cv::rectangle(jsonStrip, cv::Rect(36, 13, 3, 10), darkInk, cv::FILLED);
    // 主体文字（x=41..138）
    cv::rectangle(jsonStrip, cv::Rect(41, 14, 98, 9), darkInk, cv::FILLED);
    // 右缘外 2px 处的尾随逗号 `,`（x=141..143）
    cv::rectangle(jsonStrip, cv::Rect(141, 20, 3, 4), darkInk, cv::FILLED);
    // 右侧紧邻列（x=148..210）
    cv::rectangle(jsonStrip, cv::Rect(148, 14, 40, 9), darkInk, cv::FILLED);

    const std::vector<cv::Rect> allBoxes = {
        cv::Rect(40, 12, 100, 14),
        cv::Rect(147, 12, 50, 14),
    };
    cv::Vec3b estBg(0, 0, 0);
    bool uniformBg = false;
    const cv::Rect refined = DeepOcrEngine::refineAxisAlignedLineBox(
        jsonStrip, allBoxes[0], allBoxes, 0, &estBg, &uniformBg);
    EXPECT_TRUE(uniformBg);
    EXPECT_EQ(estBg, cv::Vec3b(255, 255, 255));
    EXPECT_LE(refined.x, 36) << "Must rescue leading '{' at x=36";
    EXPECT_GE(refined.x + refined.width, 144) << "Must rescue trailing ',' at x=141..143";
    EXPECT_LT(refined.x + refined.width, 147) << "Must never encroach onto neighbor box at x=147";
}

TEST(LiveTextOcrTest, DeepOcrDropdownChevronSuppressionPrecisionAndZeroFalsePositives) {
    using namespace tools3000::ocr;

    cv::Mat uiImg(120, 360, CV_8UC3, cv::Scalar(255, 255, 255));

    // 场景 A：绿色按钮 "GET" + 6px 空白 + 7x4 绿色对称下拉箭头（模拟 `GET v`）
    cv::rectangle(uiImg, cv::Rect(10, 10, 72, 24), cv::Scalar(235, 248, 238), cv::FILLED);
    const cv::Scalar greenInk(108, 185, 42);
    // 模拟 "GET" 三个满高字母（y=17..26, height=10）
    cv::rectangle(uiImg, cv::Rect(16, 17, 7, 10), greenInk, cv::FILLED);
    cv::rectangle(uiImg, cv::Rect(25, 17, 7, 10), greenInk, cv::FILLED);
    cv::rectangle(uiImg, cv::Rect(34, 17, 7, 10), greenInk, cv::FILLED);
    // 6px 空白后绘制 7x4 对称向下箭头（x=47..53, y=20..23, 居中悬浮）
    for (int k = 0; k < 4; ++k) {
        uiImg.at<cv::Vec3b>(20 + k, 47 + k) = cv::Vec3b(120, 196, 80);
        uiImg.at<cv::Vec3b>(20 + k, 53 - k) = cv::Vec3b(120, 196, 80);
    }

    // 场景 B："Body" 后紧跟浅灰 10x6 对称下拉箭头（模拟 `Body~`，即使前字高对比度微悬垂也不能屏蔽浅灰箭头）
    const cv::Scalar darkText(30, 35, 45);
    cv::rectangle(uiImg, cv::Rect(120, 16, 30, 12), darkText, cv::FILLED);
    for (int k = 0; k < 5; ++k) {
        uiImg.at<cv::Vec3b>(19 + k, 155 + k) = cv::Vec3b(200, 200, 200);
        uiImg.at<cv::Vec3b>(19 + k, 164 - k) = cv::Vec3b(200, 200, 200);
    }

    // 场景 C：独立下拉箭头块（模拟保存按钮旁独立 `√`）
    for (int k = 0; k < 5; ++k) {
        uiImg.at<cv::Vec3b>(20 + k, 220 + k) = cv::Vec3b(110, 110, 110);
        uiImg.at<cv::Vec3b>(20 + k, 228 - k) = cv::Vec3b(110, 110, 110);
    }

    // 场景 D（负例防误伤）：基线对齐的正常英文字母 "A v"（高 9px、宽 7px，高宽比不满足扁平下拉箭头）
    cv::rectangle(uiImg, cv::Rect(16, 62, 8, 11), darkText, cv::FILLED); // "A" (y=62..72)
    // 绘制正常窄高小写 'v'，底部落在基线 y=72（x=30..36, y=64..72, cw=7, ch=9）
    cv::line(uiImg, cv::Point(30, 64), cv::Point(33, 72), darkText, 1);
    cv::line(uiImg, cv::Point(36, 64), cv::Point(33, 72), darkText, 1);

    // 场景 E（负例防误伤）：非对称真实勾选符 "√"（左短右长，不满足左右镜像对称）
    cv::line(uiImg, cv::Point(120, 69), cv::Point(123, 73), darkText, 1);
    cv::line(uiImg, cv::Point(123, 73), cv::Point(130, 62), darkText, 1);

    OcrResult getBtn;
    getBtn.text = "GET v";
    getBtn.box = cv::Rect(14, 14, 44, 16);
    getBtn.charBoxes = {
        cv::Rect(16, 14, 8, 16), cv::Rect(25, 14, 8, 16), cv::Rect(34, 14, 8, 16),
        cv::Rect(42, 14, 4, 16), cv::Rect(47, 14, 8, 16),
    };

    OcrResult bodyBtn;
    bodyBtn.text = "Body~";
    bodyBtn.box = cv::Rect(118, 14, 50, 16);
    bodyBtn.charBoxes = {
        cv::Rect(120, 14, 8, 16), cv::Rect(128, 14, 8, 16),
        cv::Rect(136, 14, 7, 16), cv::Rect(143, 14, 8, 16), cv::Rect(154, 14, 12, 16),
    };

    OcrResult loneChev;
    loneChev.text = "\xE2\x88\x9A"; // √
    loneChev.box = cv::Rect(218, 18, 13, 9);
    loneChev.charBoxes = {loneChev.box};

    OcrResult realLetterV;
    realLetterV.text = "A v";
    realLetterV.box = cv::Rect(14, 60, 26, 15);
    realLetterV.charBoxes = {
        cv::Rect(16, 60, 8, 15), cv::Rect(24, 60, 5, 15), cv::Rect(29, 60, 9, 15),
    };

    OcrResult realCheckmark;
    realCheckmark.text = "\xE2\x88\x9A"; // √
    realCheckmark.box = cv::Rect(118, 60, 15, 15);
    realCheckmark.charBoxes = {realCheckmark.box};

    std::vector<OcrResult> blocks = {getBtn, bodyBtn, loneChev, realLetterV, realCheckmark};
    DeepOcrEngine::stripUiDropdownChevrons(uiImg, blocks);

    ASSERT_EQ(blocks.size(), 4u) << "Standalone dropdown chevron must be removed while real checkmark stays";
    EXPECT_EQ(blocks[0].text, "GET");
    EXPECT_EQ(blocks[0].charBoxes.size(), 3u);
    EXPECT_LT(blocks[0].box.width, getBtn.box.width);

    EXPECT_EQ(blocks[1].text, "Body");
    EXPECT_EQ(blocks[1].charBoxes.size(), 4u);

    EXPECT_EQ(blocks[2].text, "A v") << "Baseline-aligned real letter 'v' must never be stripped";
    EXPECT_EQ(blocks[2].charBoxes.size(), 3u);

    EXPECT_EQ(blocks[3].text, "\xE2\x88\x9A") << "Asymmetric real checkmark must never be stripped";
}

TEST(LiveTextOcrTest, DeepOcrStructuredJsonPunctuationNormalization) {
    using namespace tools3000::ocr;

    // 1. JSON 键值对含中文字符串值：仅规整 JSON 结构位上的全角引号/冒号/逗号及误识圆括号，
    //    严格保全中文值内部的中文逗号/冒号/括号，且 text 与 charBoxes 严格 1:1 保持长度同步。
    OcrResult jsonLine;
    jsonLine.text =
        "(\xE2\x80\x9C" "U_PLM_Categ1\xE2\x80\x9D\xEF\xBC\x9A\xE2\x80\x9C\xE5\x8D\x8A\xE6\x88\x90\xE5\x93\x81\xE2\x80\x9D\xEF\xBC\x8C"
        "\"User_Text\"\xEF\xBC\x9A\xE2\x80\x9C" "C\xE7\xB1\xBB\xE5\x8F\x97\xE6\x8E\xA7\xEF\xBC\x8C\xE5\x8E\x82\xE5\xAE\xB6\xEF\xBC\x9A\xE5\x8D\x97\xE9\x80\x9A\xE2\x80\x9D\xEF\xBC\x8C"
        "\"ItemPrices\":[(\"Currency\":\"RMB\",\"PriceList\":1)]}";
    const size_t origCpCount = OcrEngine::countUtf8Codepoints(jsonLine.text);
    jsonLine.charBoxes = OcrEngine::buildProportionalCharBoxes(jsonLine.text, cv::Rect(0, 0, 800, 16));
    ASSERT_EQ(jsonLine.charBoxes.size(), origCpCount);

    DeepOcrEngine::normalizeStructuredJsonPunctuation(jsonLine);

    const std::string expectedJson =
        "{\"U_PLM_Categ1\":\"\xE5\x8D\x8A\xE6\x88\x90\xE5\x93\x81\","
        "\"User_Text\":\"C\xE7\xB1\xBB\xE5\x8F\x97\xE6\x8E\xA7\xEF\xBC\x8C\xE5\x8E\x82\xE5\xAE\xB6\xEF\xBC\x9A\xE5\x8D\x97\xE9\x80\x9A\","
        "\"ItemPrices\":[{\"Currency\":\"RMB\",\"PriceList\":1}]}";
    EXPECT_EQ(jsonLine.text, expectedJson);
    EXPECT_EQ(OcrEngine::countUtf8Codepoints(jsonLine.text), origCpCount);
    EXPECT_EQ(jsonLine.charBoxes.size(), origCpCount);

    // 2. JSON 嵌套对象起始 `{` 被误识为 `f` 或 `(`（行尾 `"message": f` / 内嵌 `"message":f"lang":` / 数组 `[f"Currency":`）
    {
        OcrResult msgBrace;
        msgBrace.text = "\"message\": f";
        msgBrace.charBoxes = OcrEngine::buildProportionalCharBoxes(msgBrace.text, cv::Rect(0, 0, 120, 14));
        DeepOcrEngine::normalizeStructuredJsonPunctuation(msgBrace);
        EXPECT_EQ(msgBrace.text, "\"message\": {");
        EXPECT_EQ(msgBrace.charBoxes.size(), OcrEngine::countUtf8Codepoints(msgBrace.text));

        OcrResult inlineBrace;
        inlineBrace.text = "\"message\":f\"lang\":\"en-us\"}";
        inlineBrace.charBoxes = OcrEngine::buildProportionalCharBoxes(inlineBrace.text, cv::Rect(0, 0, 260, 14));
        DeepOcrEngine::normalizeStructuredJsonPunctuation(inlineBrace);
        EXPECT_EQ(inlineBrace.text, "\"message\":{\"lang\":\"en-us\"}");

        OcrResult arrBrace;
        arrBrace.text = "\"ItemPrices\":[f\"Currency\":\"RMB\"}]}";
        arrBrace.charBoxes = OcrEngine::buildProportionalCharBoxes(arrBrace.text, cv::Rect(0, 0, 320, 14));
        DeepOcrEngine::normalizeStructuredJsonPunctuation(arrBrace);
        EXPECT_EQ(arrBrace.text, "\"ItemPrices\":[{\"Currency\":\"RMB\"}]}");

        // 负例保护：布尔值 `"flag": false` 与单字符字符串 `"code": "f"` 严禁被误改！
        OcrResult boolFalse;
        boolFalse.text = "\"flag\": false, \"code\": \"f\"";
        DeepOcrEngine::normalizeStructuredJsonPunctuation(boolFalse);
        EXPECT_EQ(boolFalse.text, "\"flag\": false, \"code\": \"f\"");
    }

    // 3. 普通中文自然语言散文含中式引号/冒号/逗号：必须 100% 原样零改动！
    OcrResult chineseProse;
    chineseProse.text = "\xE2\x80\x9C\xE4\xBD\xA0\xE5\xA5\xBD\xE2\x80\x9D\xEF\xBC\x8C\xE5\xA5\xB9\xE8\xAF\xB4\xEF\xBC\x9A\xE2\x80\x9C\xE4\xBB\x8A\xE5\xA4\xA9\xE5\xA4\xA9\xE6\xB0\x94\xE5\xBE\x88\xE5\xA5\xBD\xE3\x80\x82\xE2\x80\x9D";
    const std::string proseBefore = chineseProse.text;
    DeepOcrEngine::normalizeStructuredJsonPunctuation(chineseProse);
    EXPECT_EQ(chineseProse.text, proseBefore);
}

TEST(LiveTextOcrTest, DeepOcrPixelUnderscoreRescueAndHyperlinkGuard) {
    using namespace tools3000::ocr;

    // 1. 正例：浅蓝选区背景上真实绘制 "bom_Make"（底部独立 6px 下划线），OCR 误输出 "bom Make" -> 精准还原 "bom_Make"
    cv::Mat img(120, 260, CV_8UC3, cv::Scalar(250, 232, 212)); // 浅蓝选中底色 (BGR)
    const cv::Scalar ink(35, 35, 35);
    // "bom" x=20..44, y=14..23; "_" x=46..52, y=25; "Make" x=54..84, y=14..23
    cv::rectangle(img, cv::Rect(20, 14, 7, 10), ink, cv::FILLED);
    cv::rectangle(img, cv::Rect(28, 16, 7, 8), ink, cv::FILLED);
    cv::rectangle(img, cv::Rect(36, 16, 8, 8), ink, cv::FILLED);
    cv::line(img, cv::Point(46, 25), cv::Point(51, 25), ink, 1);
    cv::rectangle(img, cv::Rect(54, 14, 8, 10), ink, cv::FILLED);
    cv::rectangle(img, cv::Rect(63, 16, 6, 8), ink, cv::FILLED);
    cv::rectangle(img, cv::Rect(70, 14, 6, 10), ink, cv::FILLED);
    cv::rectangle(img, cv::Rect(77, 16, 6, 8), ink, cv::FILLED);

    OcrResult missed;
    missed.text = "bom Make";
    missed.box = cv::Rect(18, 12, 68, 15);
    missed.charBoxes = {
        cv::Rect(20, 12, 7, 15), cv::Rect(28, 12, 7, 15), cv::Rect(36, 12, 8, 15),
        cv::Rect(45, 12, 8, 15),
        cv::Rect(54, 12, 8, 15), cv::Rect(63, 12, 6, 15), cv::Rect(70, 12, 6, 15), cv::Rect(77, 12, 6, 15),
    };
    DeepOcrEngine::rescueMissedUnderscores(img, missed);
    EXPECT_EQ(missed.text, "bom_Make");
    EXPECT_EQ(missed.charBoxes.size(), 8u);

    // 2. 负例 A：正常英文词间空格 "foo bar"（底部无笔画）-> 保持原样
    cv::rectangle(img, cv::Rect(20, 48, 22, 10), ink, cv::FILLED);
    cv::rectangle(img, cv::Rect(52, 48, 22, 10), ink, cv::FILLED);
    OcrResult normalSpace;
    normalSpace.text = "foo bar";
    normalSpace.box = cv::Rect(18, 46, 58, 15);
    normalSpace.charBoxes = {
        cv::Rect(20, 46, 7, 15), cv::Rect(27, 46, 7, 15), cv::Rect(34, 46, 8, 15),
        cv::Rect(43, 46, 8, 15),
        cv::Rect(52, 46, 7, 15), cv::Rect(59, 46, 7, 15), cv::Rect(66, 46, 8, 15),
    };
    DeepOcrEngine::rescueMissedUnderscores(img, normalSpace);
    EXPECT_EQ(normalSpace.text, "foo bar");

    // 3. 负例 B：带通长下划线的超链接 "foo bar"（左右两翼均延续贯穿下划线）-> 反超链接护栏拦截，100% 保持空格
    cv::rectangle(img, cv::Rect(20, 82, 22, 10), ink, cv::FILLED);
    cv::rectangle(img, cv::Rect(52, 82, 22, 10), ink, cv::FILLED);
    cv::line(img, cv::Point(20, 93), cv::Point(74, 93), ink, 1);
    OcrResult hyperlink;
    hyperlink.text = "foo bar";
    hyperlink.box = cv::Rect(18, 80, 58, 15);
    hyperlink.charBoxes = {
        cv::Rect(20, 80, 7, 15), cv::Rect(27, 80, 7, 15), cv::Rect(34, 80, 8, 15),
        cv::Rect(43, 80, 8, 15),
        cv::Rect(52, 80, 7, 15), cv::Rect(59, 80, 7, 15), cv::Rect(66, 80, 8, 15),
    };
    DeepOcrEngine::rescueMissedUnderscores(img, hyperlink);
    EXPECT_EQ(hyperlink.text, "foo bar") << "Full-width hyperlink underline must never turn a word space into underscore";
}

TEST(LiveTextOcrTest, OcrEngineSoftWrappedJsonSeamlessJoinAndGuards) {
    using namespace tools3000::ocr;

    // 1. 压缩单行 JSON 在中文字符串中间软折行：拼接后绝不能插入 \r\n 破坏 RFC 8259 字符串语法
    std::vector<OcrResult> wrappedJson;
    auto pushRow = [&](int x, int y, int w, int h, const char* txt) {
        OcrResult r;
        r.text = txt;
        r.box = cv::Rect(x, y, w, h);
        r.confidence = 0.96f;
        wrappedJson.push_back(std::move(r));
    };
    pushRow(70, 184, 880, 15, "{\"U_PLM_Categ2\":\"BCP0300\",\"U_YF_Project\":\"TXZW-WH003\",");
    pushRow(70, 200, 940, 15, "\"ItemName\":\"KT154-F19 \xE6\xB5\x87");
    pushRow(70, 216, 940, 14, "\xE5\xB0\x81\xE9\x94\x82\xE7\x94\xB5\xE6\xB1\xA0\xE7\xBB\x84\",\"User_Text\":\"\xE5\x8D\x97\xE9\x80\x9A\xE5\x8D\x97\xE5\x8D\x93\xE7\x94\xB5");
    pushRow(70, 232, 940, 15, "\xE5\xAD\x90\xE7\xA7\x91\xE6\x8A\x80\xE6\x9C\x89\xE9\x99\x90\xE5\x85\xAC\xE5\x8F\xB8\",\"U_financial_type\":\"\xE8\x87\xAA\xE7\xA0\x94\xE4\xBA\xA7");
    pushRow(70, 248, 930, 14, "\xE5\x93\x81\",\"ItemsGroupCode\":104,\"ItemPrices\":[{\"Currency\":\"RMB\",\"PriceList\":1}]}");

    const std::string joined = OcrEngine::orderTextBlocksWithXYCut(wrappedJson, "\r\n");
    const auto parsed = nlohmann::json::parse(joined);
    EXPECT_EQ(parsed.at("U_PLM_Categ2").get<std::string>(), "BCP0300");
    EXPECT_EQ(parsed.at("ItemName").get<std::string>(), "KT154-F19 \xE6\xB5\x87\xE5\xB0\x81\xE9\x94\x82\xE7\x94\xB5\xE6\xB1\xA0\xE7\xBB\x84");
    EXPECT_EQ(parsed.at("User_Text").get<std::string>(), "\xE5\x8D\x97\xE9\x80\x9A\xE5\x8D\x97\xE5\x8D\x93\xE7\x94\xB5\xE5\xAD\x90\xE7\xA7\x91\xE6\x8A\x80\xE6\x9C\x89\xE9\x99\x90\xE5\x85\xAC\xE5\x8F\xB8");
    EXPECT_EQ(parsed.at("U_financial_type").get<std::string>(), "\xE8\x87\xAA\xE7\xA0\x94\xE4\xBA\xA7\xE5\x93\x81");
    EXPECT_EQ(parsed.at("ItemsGroupCode").get<int>(), 104);
    ASSERT_EQ(parsed.at("ItemPrices").size(), 1u);
    EXPECT_EQ(parsed.at("ItemPrices")[0].at("Currency").get<std::string>(), "RMB");

    // 2. 长 URL 软折行：smartMergeParagraphs 也必须无缝直连（绝不插入空格把 URL 拆断）
    const std::string mergedUrl = OcrEngine::smartMergeParagraphsUtf8(
        "\"@odata.context\": \"https://example.com/b1s/\n    v2/view.svc/$metadata\",");
    EXPECT_EQ(mergedUrl, "\"@odata.context\": \"https://example.com/b1s/v2/view.svc/$metadata\",");

    // 3. 常规格式化多行 JSON：每行字符串均已闭合，必须完整保留换行
    const cv::Rect r1(100, 100, 150, 14);
    const cv::Rect r2(100, 116, 150, 14);
    EXPECT_FALSE(OcrEngine::shouldSeamlessJoinSoftWrappedJsonLine(
        "\"DocEntry\" : 90411,", "\"CANCELED\" : \"N\",", &r1, &r2));

    // 4. 防呆护栏 A：上一行漏识闭合引号，而下一行直接以新的 JSON 键 `"Age":` 开头 -> 严禁误合并
    EXPECT_FALSE(OcrEngine::shouldSeamlessJoinSoftWrappedJsonLine(
        "{\"Name\": \"Alice", "\"Age\": 30}", &r1, &r2));

    // 5. 防呆护栏 B：几何跨分栏（右侧新栏顶部）或垂直大间隔 -> 严禁误合并
    const cv::Rect rightColTop(600, 100, 150, 14);
    EXPECT_FALSE(OcrEngine::shouldSeamlessJoinSoftWrappedJsonLine(
        "{\"Name\": \"Alice", "Smith\"}", &r1, &rightColTop));
    const cv::Rect farBelow(100, 260, 150, 14);
    EXPECT_FALSE(OcrEngine::shouldSeamlessJoinSoftWrappedJsonLine(
        "{\"Name\": \"Alice", "Smith\"}", &r1, &farBelow));

    // 6. 防呆护栏 C：普通非 JSON 文本偶发未闭合引号 -> 严禁误合并
    EXPECT_FALSE(OcrEngine::shouldSeamlessJoinSoftWrappedJsonLine(
        "She said \"Hello world", "Next paragraph line", &r1, &r2));

    // 7. 通用长 URL 与 Windows / UNC 文件路径软折行无缝续接及防误粘护栏
    EXPECT_TRUE(OcrEngine::shouldSeamlessJoinSoftWrappedJsonLine(
        "https://221.219.244.245:52400/b1s/", "v2/view.svc/$metadata", &r1, &r2));
    EXPECT_FALSE(OcrEngine::shouldSeamlessJoinSoftWrappedJsonLine(
        "https://example.com/first/", "https://example.com/second", &r1, &r2));
    EXPECT_TRUE(OcrEngine::shouldSeamlessJoinSoftWrappedJsonLine(
        "C:\\repo\\Tools3000\\build\\", "bin\\Release\\Tools3000.exe", &r1, &r2));
    EXPECT_TRUE(OcrEngine::shouldSeamlessJoinSoftWrappedJsonLine(
        "\\\\192.168.1.150\\b1_shr\\", "attachments\\order_2026.pdf", &r1, &r2));
    EXPECT_FALSE(OcrEngine::shouldSeamlessJoinSoftWrappedJsonLine(
        "C:\\repo\\Tools3000\\", "D:\\other\\file.txt", &r1, &r2));
}

TEST(LiveTextOcrTest, OcrEngineOneDimensionalBarcodeDecodingWithFormat) {
    using namespace tools3000::ocr;

    // 按 ISO/IEC 15420 规范精确编码生成标准 EAN-13 条形码 "6901234567892"
    static constexpr const char* kL[10] = {
        "0001101", "0011001", "0010011", "0111101", "0100011",
        "0110001", "0101111", "0111011", "0110111", "0001011"
    };
    static constexpr const char* kG[10] = {
        "0100111", "0110011", "0011011", "0100001", "0011101",
        "0111001", "0000101", "0010001", "0001001", "0010111"
    };
    static constexpr const char* kR[10] = {
        "1110010", "1100110", "1101100", "1000010", "1011100",
        "1001110", "1010000", "1000100", "1001000", "1110100"
    };
    // 首数字 '6' 对应的左 6 位奇偶校验模板: L G G G L L
    const char* kParity6 = "LGGGLL";
    const int digits[13] = {6, 9, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 2};

    std::string bits = "101";
    for (int i = 1; i <= 6; ++i) {
        bits += (kParity6[i - 1] == 'L') ? kL[digits[i]] : kG[digits[i]];
    }
    bits += "01010";
    for (int i = 7; i <= 12; ++i) {
        bits += kR[digits[i]];
    }
    bits += "101";
    ASSERT_EQ(bits.size(), 95u);

    constexpr int kModuleW = 4;
    constexpr int kQuietX = 48;
    constexpr int kBarH = 120;
    constexpr int kPadY = 44;
    const int imgW = kQuietX * 2 + static_cast<int>(bits.size()) * kModuleW;
    const int imgH = kPadY * 2 + kBarH;

    cv::Mat barImg(imgH, imgW, CV_8UC3, cv::Scalar(255, 255, 255));
    for (size_t i = 0; i < bits.size(); ++i) {
        if (bits[i] == '1') {
            cv::rectangle(barImg,
                          cv::Rect(kQuietX + static_cast<int>(i) * kModuleW, kPadY, kModuleW, kBarH),
                          cv::Scalar(0, 0, 0), cv::FILLED);
        }
    }

    const auto codes = OcrEngine::detectQrCodes(barImg);
    bool foundEan13 = false;
    for (const auto& c : codes) {
        if (c.text == "6901234567892") {
            foundEan13 = true;
            EXPECT_NE(c.format.find("EAN_13"), std::string::npos) << c.format;
            EXPECT_GT(c.boundingBox.width, 100);
            EXPECT_GT(c.boundingBox.height, 40);
        }
    }
    EXPECT_TRUE(foundEan13) << "OcrEngine::detectQrCodes must decode tightly-cropped EAN-13 1D barcode";

    // 2. 大尺寸整页单据中局部嵌入 EAN-13 条形码：自动定位并解码
    cv::Mat pageImg(768, 1024, CV_8UC3, cv::Scalar(248, 248, 248));
    for (size_t i = 0; i < bits.size(); ++i) {
        if (bits[i] == '1') {
            cv::rectangle(pageImg,
                          cv::Rect(620 + static_cast<int>(i) * 3, 500, 3, 90),
                          cv::Scalar(0, 0, 0), cv::FILLED);
        }
    }
    const auto pageCodes = OcrEngine::detectQrCodes(pageImg);
    bool foundPageEan13 = false;
    for (const auto& c : pageCodes) {
        if (c.text == "6901234567892") {
            foundPageEan13 = true;
            EXPECT_NE(c.format.find("EAN_13"), std::string::npos) << c.format;
        }
    }
    EXPECT_TRUE(foundPageEan13) << "OcrEngine::detectQrCodes must locate and decode embedded EAN-13 barcode on full document page";
}

namespace {

std::filesystem::path resolveRealWorldOcrFixtureDir() {
    auto probeFrom = [](std::filesystem::path base) -> std::filesystem::path {
        std::error_code ec;
        for (int depth = 0; depth < 8 && !base.empty(); ++depth) {
            const auto candidate = base / "tests" / "fixtures" / "ocr";
            if (std::filesystem::is_regular_file(candidate / "README.md", ec)) {
                return candidate;
            }
            const auto parent = base.parent_path();
            if (parent == base) break;
            base = parent;
        }
        return {};
    };

    wchar_t exePathBuf[MAX_PATH]{};
    const DWORD exeLen = GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
    if (exeLen > 0 && exeLen < MAX_PATH) {
        if (auto dir = probeFrom(std::filesystem::path(exePathBuf).parent_path()); !dir.empty()) {
            return dir;
        }
    }
    std::error_code ec;
    return probeFrom(std::filesystem::current_path(ec));
}

cv::Mat loadRealWorldOcrFixture(const char* fileName) {
    const auto dir = resolveRealWorldOcrFixtureDir();
    if (dir.empty()) return {};
    const auto fullPath = dir / fileName;
    return cv::imread(tools3000::core::WinUtils::wstringToUtf8(fullPath.wstring()), cv::IMREAD_COLOR);
}

std::string joinOcrLines(const std::vector<tools3000::ocr::OcrResult>& lines) {
    std::string out;
    for (const auto& r : lines) {
        out += r.text;
        out.push_back('\n');
    }
    return out;
}

bool containsLineToken(const std::vector<tools3000::ocr::OcrResult>& lines, std::string_view needle) {
    for (const auto& r : lines) {
        if (r.text.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

TEST(LiveTextOcrTest, RealWorldOcrFixtureCatalogIntegrity) {
    const auto dir = resolveRealWorldOcrFixtureDir();
    ASSERT_FALSE(dir.empty()) << "Failed to locate tests/fixtures/ocr directory from executable or working dir";

    constexpr const char* kFixtures[] = {
        "sap_form_table.jpg",
        "release_notes_dense_doc.png",
        "pda_screen_photo.jpg",
        "meeting_tv_photo.jpg",
        "erp_table_screen_photo.jpg",
        "packing_card_photo.jpg",
        "apifox_json_pretty.png",
        "apifox_json_compact.jpg",
    };

    for (const char* name : kFixtures) {
        const auto p = dir / name;
        std::error_code ec;
        ASSERT_TRUE(std::filesystem::is_regular_file(p, ec)) << "Missing OCR regression fixture: " << name;
        const cv::Mat img = cv::imread(tools3000::core::WinUtils::wstringToUtf8(p.wstring()), cv::IMREAD_COLOR);
        ASSERT_FALSE(img.empty()) << "Failed to decode OCR regression fixture: " << name;
        EXPECT_GT(img.cols, 200) << name;
        EXPECT_GT(img.rows, 200) << name;
    }
}

TEST(LiveTextOcrTest, TranslateFixtureCatalogIntegrity) {
    const auto ocrDir = resolveRealWorldOcrFixtureDir();
    ASSERT_FALSE(ocrDir.empty());
    const auto trDir = ocrDir.parent_path() / "translate";
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::is_directory(trDir, ec)) << "Missing tests/fixtures/translate directory";

    constexpr const char* kTranslateFixtures[] = {
        "dark_sidebar_list.png",
        "nytimes_homepage.png",
        "yahoo_portal_cards.png",
        "sap_b1_form_designer.png",
        "hackernews_nested_comments.png",
        "alternativeto_web_grid.png",
        "dark_markdown_codebadges.png",
        "gnway_admin_dialog.jpg",
        "packing_card_handheld.jpg",
        "postman_api_dark_sanitized.png",
    };

    for (const char* name : kTranslateFixtures) {
        const auto p = trDir / name;
        ASSERT_TRUE(std::filesystem::is_regular_file(p, ec)) << "Missing translate regression fixture: " << name;
        const cv::Mat img = cv::imread(tools3000::core::WinUtils::wstringToUtf8(p.wstring()), cv::IMREAD_COLOR);
        ASSERT_FALSE(img.empty()) << "Failed to decode translate regression fixture: " << name;
        EXPECT_GT(img.cols, 200) << name;
        EXPECT_GT(img.rows, 200) << name;
    }
}

TEST(LiveTextOcrTest, Full18FixturesOcrAccuracyLatencyMemoryBenchmark) {
    const char* runEnv = std::getenv("TOOLS3000_RUN_OCR_18_BENCHMARK");
    if (!runEnv || !*runEnv || std::string(runEnv) == "0") {
        GTEST_SKIP() << "Set TOOLS3000_RUN_OCR_18_BENCHMARK=1 to run full 18-fixture OCR benchmark";
    }

    using namespace tools3000::ocr;
    const auto ocrDir = resolveRealWorldOcrFixtureDir();
    ASSERT_FALSE(ocrDir.empty());
    const auto fixturesRoot = ocrDir.parent_path();

    struct FixtureSpec {
        const char* relPath;
        const char* category;
        std::vector<const char*> anchors; // 核心 Ground-Truth 锚点词（不区分大小写可选，此处严格或小写子串匹配）
        int expectedMinQr = 0;
    };

    const std::vector<FixtureSpec> specs = {
        // 8 张 OCR 实景回归集
        {"ocr/sap_form_table.jpg", "OCR-ERP多栏表",
         {"G16611", "JX0200-00680", "62,376.00", "RMB", "物料", "数量"}, 0},
        {"ocr/release_notes_dense_doc.png", "OCR-高密中英文档",
         {"本次版本核心亮点", "Direct2D", "Tools3000-Setup.exe", "OCR", "WinML", "Windows"}, 0},
        {"ocr/pda_screen_photo.jpg", "OCR-PDA斜拍屏摄",
         {"PL202610150024", "周转箱编码", "ZZ30003665", "生产箱单"}, 0},
        {"ocr/meeting_tv_photo.jpg", "OCR-会议大屏实拍",
         {"SAP待解决问题", "5%", "开票", "税务局"}, 0},
        {"ocr/erp_table_screen_photo.jpg", "OCR-ERP摩尔纹表",
         {"物料名称", "仓库名称", "数量", "ZZ00443481", "自制落料片仓库"}, 0},
        {"ocr/packing_card_photo.jpg", "OCR-贴纸装箱卡+QR",
         {"瑞鹄浩博产品装箱卡", "501010567AA", "右前纵梁后部加强板", "ZZ30001332"}, 1},
        {"ocr/apifox_json_pretty.png", "OCR-Apifox格式化JSON",
         {"DocTotalFC", "Comments", "U_BillType", "WddStatus", "90411", "69355.20"}, 0},
        {"ocr/apifox_json_compact.jpg", "OCR-Apifox超宽单行JSON",
         {"U_PLM_Categ2", "BCP0300", "ItemPrices", "RMB", "PriceList", "Internal error"}, 0},

        // 10 张新增原位翻译全场景原图集（8 张已升级为 2K/1080p 原生无损高清原图）
        {"translate/dark_sidebar_list.png", "TR-暗色双语侧边栏",
         {"Pinned Conversations", "Tools3000", "SAP_B1_10.0", "民间借贷纠纷法律咨询", "Subproject Migration", "SSL Certificate Renewal Failure"}, 0},
        {"translate/nytimes_homepage.png", "TR-NYTimes多栏报纸",
         {"Business", "Lifestyle", "Alice Huang", "Nvidia", "Democrats", "House of Representatives"}, 0},
        {"translate/yahoo_portal_cards.png", "TR-Yahoo卡片+大图",
         {"Search the web", "Trending", "Alaska", "Seattle", "Los Angeles", "Advertisement"}, 0},
        {"translate/sap_b1_form_designer.png", "TR-SAP高密小字ERP",
         {"平台注册", "基本设置", "单据对象控制", "自定义窗口", "字段权限", "624"}, 0},
        {"translate/hackernews_nested_comments.png", "TR-HN多级嵌套评论",
         {"Hacker News", "Show HN", "LLM", "reply", "add comment", "factory"}, 0},
        {"translate/alternativeto_web_grid.png", "TR-AlternativeTo网格",
         {"AlternativeTo", "Planner", "NGINX", "PostgreSQL", "Cryptomator", "More tech news"}, 0},
        {"translate/dark_markdown_codebadges.png", "TR-暗色Markdown胶囊",
         {"侧边栏信息架构", "MECE", "App.tsx", "standardPluginIds", "Sidebar.tsx", "OCR"}, 0},
        {"translate/gnway_admin_dialog.jpg", "TR-Win32管理端弹窗",
         {"V8.6.0.724", "用户管理", "安全配置", "AS0002", "创建新用户失败", "确定"}, 0},
        // 注：该手持实拍图二维码仅 ~85px 且左下定位点受强反光物理缺损，不作 QR 强门禁（QR 硬门禁由 ocr/packing_card_photo.jpg 守护）
        {"translate/packing_card_handheld.jpg", "TR-手持装箱卡+强反光QR",
         {"瑞鹄浩博产品装箱卡", "ZZ00445707", "501023856AA", "HB0502000441", "奇瑞T1Q项目", "原材料箱"}, 0},
        {"translate/postman_api_dark_sanitized.png", "TR-暗色Postman脱敏",
         {"COLLECTIONS", "BaseUrl", "CmpnyDB", "DEMO_SAP", "Demo@24Pass", "UserObjectsMD"}, 0},
    };

    auto queryMem = []() -> std::tuple<double, double, double> {
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (::K32GetProcessMemoryInfo(::GetCurrentProcess(),
                                      reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                      sizeof(pmc))) {
            constexpr double kMb = 1024.0 * 1024.0;
            return {static_cast<double>(pmc.PrivateUsage) / kMb,
                    static_cast<double>(pmc.WorkingSetSize) / kMb,
                    static_cast<double>(pmc.PeakWorkingSetSize) / kMb};
        }
        return {0.0, 0.0, 0.0};
    };

    auto queryCpuMs = []() -> double {
        FILETIME ct{}, et{}, kt{}, ut{};
        if (::GetProcessTimes(::GetCurrentProcess(), &ct, &et, &kt, &ut)) {
            ULARGE_INTEGER ku{}, uu{};
            ku.LowPart = kt.dwLowDateTime; ku.HighPart = kt.dwHighDateTime;
            uu.LowPart = ut.dwLowDateTime; uu.HighPart = ut.dwHighDateTime;
            return static_cast<double>(ku.QuadPart + uu.QuadPart) / 10000.0;
        }
        return 0.0;
    };

    auto toLowerAscii = [](std::string s) {
        for (char& ch : s) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        }
        return s;
    };

    // 统计英文疑似漏空格粘连词（连续 >=18 个纯 ASCII 字母且非驼峰/全大写常量/含点斜杠下划线）
    auto countGluedAsciiTokens = [](const std::vector<OcrResult>& lines) -> int {
        int glued = 0;
        for (const auto& ln : lines) {
            size_t i = 0;
            while (i < ln.text.size()) {
                while (i < ln.text.size() && std::isspace(static_cast<unsigned char>(ln.text[i]))) ++i;
                const size_t start = i;
                while (i < ln.text.size() && !std::isspace(static_cast<unsigned char>(ln.text[i]))) ++i;
                const std::string_view tok = std::string_view(ln.text).substr(start, i - start);
                if (tok.size() < 18) continue;
                bool pureAlpha = true;
                size_t upperCnt = 0, lowerCnt = 0;
                for (unsigned char c : tok) {
                    if (c >= 'A' && c <= 'Z') ++upperCnt;
                    else if (c >= 'a' && c <= 'z') ++lowerCnt;
                    else { pureAlpha = false; break; }
                }
                if (pureAlpha && lowerCnt >= 12 && upperCnt <= 3) {
                    ++glued;
                }
            }
        }
        return glued;
    };

    auto& deep = DeepOcrEngine::instance();
    deep.releaseLoadedModels();
    tools3000::core::WinUtils::trimWorkingSet();
    Sleep(150);

    const auto [basePriv, baseWs, basePeakWs] = queryMem();
    std::ofstream detailFile;
    const char* detailPath = std::getenv("TOOLS3000_OCR_BENCH_DETAIL_OUT");
    if (detailPath && *detailPath) {
        detailFile.open(detailPath, std::ios::out | std::ios::trunc);
    }

    std::cout << "[OCR-BENCH-BASE] priv=" << basePriv << "MB ws=" << baseWs << "MB peakWs=" << basePeakWs << "MB\n";
    std::cout << "[OCR-BENCH-TABLE] file | size | engine | wallMs | cpuMs | lines | blocks | chars | qr | avgConf | p10Conf | low(<0.75) | anchors | gluedEn | privMB | wsMB\n";

    double totalWarmWallMs = 0.0;
    double totalWarmCpuMs = 0.0;
    size_t totalLines = 0;
    size_t totalChars = 0;
    size_t totalAnchorsHit = 0;
    size_t totalAnchorsAll = 0;
    double maxPrivMb = basePriv;
    double maxWsMb = baseWs;

    for (size_t idx = 0; idx < specs.size(); ++idx) {
        const auto& sp = specs[idx];
        const auto fullPath = fixturesRoot / sp.relPath;
        const cv::Mat img = cv::imread(tools3000::core::WinUtils::wstringToUtf8(fullPath.wstring()), cv::IMREAD_COLOR);
        ASSERT_FALSE(img.empty()) << "Failed to load: " << sp.relPath;

        // 第 1 张图同时记录冷启耗时，再对副本左上角 1px 做 1-bit 无感扰动绕开 LRU 缓存以精确测量热态无缓存真实耗时
        double coldMs = -1.0;
        cv::Mat warmImg = img;
        if (idx == 0) {
            const auto t0Cold = std::chrono::steady_clock::now();
            const auto coldPage = OcrEngine::instance().extractPage(img);
            const auto t1Cold = std::chrono::steady_clock::now();
            coldMs = std::chrono::duration<double, std::milli>(t1Cold - t0Cold).count();
            const auto [cPriv, cWs, cPeak] = queryMem();
            std::cout << "[OCR-BENCH-COLD] first=" << sp.relPath << " coldWallMs=" << coldMs
                      << " engine=" << coldPage.activeEngine
                      << " priv=" << cPriv << "MB ws=" << cWs << "MB peakWs=" << cPeak << "MB\n";
            warmImg = img.clone();
            warmImg.at<cv::Vec3b>(0, 0)[0] ^= 1;
        }

        const double cpuBefore = queryCpuMs();
        const auto t0 = std::chrono::steady_clock::now();
        const auto page = OcrEngine::instance().extractPage(warmImg);
        const auto t1 = std::chrono::steady_clock::now();
        const double wallMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double cpuMs = queryCpuMs() - cpuBefore;
        const auto [privMb, wsMb, peakWsMb] = queryMem();
        maxPrivMb = (std::max)(maxPrivMb, privMb);
        maxWsMb = (std::max)(maxWsMb, wsMb);

        const auto atomic = OcrEngine::splitIntoAtomicBlocks(page.lines);
        std::vector<float> confs;
        confs.reserve(page.lines.size());
        size_t chars = 0;
        size_t lowConfCnt = 0;
        double confSum = 0.0;
        for (const auto& ln : page.lines) {
            confs.push_back(ln.confidence);
            confSum += ln.confidence;
            if (ln.confidence < 0.75f) ++lowConfCnt;
            chars += OcrEngine::countUtf8Codepoints(ln.text);
        }
        double avgConf = confs.empty() ? 0.0 : (confSum / static_cast<double>(confs.size()));
        float p10Conf = 0.0f;
        if (!confs.empty()) {
            std::sort(confs.begin(), confs.end());
            p10Conf = confs[confs.size() / 10];
        }

        const std::string allTextLower = toLowerAscii(joinOcrLines(page.lines));
        size_t hitCnt = 0;
        std::string missedAnchors;
        for (const char* a : sp.anchors) {
            if (allTextLower.find(toLowerAscii(a)) != std::string::npos) {
                ++hitCnt;
            } else {
                if (!missedAnchors.empty()) missedAnchors += ", ";
                missedAnchors += a;
            }
        }
        const int gluedEn = countGluedAsciiTokens(page.lines);

        totalWarmWallMs += wallMs;
        totalWarmCpuMs += cpuMs;
        totalLines += page.lines.size();
        totalChars += chars;
        totalAnchorsHit += hitCnt;
        totalAnchorsAll += sp.anchors.size();

        std::printf("  [%02zu] %-36s | %4dx%-4d | %-8s | %6.1f | %6.1f | %3zu | %3zu | %4zu | %zu | %.3f | %.3f | %2zu | %zu/%zu | %2d | %5.1f | %5.1f\n",
                    idx + 1, sp.relPath, img.cols, img.rows, page.activeEngine.c_str(),
                    wallMs, cpuMs, page.lines.size(), atomic.size(), chars, page.qrCodes.size(),
                    avgConf, p10Conf, lowConfCnt, hitCnt, sp.anchors.size(), gluedEn, privMb, wsMb);

        if (detailFile.is_open()) {
            detailFile << "=== [" << (idx + 1) << "] " << sp.relPath << " (" << sp.category << ") "
                       << img.cols << "x" << img.rows << " wall=" << wallMs << "ms cpu=" << cpuMs
                       << "ms lines=" << page.lines.size() << " avgConf=" << avgConf
                       << " anchors=" << hitCnt << "/" << sp.anchors.size();
            if (!missedAnchors.empty()) detailFile << " MISSED=[" << missedAnchors << "]";
            detailFile << " ===\n";
            for (size_t k = 0; k < page.lines.size(); ++k) {
                const auto& ln = page.lines[k];
                detailFile << "  L" << k << " (" << ln.confidence << ") [" << ln.text << "]\n";
            }
            for (const auto& q : page.qrCodes) {
                detailFile << "  [QR:" << q.format << "] " << q.text << "\n";
            }
            detailFile << "\n";
        }

        EXPECT_FALSE(page.lines.empty()) << sp.relPath;
        if (sp.expectedMinQr > 0) {
            EXPECT_GE(static_cast<int>(page.qrCodes.size()), sp.expectedMinQr) << sp.relPath;
        }
    }

    const auto [beforeRelPriv, beforeRelWs, peakWsAll] = queryMem();
    deep.releaseLoadedModels();
    tools3000::core::WinUtils::trimWorkingSet();
    Sleep(250);
    const auto [afterRelPriv, afterRelWs, afterPeakWs] = queryMem();

    std::printf("[OCR-BENCH-SUMMARY] images=%zu totalWarmWall=%.1fms avgWall=%.1fms totalCpu=%.1fms totalLines=%zu totalChars=%zu anchors=%zu/%zu (%.1f%%)\n",
                specs.size(), totalWarmWallMs, totalWarmWallMs / static_cast<double>(specs.size()),
                totalWarmCpuMs, totalLines, totalChars, totalAnchorsHit, totalAnchorsAll,
                totalAnchorsAll > 0 ? (100.0 * static_cast<double>(totalAnchorsHit) / static_cast<double>(totalAnchorsAll)) : 0.0);
    std::printf("[OCR-BENCH-MEM] base=(priv=%.1fMB ws=%.1fMB) peak=(priv=%.1fMB ws=%.1fMB peakWs=%.1fMB) afterRelease=(priv=%.1fMB ws=%.1fMB)\n",
                basePriv, baseWs, maxPrivMb, maxWsMb, peakWsAll, afterRelPriv, afterRelWs);
}

TEST(LiveTextOcrTest, FixtureApifoxPrettyJsonAndDropdownChevronRegression) {
    using namespace tools3000::ocr;
    auto& deep = DeepOcrEngine::instance();
    if (!deep.getModelStatus().installed) {
        GTEST_SKIP() << "PP-OCRv4 offline model bundle not installed";
    }

    const cv::Mat img = loadRealWorldOcrFixture("apifox_json_pretty.png");
    ASSERT_FALSE(img.empty());

    const auto page = OcrEngine::instance().extractPage(img);
    EXPECT_EQ(page.activeEngine, "ppocr_v4");
    ASSERT_GE(page.lines.size(), 35u);

    const std::string allText = joinOcrLines(page.lines);
    EXPECT_NE(allText.find("\"DocTotalFC\" : 0.0,"), std::string::npos) << allText;
    EXPECT_NE(allText.find("\"Comments\" : \"\","), std::string::npos) << allText;
    EXPECT_NE(allText.find("\"U_BillType\" : \"Y\","), std::string::npos) << allText;
    EXPECT_NE(allText.find("\"WddStatus\" : \"-\","), std::string::npos) << allText;
    EXPECT_NE(allText.find("\"DocEntry\" : 90411,"), std::string::npos) << allText;
    EXPECT_NE(allText.find("\"DocTotal\" : 69355.20,"), std::string::npos) << allText;
    EXPECT_NE(allText.find("\"CardName\":\"北京赛泰克生物科技有限公司\","), std::string::npos) << allText;
    EXPECT_NE(allText.find("$filter"), std::string::npos) << allText;
    EXPECT_NE(allText.find("$orderby"), std::string::npos) << allText;
    EXPECT_NE(allText.find("$skip"), std::string::npos) << allText;
    EXPECT_NE(allText.find("1000000"), std::string::npos) << allText;

    // UI 下拉箭头剥离防回退断言：GET / Body / Raw / utf8 不应夹带下拉箭头误识尾缀
    EXPECT_TRUE(containsLineToken(page.lines, "GET")) << allText;
    EXPECT_EQ(allText.find("GET v"), std::string::npos) << "Dropdown chevron next to GET leaked into OCR text";
    EXPECT_EQ(allText.find("Body~"), std::string::npos) << "Dropdown chevron next to Body leaked into OCR text";
    EXPECT_EQ(allText.find("Raw v"), std::string::npos) << "Dropdown chevron next to Raw leaked into OCR text";

    for (const auto& blk : page.lines) {
        EXPECT_EQ(blk.charBoxes.size(), OcrEngine::countUtf8Codepoints(blk.text)) << blk.text;
    }
}

TEST(LiveTextOcrTest, FixtureApifoxCompactWideJsonRegression) {
    using namespace tools3000::ocr;
    auto& deep = DeepOcrEngine::instance();
    if (!deep.getModelStatus().installed) {
        GTEST_SKIP() << "PP-OCRv4 offline model bundle not installed";
    }

    const cv::Mat img = loadRealWorldOcrFixture("apifox_json_compact.jpg");
    ASSERT_FALSE(img.empty());

    const auto lines = deep.extractText(img);
    ASSERT_GE(lines.size(), 25u);
    const std::string allText = joinOcrLines(lines);

    // 超宽紧凑单行 JSON 核心字段与结构标点规整回归断言
    EXPECT_NE(allText.find("{\"U_PLM_Categ2\":\"BCP0300\","), std::string::npos) << allText;
    EXPECT_NE(allText.find("\"U_PLM_Categ1\":\"半成品\","), std::string::npos) << allText;
    EXPECT_NE(allText.find("\"U_YF_Project\":\"TXZW-WH003\","), std::string::npos) << allText;
    EXPECT_NE(allText.find("\"PlanningSystem\":\"bop_MRP\","), std::string::npos) << allText;
    EXPECT_NE(allText.find("\"ProcurementMethod\":\"bom_Make\","), std::string::npos) << allText;
    EXPECT_NE(allText.find("\"ItemCode\":\"BCP1097\","), std::string::npos) << allText;
    EXPECT_NE(allText.find("\"InventoryItem\":\"Y\","), std::string::npos) << allText;
    EXPECT_NE(allText.find("\"User_Text\":"), std::string::npos) << allText;
    EXPECT_NE(allText.find("\"U_financial_type\":\"自研产"), std::string::npos) << allText;
    EXPECT_NE(allText.find("\"ItemPrices\":[{\"Currency\":\"RMB\",\"PriceList\":1}]}"), std::string::npos) << allText;

    // 字符串值内部中文标点必须完好保全，严防被 JSON 标点规整误伤
    EXPECT_NE(allText.find("（型号：SY-F19-L）"), std::string::npos) << allText;
    EXPECT_NE(allText.find("生产厂家：南通南卓电"), std::string::npos) << allText;

    // 底部错误响应 JSON 与 UI 按钮断言（含 `"message": {` 单个花括号精准还原）
    EXPECT_NE(allText.find("\"error\": {"), std::string::npos) << allText;
    EXPECT_NE(allText.find("\"code\": -10,"), std::string::npos) << allText;
    EXPECT_NE(allText.find("\"message\": {"), std::string::npos) << allText;
    EXPECT_EQ(allText.find("\"message\": f"), std::string::npos) << allText;
    EXPECT_NE(allText.find("\"lang\": \"en-us\","), std::string::npos) << allText;
    EXPECT_NE(allText.find("\"value\": \"Internal error (-10) occurred\""), std::string::npos) << allText;
    EXPECT_NE(allText.find("PATCH"), std::string::npos) << allText;
    EXPECT_EQ(allText.find("PATCH v"), std::string::npos) << "Dropdown chevron next to PATCH leaked into OCR text";
    EXPECT_NE(allText.find("格式化"), std::string::npos) << allText;

    // 端到端复制拼接 + 标准 JSON 解析器强断言：
    // 框选编辑器区 5 行软折行紧凑 JSON 并走 OcrEngine::orderTextBlocksWithXYCut 拼接后，
    // 必须无缝缝合所有被中文软折行劈断的字符串，直接通过 nlohmann::json::parse 零报错解析！
    std::vector<OcrResult> jsonEditorLines;
    for (const auto& blk : lines) {
        if (blk.box.y >= 175 && blk.box.y <= 260 && blk.box.x >= 60) {
            jsonEditorLines.push_back(blk);
        }
    }
    ASSERT_EQ(jsonEditorLines.size(), 5u);
    const std::string copiedJson = OcrEngine::orderTextBlocksWithXYCut(jsonEditorLines, "\r\n");
    nlohmann::json parsed;
    ASSERT_NO_THROW(parsed = nlohmann::json::parse(copiedJson)) << "Copied compact JSON must be valid RFC 8259 JSON:\n" << copiedJson;
    EXPECT_EQ(parsed.at("U_PLM_Categ2").get<std::string>(), "BCP0300");
    EXPECT_EQ(parsed.at("U_PLM_Categ1").get<std::string>(), "半成品");
    EXPECT_EQ(parsed.at("ProcurementMethod").get<std::string>(), "bom_Make");
    EXPECT_EQ(parsed.at("ItemCode").get<std::string>(), "BCP1097");
    EXPECT_NE(parsed.at("ItemName").get<std::string>().find("浇封锂电池组（型号：SY-F19-L）"), std::string::npos);
    EXPECT_NE(parsed.at("User_Text").get<std::string>().find("南通南卓电子科技有限公司"), std::string::npos);
    EXPECT_EQ(parsed.at("U_financial_type").get<std::string>(), "自研产品");
    EXPECT_EQ(parsed.at("ItemsGroupCode").get<int>(), 104);
    ASSERT_TRUE(parsed.at("ItemPrices").is_array());
    ASSERT_EQ(parsed.at("ItemPrices").size(), 1u);
    EXPECT_EQ(parsed.at("ItemPrices")[0].at("Currency").get<std::string>(), "RMB");
    EXPECT_EQ(parsed.at("ItemPrices")[0].at("PriceList").get<int>(), 1);
}

TEST(LiveTextOcrTest, FixtureRealWorldScreenPhotosAndQrRegression) {
    using namespace tools3000::ocr;
    using namespace tools3000::capture;
    auto& deep = DeepOcrEngine::instance();
    if (!deep.getModelStatus().installed) {
        GTEST_SKIP() << "PP-OCRv4 offline model bundle not installed";
    }

    // 1. 手持 PDA 强摩尔纹拍屏：验证 ZZ30003665 不再回退为 "乙乙30003665"
    {
        const cv::Mat pda = loadRealWorldOcrFixture("pda_screen_photo.jpg");
        ASSERT_FALSE(pda.empty());
        const auto lines = deep.extractText(pda);
        const std::string allText = joinOcrLines(lines);
        EXPECT_NE(allText.find("生产报工"), std::string::npos) << allText;
        EXPECT_NE(allText.find("生产箱单-PL202610150024"), std::string::npos) << allText;
        EXPECT_NE(allText.find("ZZ30003665"), std::string::npos) << allText;
        EXPECT_EQ(allText.find("乙乙30003665"), std::string::npos) << allText;
    }

    // 2. 会议室电视 PPT 实拍：OCR 精准识别 + 真实端到端原位改字 "5%" -> "10%" 不抹除紧邻左字 "余"
    {
        const cv::Mat ppt = loadRealWorldOcrFixture("meeting_tv_photo.jpg");
        ASSERT_FALSE(ppt.empty());
        const auto lines = deep.extractText(ppt);
        const std::string allText = joinOcrLines(lines);
        EXPECT_NE(allText.find("SAP待解决问题"), std::string::npos) << allText;
        EXPECT_NE(allText.find("SAP机台销货只能是整数台"), std::string::npos) << allText;
        EXPECT_NE(allText.find("剩余5%待到期时开票"), std::string::npos) << allText;

        const OcrResult* targetLine = nullptr;
        for (const auto& r : lines) {
            if (r.text.find("剩余5%") != std::string::npos) {
                targetLine = &r;
                break;
            }
        }
        ASSERT_NE(targetLine, nullptr);
        const auto cps = OcrEngine::splitUtf8Codepoints(targetLine->text);
        int idx5 = -1;
        for (size_t c = 0; c + 1 < cps.size(); ++c) {
            if (cps[c] == "5" && cps[c + 1] == "%") {
                idx5 = static_cast<int>(c);
                break;
            }
        }
        ASSERT_GE(idx5, 1);
        ASSERT_EQ(targetLine->charBoxes.size(), cps.size());

        const cv::Rect prevCharBox = targetLine->charBoxes[static_cast<size_t>(idx5 - 1)]; // "余"
        const cv::Rect selRect = targetLine->charBoxes[static_cast<size_t>(idx5)] |
                                 targetLine->charBoxes[static_cast<size_t>(idx5 + 1)];
        cv::Rect tailRect;
        for (size_t c = static_cast<size_t>(idx5 + 2); c < targetLine->charBoxes.size(); ++c) {
            tailRect = tailRect.empty() ? targetLine->charBoxes[c] : (tailRect | targetLine->charBoxes[c]);
        }

        initializeMarkupTextRenderer();
        TextReplaceEngine::clearPatchCache();
        TextReplaceRenderSpec spec;
        spec.sourceRect = selRect;
        spec.charBoxes = {targetLine->charBoxes[static_cast<size_t>(idx5)],
                          targetLine->charBoxes[static_cast<size_t>(idx5 + 1)]};
        spec.originalTextUtf8 = "5%";
        spec.newTextUtf8 = "10%";
        spec.bgMode = TextReplaceBgMode::Auto;
        spec.reflowRightTail = !tailRect.empty();
        spec.rightTailRect = tailRect;

        cv::Mat replaced = ppt.clone();
        TextReplaceEngine::renderReplacement(replaced, spec);

        // 验证 "余" 字主体像素（避开最右 1px 抗锯齿交界）100% 未受损
        const cv::Rect safePrevRc = prevCharBox & cv::Rect(0, 0, ppt.cols, ppt.rows);
        const int checkW = (std::max)(1, safePrevRc.width - 2);
        int alteredPrevPixels = 0;
        for (int y = safePrevRc.y; y < safePrevRc.y + safePrevRc.height; ++y) {
            for (int x = safePrevRc.x; x < safePrevRc.x + checkW; ++x) {
                const cv::Vec3b a = ppt.at<cv::Vec3b>(y, x);
                const cv::Vec3b b = replaced.at<cv::Vec3b>(y, x);
                if (std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) > 18) {
                    ++alteredPrevPixels;
                }
            }
        }
        EXPECT_EQ(alteredPrevPixels, 0) << "In-place replacing 5% -> 10% must not erase neighboring glyph";
    }

    // 3. ERP 摩尔纹深灰表头密集表格拍屏
    {
        const cv::Mat erp = loadRealWorldOcrFixture("erp_table_screen_photo.jpg");
        ASSERT_FALSE(erp.empty());
        const auto lines = deep.extractText(erp);
        ASSERT_GE(lines.size(), 90u);
        const std::string allText = joinOcrLines(lines);
        EXPECT_NE(allText.find("仓库名称"), std::string::npos) << allText;
        EXPECT_NE(allText.find("零件号"), std::string::npos) << allText;
        EXPECT_NE(allText.find("单位"), std::string::npos) << allText;
        EXPECT_NE(allText.find("周转箱编码"), std::string::npos) << allText;
        EXPECT_NE(allText.find("焊装车间线边库"), std::string::npos) << allText;
        EXPECT_NE(allText.find("冲压车间线边库"), std::string::npos) << allText;
        EXPECT_NE(allText.find("ZZ00443481"), std::string::npos) << allText;
        EXPECT_NE(allText.find("400"), std::string::npos) << allText;
    }

    // 4. 工业产品装箱卡 + QR 码 + 背景脏污物料表
    {
        const cv::Mat card = loadRealWorldOcrFixture("packing_card_photo.jpg");
        ASSERT_FALSE(card.empty());
        const auto page = OcrEngine::instance().extractPage(card);
        ASSERT_GE(page.lines.size(), 40u);
        const std::string allText = joinOcrLines(page.lines);
        EXPECT_NE(allText.find("HB0501000546"), std::string::npos) << allText;
        EXPECT_NE(allText.find("501010567AA"), std::string::npos) << allText;
        EXPECT_NE(allText.find("右前纵梁后部加强板"), std::string::npos) << allText;
        EXPECT_NE(allText.find("成飞T1X项目"), std::string::npos) << allText;
        EXPECT_NE(allText.find("ZZ30001332"), std::string::npos) << allText;
        EXPECT_NE(allText.find("崔华松"), std::string::npos) << allText;
        EXPECT_NE(allText.find("502009390AA"), std::string::npos) << allText;

        bool foundQr = false;
        for (const auto& qr : page.qrCodes) {
            if (qr.text.find("ZZ00444040") != std::string::npos) {
                foundQr = true;
                break;
            }
        }
        EXPECT_TRUE(foundQr) << "Packing card QR code ZZ00444040 must be decoded by OcrEngine::extractPage";
    }
}

TEST(LiveTextOcrTest, FixtureDenseDocAndSapMultiColumnRegression) {
    using namespace tools3000::ocr;
    auto& deep = DeepOcrEngine::instance();
    if (!deep.getModelStatus().installed) {
        GTEST_SKIP() << "PP-OCRv4 offline model bundle not installed";
    }

    // 1. SAP B1 多栏表单 + 密集表格 + 右侧属性边栏：验证高召回与左右栏物理隔离
    {
        const cv::Mat sap = loadRealWorldOcrFixture("sap_form_table.jpg");
        ASSERT_FALSE(sap.empty());
        const auto lines = deep.extractText(sap);
        ASSERT_GE(lines.size(), 60u);
        const std::string allText = joinOcrLines(lines);
        EXPECT_NE(allText.find("17,300.000 RMB"), std::string::npos) << allText;
        EXPECT_NE(allText.find("627,699.08 RMB"), std::string::npos) << allText;
        EXPECT_NE(allText.find("折扣前总计"), std::string::npos) << allText;
        EXPECT_NE(allText.find("到期付款总计"), std::string::npos) << allText;
        EXPECT_NE(allText.find("第3次CDD日期"), std::string::npos) << allText;
        EXPECT_NE(allText.find("SAP"), std::string::npos) << allText;

        // 主表单区 (x < 780) 与右侧属性边栏 (x > 820) 之间存在硬分栏，严禁出现跨越中线的超长幽灵框
        for (const auto& r : lines) {
            EXPECT_FALSE(r.box.x < 500 && (r.box.x + r.box.width) > 900)
                << "Cross-column ghost merge detected across SAP form and right sidebar: " << r.text;
        }
    }

    // 2. 高密度中英混排长文档：验证大文本量稳定召回与核心段落精度
    {
        const cv::Mat doc = loadRealWorldOcrFixture("release_notes_dense_doc.png");
        ASSERT_FALSE(doc.empty());
        const auto lines = deep.extractText(doc);
        ASSERT_GE(lines.size(), 35u);
        size_t totalChars = 0;
        for (const auto& r : lines) {
            totalChars += OcrEngine::countUtf8Codepoints(r.text);
        }
        EXPECT_GE(totalChars, 2000u);

        const std::string allText = joinOcrLines(lines);
        EXPECT_NE(allText.find("本次版本核心亮点与体验升级"), std::string::npos) << allText;
        EXPECT_NE(allText.find("演播室视听与时间轴重塑"), std::string::npos) << allText;
        EXPECT_NE(allText.find("WinML/DirectML"), std::string::npos) << allText;
        EXPECT_NE(allText.find("SHA-256"), std::string::npos) << allText;
        EXPECT_NE(allText.find("MP4 / GIF / WebP"), std::string::npos) << allText;
    }
}

TEST(LiveTextOcrTest, DeepOcrRedStampSuppressionShadowNormalizationAndRotatedPageOrder) {
    using namespace tools3000::ocr;

    // 1. 红章压黑字穿透：半透明大红公章圆环直接压在黑色正文上 -> 印章红墨水被穿透滤除，黑字 100% 保留；
    //    同时独立的纯红警示文字（不与黑字交叉）严禁被误抹除！
    {
        cv::Mat doc(220, 480, CV_8UC3, cv::Scalar(248, 248, 248));
        cv::putText(doc, "INV-2026-8899", cv::Point(40, 96),
                    cv::FONT_HERSHEY_SIMPLEX, 1.05, cv::Scalar(22, 22, 22), 2, cv::LINE_AA);
        // 叠加一个半透明大红圆形财务印章直接盖在 "INV-2026-8899" 上
        cv::Mat overlay = doc.clone();
        cv::circle(overlay, cv::Point(180, 90), 54, cv::Scalar(25, 28, 228), 5, cv::LINE_AA);
        cv::circle(overlay, cv::Point(180, 90), 36, cv::Scalar(25, 28, 228), 3, cv::LINE_AA);
        cv::line(overlay, cv::Point(132, 90), cv::Point(228, 90), cv::Scalar(25, 28, 228), 4, cv::LINE_AA);
        cv::addWeighted(overlay, 0.72, doc, 0.28, 0.0, doc);

        // 下方远离黑字区域放置独立的纯红警示文字（无黑字交叉），必须 100% 完整保留
        cv::putText(doc, "WARNING 404", cv::Point(40, 195),
                    cv::FONT_HERSHEY_SIMPLEX, 0.85, cv::Scalar(28, 30, 224), 2, cv::LINE_AA);

        bool suppressed = false;
        const cv::Mat cleaned = DeepOcrEngine::suppressRedStampForOcr(doc, &suppressed);
        EXPECT_TRUE(suppressed);

        // 校验红章顶部纯红弧线（非黑字处）已被漂白至接近纸面白底
        const cv::Vec3b stampTopPx = cleaned.at<cv::Vec3b>(36, 180);
        EXPECT_GT(static_cast<int>(stampTopPx[1]), 175) << "Red stamp green channel should be neutralized near white";
        EXPECT_LT(std::abs(static_cast<int>(stampTopPx[2]) - static_cast<int>(stampTopPx[1])), 16);

        // 校验独立纯红警示文字 "WARNING 404" 像素 100% 保留红字特征未被误伤
        int preservedRedWarningPixels = 0;
        for (int y = 172; y < 204; ++y) {
            for (int x = 36; x < 240; ++x) {
                const auto& p = cleaned.at<cv::Vec3b>(y, x);
                if (static_cast<int>(p[2]) - static_cast<int>(p[1]) >= 80) {
                    ++preservedRedWarningPixels;
                }
            }
        }
        EXPECT_GT(preservedRedWarningPixels, 120) << "Standalone red warning text must never be erased";

        // 模型已安装时，端到端实测穿透后能正确读出被红章覆盖的 "INV-2026-8899" 与独立红字 "WARNING 404"
        if (DeepOcrEngine::instance().getModelStatus().installed) {
            const auto lines = DeepOcrEngine::instance().extractText(doc);
            const std::string allText = joinOcrLines(lines);
            EXPECT_NE(allText.find("INV-2026-8899"), std::string::npos) << allText;
            EXPECT_NE(allText.find("WARNING"), std::string::npos) << allText;
        }
    }

    // 2. 纸面/手写拍照光照阴影归一化：左暗右亮手影梯度 + 轻微相机颗粒噪声 -> 精准判定并拉平纸面光照，
    //    同时对清晰纯白数字截图 100% 返回 false 零干预！
    {
        cv::Mat cleanScreen(200, 320, CV_8UC3, cv::Scalar(255, 255, 255));
        cv::putText(cleanScreen, "Clean Digital UI", cv::Point(20, 100),
                    cv::FONT_HERSHEY_SIMPLEX, 0.9, cv::Scalar(20, 20, 20), 2, cv::LINE_AA);
        EXPECT_FALSE(DeepOcrEngine::isShadowedDocumentPhoto(cleanScreen));

        cv::Mat shadowedPaper(260, 420, CV_8UC3);
        cv::RNG rng(20261121);
        for (int y = 0; y < shadowedPaper.rows; ++y) {
            auto* row = shadowedPaper.ptr<cv::Vec3b>(y);
            for (int x = 0; x < shadowedPaper.cols; ++x) {
                const float t = static_cast<float>(x) / static_cast<float>(shadowedPaper.cols);
                const int base = static_cast<int>(108.0f + 122.0f * t);
                const int noise = rng.uniform(-7, 8);
                const uint8_t b = static_cast<uint8_t>(std::clamp(base - 8 + noise, 0, 255));
                const uint8_t g = static_cast<uint8_t>(std::clamp(base - 2 + noise, 0, 255));
                const uint8_t r = static_cast<uint8_t>(std::clamp(base + 4 + noise, 0, 255));
                row[x] = cv::Vec3b(b, g, r);
            }
        }
        cv::putText(shadowedPaper, "NOTE 2026-11-21", cv::Point(32, 130),
                    cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(48, 52, 58), 2, cv::LINE_AA);
        EXPECT_TRUE(DeepOcrEngine::isShadowedDocumentPhoto(shadowedPaper));

        const cv::Mat normalized = DeepOcrEngine::enhanceDocumentPhoto(shadowedPaper);
        // 左侧原暗影区 (base~110) 应被提亮至接近纸面白底 (>= 195)，大幅缩小左右亮度极差
        const cv::Scalar leftMean = cv::mean(normalized(cv::Rect(10, 10, 40, 40)));
        const cv::Scalar rightMean = cv::mean(normalized(cv::Rect(360, 10, 40, 40)));
        EXPECT_GT(leftMean[1], 195.0);
        EXPECT_LT(std::abs(rightMean[1] - leftMean[1]), 35.0);
    }

    // 3. 整图顺时针拍歪 90° 横向文档端到端识别与阅读序校正
    if (DeepOcrEngine::instance().getModelStatus().installed) {
        cv::Mat horiz(240, 520, CV_8UC3, cv::Scalar(252, 252, 252));
        cv::putText(horiz, "LINE-ONE-ALPHA", cv::Point(40, 62),
                    cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(20, 20, 20), 2, cv::LINE_AA);
        cv::putText(horiz, "LINE-TWO-BETA", cv::Point(40, 132),
                    cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(20, 20, 20), 2, cv::LINE_AA);
        cv::putText(horiz, "LINE-THREE-GAMMA", cv::Point(40, 202),
                    cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(20, 20, 20), 2, cv::LINE_AA);

        cv::Mat rot90;
        cv::rotate(horiz, rot90, cv::ROTATE_90_CLOCKWISE);
        const auto lines = DeepOcrEngine::instance().extractText(rot90);
        const std::string allText = joinOcrLines(lines);
        const size_t pos1 = allText.find("ALPHA");
        const size_t pos2 = allText.find("BETA");
        const size_t pos3 = allText.find("GAMMA");
        EXPECT_NE(pos1, std::string::npos) << allText;
        EXPECT_NE(pos2, std::string::npos) << allText;
        EXPECT_NE(pos3, std::string::npos) << allText;
        if (pos1 != std::string::npos && pos2 != std::string::npos && pos3 != std::string::npos) {
            EXPECT_LT(pos1, pos2) << "90-degree rotated page must restore logical top-to-bottom reading order:\n" << allText;
            EXPECT_LT(pos2, pos3) << "90-degree rotated page must restore logical top-to-bottom reading order:\n" << allText;
        }
    }
}

TEST(LiveTextOcrTest, DeepOcrModelTierFallbackAndServerRemoval) {
    using namespace tools3000::ocr;
    namespace fs = std::filesystem;

    auto& deep = DeepOcrEngine::instance();
    auto& cfg = tools3000::core::ConfigManager::instance();

    const fs::path tempRoot = fs::temp_directory_path() / L"tools3000_ocr_tier_test_dir";
    const fs::path tempDir = tempRoot / L"models";
    const fs::path tempCfgDir = tempRoot / L"config";
    std::error_code ec;
    fs::remove_all(tempRoot, ec);
    ASSERT_TRUE(fs::create_directories(tempDir, ec) || fs::is_directory(tempDir));
    ASSERT_TRUE(fs::create_directories(tempCfgDir, ec) || fs::is_directory(tempCfgDir));
    ASSERT_TRUE(cfg.initialize(tempCfgDir));

    struct TierTestGuard {
        DeepOcrEngine& engine;
        tools3000::core::ConfigManager& config;
        fs::path rootDir;
        ~TierTestGuard() {
            engine.setCustomModelDirectoryForTest({});
            try {
                config.mergePatch({{"ocr", {{"modelTier", "mobile"}}}}, "/ocr");
            } catch (...) {
            }
            std::error_code cleanEc;
            fs::remove_all(rootDir, cleanEc);
        }
    } guard{deep, cfg, tempRoot};

    auto writeDummyFile = [](const fs::path& p, size_t size, uint8_t firstByte) {
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(ofs.good());
        std::vector<uint8_t> buf(size, 0x20);
        buf[0] = firstByte;
        ofs.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    };

    // 1. 仅安装基础 mobile 四件套
    writeDummyFile(tempDir / DeepOcrConstants::kDetModelFileName, 512, 0x08);
    writeDummyFile(tempDir / DeepOcrConstants::kClsModelFileName, 512, 0x08);
    writeDummyFile(tempDir / DeepOcrConstants::kRecModelFileName, 512, 0x08);
    writeDummyFile(tempDir / DeepOcrConstants::kKeysDictFileName, 128, 'a');

    deep.setCustomModelDirectoryForTest(tempDir);
    EXPECT_TRUE(deep.isModelInstalled());
    EXPECT_FALSE(deep.isServerModelInstalled());

    // 未安装 server 模型时配置为 server，必须优雅回退 mobile 不瘫痪
    ASSERT_TRUE(cfg.mergePatch({{"ocr", {{"modelTier", "server"}}}}, "/ocr"));
    EXPECT_EQ(deep.configuredModelTier(), "server");
    EXPECT_EQ(deep.effectiveModelTier(), "mobile");
    {
        const auto st = deep.getModelStatus();
        EXPECT_TRUE(st.installed);
        EXPECT_FALSE(st.serverInstalled);
        EXPECT_EQ(st.modelTier, "server");
        EXPECT_EQ(st.activeModelTier, "mobile");
    }

    // 2. 落地高精度 server 识别模型文件后自动生效
    writeDummyFile(tempDir / DeepOcrConstants::kRecServerModelFileName, 1024, 0x08);
    EXPECT_TRUE(deep.isServerModelInstalled());
    EXPECT_EQ(deep.effectiveModelTier(), "server");
    {
        const auto st = deep.getModelStatus();
        EXPECT_TRUE(st.serverInstalled);
        EXPECT_EQ(st.activeModelTier, "server");
    }

    // 切回 mobile 后 server 文件保留但生效档位切回 mobile
    ASSERT_TRUE(cfg.mergePatch({{"ocr", {{"modelTier", "mobile"}}}}, "/ocr"));
    EXPECT_TRUE(deep.isServerModelInstalled());
    EXPECT_EQ(deep.effectiveModelTier(), "mobile");

    // 3. 选中 server 后执行 deleteServerModel()：释放 server 文件、基础包完好且配置回落 mobile
    ASSERT_TRUE(cfg.mergePatch({{"ocr", {{"modelTier", "server"}}}}, "/ocr"));
    EXPECT_TRUE(deep.deleteServerModel());
    EXPECT_FALSE(deep.isServerModelInstalled());
    EXPECT_TRUE(deep.isModelInstalled());
    EXPECT_EQ(deep.configuredModelTier(), "mobile");
    EXPECT_EQ(deep.effectiveModelTier(), "mobile");
}

TEST(LiveTextOcrTest, DeepOcrIdleUnloadAndCompactSessionBudget) {
    using namespace tools3000::ocr;
    auto& deep = DeepOcrEngine::instance();
    if (!deep.isModelInstalled()) {
        GTEST_SKIP() << "PP-OCRv4 model bundle not installed";
    }

    struct TimeoutRestoreGuard {
        DeepOcrEngine& engine;
        ~TimeoutRestoreGuard() {
            engine.setIdleUnloadTimeoutForTest(DeepOcrConstants::kIdleUnloadTimeoutSeconds);
        }
    } restoreGuard{deep};

    // 设置 1 秒极速闲置回收用于自动化验证
    deep.setIdleUnloadTimeoutForTest(1);

    cv::Mat sample(120, 420, CV_8UC3, cv::Scalar(255, 255, 255));
    cv::putText(sample, "Tools3000 OCR 2026", cv::Point(24, 76),
                cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(20, 20, 20), 2, cv::LINE_AA);

    const auto queryPrivateAndWsMb = []() -> std::pair<double, double> {
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (::K32GetProcessMemoryInfo(::GetCurrentProcess(),
                                      reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                      sizeof(pmc))) {
            return {static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0),
                    static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0)};
        }
        return {0.0, 0.0};
    };

    const auto [basePrivMb, baseWsMb] = queryPrivateAndWsMb();
    const auto lines = deep.extractText(sample);
    ASSERT_FALSE(lines.empty());
    EXPECT_TRUE(deep.isWarmAndLoaded());
    // 紧凑会话池上限约束：单次简单识别绝不允许初始化十余个冗余 DirectML 会话
    EXPECT_LE(deep.activeSessionCount(), 6u);
    const auto [peakPrivMb, peakWsMb] = queryPrivateAndWsMb();

    // 等待闲置看门狗在 1 秒后自动释放全部 WinML/DirectML 会话及独占 D3D11 设备，真正归还操作系统私有提交内存
    bool unloaded = false;
    for (int i = 0; i < 40; ++i) {
        if (!deep.isWarmAndLoaded() && deep.activeSessionCount() == 0u) {
            unloaded = true;
            break;
        }
        Sleep(100);
    }
    EXPECT_TRUE(unloaded) << "DeepOcrEngine did not auto-release models after idle timeout";
    EXPECT_EQ(deep.activeSessionCount(), 0u);
    Sleep(200);
    const auto [afterPrivMb, afterWsMb] = queryPrivateAndWsMb();
    std::printf("[OCR-MEM] base=(priv=%.1fMB ws=%.1fMB) peak=(priv=%.1fMB ws=%.1fMB) afterIdle=(priv=%.1fMB ws=%.1fMB)\n",
                basePrivMb, baseWsMb, peakPrivMb, peakWsMb, afterPrivMb, afterWsMb);
    // 防回退硬门禁：当 DirectML GPU 推理将专用私有内存推升超 250MB 时，独占 D3D11 释放后必须真正归还至少 55% 增量私有提交
    const double peakDeltaMb = peakPrivMb - basePrivMb;
    if (peakDeltaMb > 250.0) {
        const double reclaimedMb = peakPrivMb - afterPrivMb;
        EXPECT_GE(reclaimedMb, peakDeltaMb * 0.55)
            << "Owned D3D11 + WinML idle unload must reclaim DirectML private commit back to OS";
    }
}

TEST(WorkerIsolationTest, StudioExportWorkerJobJsonRoundTrip) {
    using namespace tools3000::capture;

    StudioExportJobPayload src{};
    src.rawInputPathUtf8 = "C:/temp/raw_input.mp4";
    src.finalOutputPathUtf8 = "C:/temp/final_studio.mp4";
    src.traceId = "20261121-140000-ABCD";
    src.exportOpts.format = StudioExportFormat::MP4_HEVC;
    src.exportOpts.resolution = StudioExportResolution::Res1440P;
    src.exportOpts.fps = 60;
    src.exportOpts.quality = StudioExportQuality::High;
    src.exportOpts.baseBitrateMbps = 18;

    src.config.autoZoomEnabled = true;
    src.config.zoomMode = StudioZoomMode::Motion3D;
    src.config.typingAutoZoom = true;
    src.config.defaultZoomFactor = 1.65f;
    src.config.zoomPacing = 0.55f;
    src.config.autoZoomSensitivity = 0.62f;
    src.config.focusSpotlightDim = 0.24f;
    src.config.lockCameraWhileZoomed = false;
    src.config.motionBlurEnabled = true;
    src.config.smoothCursorEnabled = true;
    src.config.cursorScale = 1.5f;
    src.config.cursorHighlightHalo = true;
    src.config.showClickRipple = true;
    src.config.clickEffectStyle = StudioClickEffectStyle::DualRipple;
    src.config.autoFrameEnabled = true;
    src.config.aspectRatio = StudioAspectRatio::Ratio16_9;
    src.config.backgroundTheme = BeautyBackgroundType::MidnightGraphite;
    src.config.backgroundPresetKey = "blossom_silk";
    src.config.customBackgroundPath = "C:/wall/custom.jpg";
    src.config.paddingRatio = 0.11f;
    src.config.insetBorderWidth = 9.0f;
    src.config.insetBorderColorHex = "#E2E8F0";
    src.config.backgroundBlur = 12.5f;
    src.config.cornerRadius = 22.0f;
    src.config.shadowIntensity = 0.8f;
    src.config.autoCaptionsEnabled = true;
    src.config.exportSrtSidecar = false;
    src.config.watermark.enabled = true;
    src.config.watermark.mode = WatermarkMode::CornerText;
    src.config.watermark.textTemplate = "Tools3000 {date}";
    src.config.watermark.anchor = WatermarkAnchor::CustomFree;
    src.config.watermark.customNormX = 0.77f;
    src.config.watermark.customNormY = 0.84f;
    src.config.watermark.scale = 1.25f;
    src.config.watermark.opacity = 0.45f;

    src.timeline.regionX = 100;
    src.timeline.regionY = 200;
    src.timeline.sourceWidth = 1920;
    src.timeline.sourceHeight = 1080;
    src.timeline.fps = 60;
    src.timeline.durationMs = 5400;
    src.timeline.cursorSamples.push_back({120, 320.5f, 410.25f, StudioCursorKind::Hand, true, false});
    src.timeline.clickEvents.push_back({450, 500.0f, 300.0f, true});
    src.timeline.focusEvents.push_back({600, 640.0f, 360.0f});
    src.timeline.windowEvents.push_back({800, 0x123456ULL, 10.0f, 20.0f, 800.0f, 600.0f});
    src.timeline.captions.push_back({100, 1500, "Hello Tools3000"});
    src.timeline.zoomSegments.push_back({300, 1800, 512.0f, 384.0f, 1.75f, true, true});
    src.timeline.cutSegments.push_back({2000, 2600, 2.0f, true});
    src.timeline.annotations.push_back({StudioAnnotationKind::TextBubble, 400, 2200, 0.2f, 0.3f, 0.6f, 0.5f, "#3B82F6", "Key Step", 4.0f, true, true});

    const std::string encoded = serializeStudioExportJobJson(src);
    StudioExportJobPayload dst{};
    ASSERT_TRUE(deserializeStudioExportJobJson(encoded, dst));

    EXPECT_EQ(dst.rawInputPathUtf8, src.rawInputPathUtf8);
    EXPECT_EQ(dst.finalOutputPathUtf8, src.finalOutputPathUtf8);
    EXPECT_EQ(dst.traceId, src.traceId);
    EXPECT_EQ(dst.exportOpts.format, src.exportOpts.format);
    EXPECT_EQ(dst.exportOpts.resolution, src.exportOpts.resolution);
    EXPECT_EQ(dst.exportOpts.fps, src.exportOpts.fps);
    EXPECT_EQ(dst.exportOpts.quality, src.exportOpts.quality);
    EXPECT_EQ(dst.exportOpts.baseBitrateMbps, src.exportOpts.baseBitrateMbps);
    EXPECT_EQ(dst.config.zoomMode, src.config.zoomMode);
    EXPECT_FLOAT_EQ(dst.config.defaultZoomFactor, src.config.defaultZoomFactor);
    EXPECT_EQ(dst.config.backgroundPresetKey, src.config.backgroundPresetKey);
    EXPECT_EQ(dst.config.watermark.textTemplate, src.config.watermark.textTemplate);
    EXPECT_FLOAT_EQ(dst.config.watermark.customNormX, src.config.watermark.customNormX);
    ASSERT_EQ(dst.timeline.cursorSamples.size(), 1u);
    EXPECT_EQ(dst.timeline.cursorSamples[0].kind, StudioCursorKind::Hand);
    EXPECT_TRUE(dst.timeline.cursorSamples[0].leftDown);
    ASSERT_EQ(dst.timeline.zoomSegments.size(), 1u);
    EXPECT_FLOAT_EQ(dst.timeline.zoomSegments[0].targetZoom, 1.75f);
    ASSERT_EQ(dst.timeline.annotations.size(), 1u);
    EXPECT_EQ(dst.timeline.annotations[0].text, "Key Step");
    EXPECT_EQ(dst.timeline.annotations[0].kind, StudioAnnotationKind::TextBubble);
}

TEST(WorkerIsolationTest, OcrWorkerOutOfProcessIsolationAndIdleExit) {
    using namespace tools3000::ocr;

    std::array<wchar_t, 4096> exeBuf{};
    const DWORD len = GetModuleFileNameW(nullptr, exeBuf.data(), static_cast<DWORD>(exeBuf.size()));
    ASSERT_GT(len, 0u);
    const std::filesystem::path hostExe = std::filesystem::path(exeBuf.data()).parent_path() / L"Tools3000.exe";
    if (!std::filesystem::is_regular_file(hostExe)) {
        GTEST_SKIP() << "Tools3000.exe not found beside test binary";
    }

    auto& deep = DeepOcrEngine::instance();
    if (!deep.isModelInstalled()) {
        GTEST_SKIP() << "PP-OCRv4 model bundle not installed";
    }

    deep.releaseLoadedModels();
    ASSERT_EQ(deep.activeSessionCount(), 0u);

    struct WorkerOverrideGuard {
        ~WorkerOverrideGuard() {
            OcrWorkerClient::instance().setTestOverride(false);
        }
    } guard;
    OcrWorkerClient::instance().setTestOverride(true, hostExe, /*idleSecondsOverride=*/2);

    cv::Mat sample(140, 480, CV_8UC3, cv::Scalar(255, 255, 255));
    cv::putText(sample, "WorkerOCR 2026", cv::Point(24, 88),
                cv::FONT_HERSHEY_SIMPLEX, 1.15, cv::Scalar(16, 16, 16), 3, cv::LINE_AA);

    const auto lines1 = deep.extractText(sample);
    ASSERT_FALSE(lines1.empty());
    EXPECT_EQ(deep.activeSessionCount(), 0u) << "Host process must keep 0 in-process DirectML sessions";
    EXPECT_TRUE(OcrWorkerClient::instance().isWorkerRunning());
    const DWORD pid1 = OcrWorkerClient::instance().activeWorkerPid();
    ASSERT_NE(pid1, 0u);
    ASSERT_NE(pid1, GetCurrentProcessId());

    // 等待 2 秒闲置超时触发子进程自动退场，操作系统 100% 物理+虚拟双归零
    bool exited = false;
    for (int i = 0; i < 60; ++i) {
        if (!OcrWorkerClient::instance().isWorkerRunning()) {
            exited = true;
            break;
        }
        Sleep(100);
    }
    EXPECT_TRUE(exited) << "Isolated OCR worker must self-exit after idle timeout";

    // 二次识别自动透明拉起新子进程
    const auto lines2 = deep.extractText(sample);
    ASSERT_FALSE(lines2.empty());
    EXPECT_TRUE(OcrWorkerClient::instance().isWorkerRunning());
    const DWORD pid2 = OcrWorkerClient::instance().activeWorkerPid();
    EXPECT_NE(pid2, 0u);
    EXPECT_NE(pid2, pid1);

    // 主动释放立即停机
    deep.releaseLoadedModels();
    EXPECT_FALSE(OcrWorkerClient::instance().isWorkerRunning());
}

TEST(WorkerIsolationTest, RecordEncodeAndStudioExportWorkersOutOfProcessEndToEnd) {
    using namespace tools3000::capture;

    std::array<wchar_t, 4096> exeBuf{};
    const DWORD len = GetModuleFileNameW(nullptr, exeBuf.data(), static_cast<DWORD>(exeBuf.size()));
    ASSERT_GT(len, 0u);
    const std::filesystem::path hostExe = std::filesystem::path(exeBuf.data()).parent_path() / L"Tools3000.exe";
    if (!std::filesystem::is_regular_file(hostExe)) {
        GTEST_SKIP() << "Tools3000.exe not found beside test binary";
    }

    const auto tmpDir = std::filesystem::temp_directory_path() /
        (L"tools3000_worker_e2e_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    std::error_code ec;
    std::filesystem::create_directories(tmpDir, ec);
    struct TempDirCleanup {
        std::filesystem::path dir;
        ~TempDirCleanup() {
            std::error_code rmEc;
            std::filesystem::remove_all(dir, rmEc);
        }
    } dirGuard{tmpDir};

    const std::filesystem::path rawMp4 = tmpDir / L"raw_isolated.mp4";
    const std::filesystem::path studioMp4 = tmpDir / L"studio_isolated.mp4";
    const std::string rawMp4Utf8 = tools3000::core::WinUtils::wstringToUtf8(rawMp4.wstring());
    const std::string studioMp4Utf8 = tools3000::core::WinUtils::wstringToUtf8(studioMp4.wstring());

    // 1. 通过独立短命 record-encode 子进程编码 12 帧 640x360 BGRA + 立体声 PCM
    RecordEncodeWorkerClient recWorker;
    RecordEncodeInitConfig initCfg{};
    initCfg.format = RecordFormat::MP4_H264;
    initCfg.width = 640;
    initCfg.height = 360;
    initCfg.fps = 30;
    initCfg.bitrateMbps = 4;
    initCfg.enableAudio = true;
    initCfg.audioSampleRate = 48000;
    initCfg.audioChannels = 2;
    initCfg.workingOutputPathUtf8 = rawMp4Utf8;

    RecordEncodeInitResult initRes{};
    ASSERT_TRUE(recWorker.start(initCfg, initRes, hostExe));
    ASSERT_TRUE(initRes.ok);
    EXPECT_FALSE(initRes.encoderName.empty());
    EXPECT_NE(recWorker.childPid(), 0u);
    EXPECT_NE(recWorker.childPid(), GetCurrentProcessId());

    cv::Mat frameBgra(360, 640, CV_8UC4, cv::Scalar(48, 84, 180, 255));
    for (int f = 0; f < 12; ++f) {
        cv::rectangle(frameBgra, cv::Rect(30 + f * 18, 80, 120, 90), cv::Scalar(240, 200, 60, 255), -1);
        ASSERT_TRUE(recWorker.sendVideoFrame(
            frameBgra.data, frameBgra.cols, frameBgra.rows,
            static_cast<int>(frameBgra.step[0]), CapturePixelFormat::Bgra32,
            f, f == 0));
    }
    if (initRes.audioEnabled) {
        std::vector<float> pcm(static_cast<size_t>(initRes.audioFrameSamples) * 2u, 0.01f);
        ASSERT_TRUE(recWorker.sendAudioSamples(pcm.data(), initRes.audioFrameSamples, 2, 0));
    }
    bool closeOk = false;
    ASSERT_TRUE(recWorker.finalize(closeOk));
    EXPECT_TRUE(closeOk);
    EXPECT_FALSE(recWorker.isActive());
    ASSERT_TRUE(std::filesystem::is_regular_file(rawMp4, ec));
    EXPECT_GT(std::filesystem::file_size(rawMp4, ec), 1024u);

    // 2. 通过独立短命 studio-export 子进程把上述原片合成为美化 MP4
    StudioTimelineData timeline{};
    timeline.sourceWidth = 640;
    timeline.sourceHeight = 360;
    timeline.fps = 30;
    timeline.durationMs = 400;
    StudioRenderConfig cfg{};
    cfg.autoFrameEnabled = true;
    cfg.backgroundPresetKey = "studio_slate";
    StudioExportOptions exportOpts{};
    exportOpts.format = StudioExportFormat::MP4_H264;
    exportOpts.resolution = StudioExportResolution::Res720P;
    exportOpts.fps = 30;
    exportOpts.quality = StudioExportQuality::Compact;

    double maxProgress = 0.0;
    int progressCalls = 0;
    const auto exportOutcome = exportStudioMediaOutOfProcess(
        rawMp4Utf8,
        studioMp4Utf8,
        timeline,
        cfg,
        exportOpts,
        [&](double p) {
            ++progressCalls;
            if (p > maxProgress) maxProgress = p;
        },
        nullptr,
        hostExe);
    EXPECT_TRUE(exportOutcome.handledOutOfProcess);
    EXPECT_TRUE(exportOutcome.succeeded);
    EXPECT_GT(progressCalls, 0);
    EXPECT_GE(maxProgress, 0.99);
    ASSERT_TRUE(std::filesystem::is_regular_file(studioMp4, ec));
    EXPECT_GT(std::filesystem::file_size(studioMp4, ec), 1024u);

    // 3. 验证导出的成片可被 StudioHiResFrameReader 正常解码，且全局缓存可安全释放
    StudioHiResFrameReader reader;
    ASSERT_TRUE(reader.open(studioMp4Utf8, 1280));
    cv::Mat decodedFrame;
    EXPECT_TRUE(reader.decodeFrameAt(0.05, decodedFrame, false));
    EXPECT_FALSE(decodedFrame.empty());
    reader.close();
    StudioVideoCompositor::clearGlobalCaches();
}
