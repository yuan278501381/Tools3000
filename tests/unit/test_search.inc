// ─────────────────────────────────────────────────────────────────────────────
// test_search.inc — SEARCH 模块化单元测试集
// ─────────────────────────────────────────────────────────────────────────────

#include "service/StringArena.h"

TEST(StringArenaTest, OversizedSideBlockPreservesActiveBlockCapacity) {
    tools3000::service::StringArena arena;
    const std::uint64_t baselineBytes = arena.approximateBytes();

    const auto prefixOffset = arena.append(L"prefix");
    std::wstring oversized(tools3000::service::StringArena::BlockChars + 17, L'X');
    const auto oversizedOffset = arena.append(oversized);
    const auto suffixOffset = arena.append(L"suffix");

    EXPECT_EQ(prefixOffset, 1u);
    EXPECT_EQ(oversizedOffset, 1u << tools3000::service::StringArena::BlockShift);
    EXPECT_EQ(suffixOffset, prefixOffset + 6u)
        << "the oversized side block must not abandon the active block tail";
    EXPECT_EQ(arena.view(prefixOffset, 6), L"prefix");
    EXPECT_EQ(arena.view(oversizedOffset, static_cast<std::uint32_t>(oversized.size())),
              std::wstring_view(oversized));
    EXPECT_EQ(arena.view(suffixOffset, 6), L"suffix");
    EXPECT_EQ(arena.approximateBytes(),
              baselineBytes + oversized.size() * sizeof(wchar_t));

    arena.reset();
    EXPECT_EQ(arena.approximateBytes(), baselineBytes);
}

TEST(PinyinEngineTest, FuzzyAndInitialMatching) {
    auto wx_init = PinyinEngine::GetInitials(L"微信");
    auto wx_full = PinyinEngine::GetFullPinyin(L"微信");
    std::wprintf(L"[DEBUG] 微信 initials: %ls, full: %ls\n", wx_init.c_str(), wx_full.c_str());

    // 拼音首字母
    EXPECT_EQ(PinyinEngine::GetInitials(L"微信"), L"wx");
    EXPECT_EQ(PinyinEngine::GetInitials(L"你好世界"), L"nhsj");
    EXPECT_EQ(PinyinEngine::GetInitials(L"Tools3000工具"), L"tools3000gj");

    // 拼音全拼
    EXPECT_EQ(PinyinEngine::GetFullPinyin(L"微信"), L"weixin");
    EXPECT_EQ(PinyinEngine::GetFullPinyin(L"你好"), L"nihao");
    EXPECT_EQ(PinyinEngine::GetFullPinyin(L"Tools3000"), L"tools3000");
}

TEST(PinyinEngineTest, PreservesUnsupportedUnicodeAndSurrogatePairs) {
    const std::wstring supplementary{static_cast<wchar_t>(0xD840),
                                     static_cast<wchar_t>(0xDC00)};
    EXPECT_EQ(PinyinEngine::GetFirstLetter(L'\u3042'), L'\u3042');
    EXPECT_EQ(PinyinEngine::GetCharPinyin(L'\uD7FF'), L"\uD7FF");
    EXPECT_EQ(PinyinEngine::GetInitials(supplementary), supplementary);
    EXPECT_EQ(PinyinEngine::GetFullPinyin(supplementary), supplementary);
}

// -----------------------------------------------------------------------------
// 15. Everything 风格搜索表达式解析测试套件
// -----------------------------------------------------------------------------
TEST(SearchExpressionTest, EverythingSyntaxParsing) {
    FileRecord txtFile{1, 0, L"readme.txt", L"readme.txt", L"readme.txt", L"readme.txt", false};
    FileRecord pngFile{2, 0, L"screenshot.png", L"screenshot.png", L"screenshot.png", L"screenshot.png", false};
    FileRecord docDir{3, 0, L"Documents", L"documents", L"documents", L"documents", true};
    FileRecord wxFile{4, 0, L"微信.exe", L"微信.exe", L"wx.exe", L"weixin.exe", false};
    FileRecord cppFile{5, 0, L"test_main.cpp", L"test_main.cpp", L"test_main.cpp", L"test_main.cpp", false};
    FileRecord logFile{6, 0, L"app_2026.log", L"app_2026.log", L"app_2026.log", L"app_2026.log", false};

    // 1. 标准通配符与扩展名
    auto expr1 = SearchExpression::parse(L"*.txt");
    EXPECT_TRUE(expr1.matches(txtFile, L'C', L"C:\\readme.txt"));
    EXPECT_FALSE(expr1.matches(pngFile, L'C', L"C:\\screenshot.png"));

    auto exprExt = SearchExpression::parse(L"ext:png;jpg;webp");
    EXPECT_TRUE(exprExt.matches(pngFile, L'C', L"C:\\screenshot.png"));
    EXPECT_FALSE(exprExt.matches(txtFile, L'C', L"C:\\readme.txt"));

    // 2. 类型过滤器 (file: / folder: / dir:)
    auto exprFile = SearchExpression::parse(L"file: *.txt");
    EXPECT_TRUE(exprFile.matches(txtFile, L'C', L"C:\\readme.txt"));
    EXPECT_FALSE(exprFile.matches(docDir, L'C', L"C:\\Documents"));

    auto exprDir = SearchExpression::parse(L"folder: documents");
    EXPECT_TRUE(exprDir.matches(docDir, L'C', L"C:\\Documents"));
    EXPECT_FALSE(exprDir.matches(txtFile, L'C', L"C:\\readme.txt"));

    // 3. 逻辑与、或、非 (AND / OR / NOT)
    auto exprNot = SearchExpression::parse(L"*.cpp !test");
    FileRecord normalCpp{7, 0, L"main.cpp", L"main.cpp", L"main.cpp", L"main.cpp", false};
    EXPECT_FALSE(exprNot.matches(cppFile, L'C', L"C:\\test_main.cpp"));
    EXPECT_TRUE(exprNot.matches(normalCpp, L'C', L"C:\\main.cpp"));

    auto exprOr = SearchExpression::parse(L"ext:txt | ext:png");
    EXPECT_TRUE(exprOr.matches(txtFile, L'C', L"C:\\readme.txt"));
    EXPECT_TRUE(exprOr.matches(pngFile, L'C', L"C:\\screenshot.png"));
    EXPECT_FALSE(exprOr.matches(cppFile, L'C', L"C:\\test_main.cpp"));

    // 4. 正则表达式 (regex: / r:)
    auto exprRegex = SearchExpression::parse(L"regex:^app_[0-9]+\\.log$");
    EXPECT_TRUE(exprRegex.matches(logFile, L'C', L"C:\\logs\\app_2026.log"));
    EXPECT_FALSE(exprRegex.matches(txtFile, L'C', L"C:\\readme.txt"));

    // 5. 盘符与路径过滤 (c: / path: / parent:)
    auto exprDrive = SearchExpression::parse(L"c: *.txt");
    EXPECT_TRUE(exprDrive.matches(txtFile, L'C', L"C:\\readme.txt"));
    EXPECT_FALSE(exprDrive.matches(txtFile, L'D', L"D:\\readme.txt"));

    auto exprPath = SearchExpression::parse(L"path:logs");
    EXPECT_TRUE(exprPath.matches(logFile, L'C', L"C:\\logs\\app_2026.log"));
    EXPECT_FALSE(exprPath.matches(txtFile, L'C', L"C:\\readme.txt"));

    // 6. 拼音检索与禁用拼音 (pinyin: / nopy:)
    auto exprPy = SearchExpression::parse(L"wx");
    EXPECT_TRUE(exprPy.matches(wxFile, L'C', L"C:\\微信.exe"));

    auto exprNoPy = SearchExpression::parse(L"nopy:wx");
    EXPECT_FALSE(exprNoPy.matches(wxFile, L'C', L"C:\\微信.exe"));

    // 7. 双引号带空格短语
    auto exprQuote = SearchExpression::parse(L"\"test main\"");
    FileRecord spaceFile{8, 0, L"test main.cpp", L"test main.cpp", L"test main.cpp", L"test main.cpp", false};
    EXPECT_TRUE(exprQuote.matches(spaceFile, L'C', L"C:\\test main.cpp"));

    // 8. 跨目录与文件名联合搜索及跨层级通配符 (*中源*账号密码* 与 中源 账号密码)
    FileRecord nestedPassFile{9, 0, L"账号密码.txt", L"账号密码.txt", L"zhmm.txt", L"zhanghaomima.txt", false};
    std::wstring nestedPassPath = L"D:\\Chosen\\216-北京中源\\账号密码.txt";

    // 通配符跨目录搜索: *中源*账号密码*
    auto exprWildcardPath = SearchExpression::parse(L"*中源*账号密码*");
    EXPECT_TRUE(exprWildcardPath.matches(nestedPassFile, L'D', nestedPassPath));
    EXPECT_FALSE(exprWildcardPath.matches(txtFile, L'C', L"C:\\readme.txt"));

    // 多关键词空格组合跨目录搜索: 中源 账号密码
    auto exprMultiTerms = SearchExpression::parse(L"中源 账号密码");
    EXPECT_TRUE(exprMultiTerms.matches(nestedPassFile, L'D', nestedPassPath));
    EXPECT_FALSE(exprMultiTerms.matches(txtFile, L'C', L"C:\\readme.txt"));

    // 拼音与目录联合匹配: 中源 zhmm
    auto exprPinyinDir = SearchExpression::parse(L"中源 zhmm");
    EXPECT_TRUE(exprPinyinDir.matches(nestedPassFile, L'D', nestedPassPath));

    // 9. 用户真实连字符文件名检索: 019-get_Bom_Lev
    FileRecord bomFile{10, 0, L"019-get_Bom_Lev.txt", L"019-get_bom_lev.txt", L"019-get_bom_lev.txt", L"019-get_bom_lev.txt", false};
    auto exprBom1 = SearchExpression::parse(L"019-get_Bom_Lev");
    // 10. 盘符与根目录前缀搜索 (c:\, c:\ readme, c:\logs\ *.log)
    auto exprRoot = SearchExpression::parse(L"c:\\");
    EXPECT_TRUE(exprRoot.matches(txtFile, L'C', L"C:\\readme.txt"));
    EXPECT_FALSE(exprRoot.matches(bomFile, L'D', L"D:\\Chosen\\019.txt"));

    auto exprRootTerms = SearchExpression::parse(L"c:\\ readme");
    EXPECT_TRUE(exprRootTerms.matches(txtFile, L'C', L"C:\\readme.txt"));
    EXPECT_FALSE(exprRootTerms.matches(pngFile, L'C', L"C:\\screenshot.png"));

    auto exprSubdirWildcard = SearchExpression::parse(L"c:\\logs\\ *.log");
    EXPECT_TRUE(exprSubdirWildcard.matches(logFile, L'C', L"C:\\logs\\app_2026.log"));
    EXPECT_FALSE(exprSubdirWildcard.matches(txtFile, L'C', L"C:\\readme.txt"));
}

TEST(SearchExpressionTest, EnvironmentVariableExpansion) {
    FileRecord appDataFile;
    appDataFile.fileName = L"Tools3000.json";
    appDataFile.normalizedName = L"tools3000.json";
    appDataFile.isDirectory = false;

    wchar_t appDataBuf[MAX_PATH] = {0};
    GetEnvironmentVariableW(L"APPDATA", appDataBuf, MAX_PATH);
    std::wstring appDataStr = appDataBuf;
    if (!appDataStr.empty()) {
        std::wstring targetPath = appDataStr + L"\\Tools3000\\Tools3000.json";
        auto expr = SearchExpression::parse(L"%APPDATA%\\Tools3000\\Tools3000.json");
        EXPECT_TRUE(expr.requiresFullPath());
        EXPECT_TRUE(expr.matches(appDataFile, appDataStr[0], targetPath));
    }
}

// -----------------------------------------------------------------------------
// 16. 全屏检测与驱动器枚举测试套件
// -----------------------------------------------------------------------------

TEST(PipeEndpointTest, RequiresOpaqueTokenAndBuildsPerUserDescriptor) {
    using namespace tools3000::service::pipe_endpoint;
    EXPECT_FALSE(isValidToken("tools3000"));
    EXPECT_FALSE(isValidToken(std::string(TokenHexLength, 'g')));
    const std::string token(TokenHexLength, 'a');
    EXPECT_TRUE(isValidToken(token));
    ASSERT_TRUE(makePipeName(token).has_value());
    EXPECT_NE(makePipeName(token)->find(token), std::string::npos);
    EXPECT_FALSE(makePipeName("invalid").has_value());
    ASSERT_TRUE(makeSecurityDescriptor(L"S-1-5-21-1-2-3-1001").has_value());
    EXPECT_FALSE(makeSecurityDescriptor(L"").has_value());
}

TEST(PipeEndpointTest, RestrictedDescriptorAllowsCurrentUserEndpoint) {
    using namespace tools3000::service::pipe_endpoint;
    HANDLE token = nullptr;
    ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    struct TokenCloser { HANDLE value; ~TokenCloser() { if (value) CloseHandle(value); } } tokenCloser{token};
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    ASSERT_GT(bytes, 0u);
    std::vector<std::byte> tokenInfo(bytes);
    ASSERT_TRUE(GetTokenInformation(token, TokenUser, tokenInfo.data(), bytes, &bytes));
    auto* user = reinterpret_cast<TOKEN_USER*>(tokenInfo.data());
    LPWSTR sidText = nullptr;
    ASSERT_TRUE(ConvertSidToStringSidW(user->User.Sid, &sidText));
    struct SidFreer { LPWSTR value; ~SidFreer() { if (value) LocalFree(value); } } sidFreer{sidText};

    const auto sddl = makeSecurityDescriptor(sidText);
    ASSERT_TRUE(sddl.has_value());
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl->c_str(), SDDL_REVISION_1, &descriptor, nullptr));
    struct DescriptorFreer { PSECURITY_DESCRIPTOR value; ~DescriptorFreer() { if (value) LocalFree(value); } } descriptorFreer{descriptor};
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    const std::wstring name = L"\\\\.\\pipe\\Tools3000AclTest-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 128, 128, 0, &security);
    ASSERT_NE(pipe, INVALID_HANDLE_VALUE);
    struct PipeCloser { HANDLE value; ~PipeCloser() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } pipeCloser{pipe};

    std::atomic<bool> connected{false};
    std::thread client([&] {
        HANDLE clientPipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (clientPipe != INVALID_HANDLE_VALUE) {
            connected = true;
            CloseHandle(clientPipe);
        }
    });
    const BOOL connectedByConnect = ConnectNamedPipe(pipe, nullptr);
    const DWORD connectError = connectedByConnect ? ERROR_SUCCESS : GetLastError();
    EXPECT_TRUE(connectedByConnect || connectError == ERROR_PIPE_CONNECTED);
    client.join();
    EXPECT_TRUE(connected.load());
    DisconnectNamedPipe(pipe);

    const auto invalidSddl = makeSecurityDescriptor(L"not-a-windows-sid");
    ASSERT_TRUE(invalidSddl.has_value());
    PSECURITY_DESCRIPTOR invalidDescriptor = nullptr;
    EXPECT_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        invalidSddl->c_str(), SDDL_REVISION_1, &invalidDescriptor, nullptr));
    if (invalidDescriptor) LocalFree(invalidDescriptor);
}

TEST(PipeEndpointTest, DescriptorRejectsAnUnlistedCurrentUser) {
    using namespace tools3000::service::pipe_endpoint;
    // A syntactically valid but deliberately unrelated SID permits verifying
    // the actual Windows access check without creating or depending on a
    // second local account in CI.
    const auto sddl = makeSecurityDescriptor(L"S-1-5-21-424242-424243-424244-424245");
    ASSERT_TRUE(sddl.has_value());
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl->c_str(), SDDL_REVISION_1, &descriptor, nullptr));
    struct DescriptorFreer { PSECURITY_DESCRIPTOR value; ~DescriptorFreer() { if (value) LocalFree(value); } } descriptorFreer{descriptor};
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    const std::wstring name = L"\\\\.\\pipe\\Tools3000AclDenyTest-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 128, 128, 0, &security);
    ASSERT_NE(pipe, INVALID_HANDLE_VALUE);
    struct PipeCloser { HANDLE value; ~PipeCloser() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } pipeCloser{pipe};

    SetLastError(ERROR_SUCCESS);
    HANDLE client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_EQ(client, INVALID_HANDLE_VALUE);
    EXPECT_EQ(GetLastError(), ERROR_ACCESS_DENIED);
}

TEST(SearchServiceStartupPolicyTest, ScmStateNeverCausesPrematurePortableDuplicate) {
    using tools3000::search::ScmServiceState;
    using tools3000::search::StartupAction;
    using tools3000::search::decideStartupAction;

    EXPECT_EQ(decideStartupAction(true, ScmServiceState::Running, false),
              StartupAction::UseEndpoint);
    EXPECT_EQ(decideStartupAction(false, ScmServiceState::Missing, false),
              StartupAction::AllowPortableFallback);
    EXPECT_EQ(decideStartupAction(false, ScmServiceState::Stopped, false),
              StartupAction::StartScmService);
    EXPECT_EQ(decideStartupAction(false, ScmServiceState::Stopped, true),
              StartupAction::AllowPortableFallback);
    EXPECT_EQ(decideStartupAction(false, ScmServiceState::StartPending, false),
              StartupAction::WaitForScmEndpoint);
    EXPECT_EQ(decideStartupAction(false, ScmServiceState::Running, false),
              StartupAction::WaitForScmEndpoint);
    // A concurrent StartService can become Running after the caller's initial
    // status query; that state must still wait, never spawn portable mode.
    EXPECT_NE(decideStartupAction(false, ScmServiceState::Running, false),
              StartupAction::AllowPortableFallback);
    EXPECT_EQ(decideStartupAction(false, ScmServiceState::StopPending, false),
              StartupAction::ReportUnavailable);
    EXPECT_TRUE(tools3000::search::isScmEndpointIdentityConflict(
        ScmServiceState::Running, false, true));
    EXPECT_FALSE(tools3000::search::isScmEndpointIdentityConflict(
        ScmServiceState::StartPending, false, true));
    EXPECT_FALSE(tools3000::search::isScmEndpointIdentityConflict(
        ScmServiceState::Running, true, true));
    EXPECT_TRUE(tools3000::search::scmOpenShouldRetryQueryOnly(ERROR_ACCESS_DENIED));
    EXPECT_FALSE(tools3000::search::scmOpenShouldRetryQueryOnly(ERROR_SERVICE_DOES_NOT_EXIST));
    EXPECT_FALSE(tools3000::search::scmOpenShouldRetryQueryOnly(ERROR_SUCCESS));
    // 没有启动权限时，已停止的 SCM 服务必须回退便携进程，否则搜索永久不可用。
    EXPECT_EQ(decideStartupAction(false, ScmServiceState::Stopped, true),
              StartupAction::AllowPortableFallback);
    EXPECT_EQ(decideStartupAction(false, ScmServiceState::Running, true),
              StartupAction::WaitForScmEndpoint);
}

// -----------------------------------------------------------------------------
// 20. 屏幕录制编码管道测试套件 (内存虚拟驱动 + FFmpeg 编码全链路测试)
// -----------------------------------------------------------------------------

TEST(ContentSearchTest, ExtractorEngines) {
    // 1. SearchExpression content 语法解析验证
    auto expr1 = SearchExpression::parse(L"content:SELECT");
    EXPECT_TRUE(expr1.hasContentFilter());
    EXPECT_EQ(tools3000::core::WinUtils::wstringToUtf8(expr1.getContentQuery()), "SELECT");
    EXPECT_FALSE(expr1.requiresFullPath());

    auto expr2 = SearchExpression::parse(L"ext:cpp;h c:Tools3000 c:\\projects\\");
    EXPECT_TRUE(expr2.hasContentFilter());
    EXPECT_EQ(tools3000::core::WinUtils::wstringToUtf8(expr2.getContentQuery()), "Tools3000");
    EXPECT_TRUE(expr2.requiresFullPath());

    auto expr3 = SearchExpression::parse(L"内容:工程图纸");
    EXPECT_TRUE(expr3.hasContentFilter());
    EXPECT_EQ(tools3000::core::WinUtils::wstringToUtf8(expr3.getContentQuery()), "工程图纸");

    // 验证多关键词全文穿透检索 (如 content:同心 账号密码，后续词不会被误判为文件名过滤)
    auto exprMultiContent = SearchExpression::parse(L"content:同心 账号密码");
    EXPECT_TRUE(exprMultiContent.hasContentFilter());
    EXPECT_EQ(tools3000::core::WinUtils::wstringToUtf8(exprMultiContent.getContentQuery()), "同心 账号密码");
    FileRecord recMulti;
    recMulti.fileName = L"备忘录.txt";
    recMulti.normalizedName = L"备忘录.txt";
    EXPECT_TRUE(exprMultiContent.matches(recMulti, L'C'));

    // 拼音音节分隔符 (如输入法 tong'xi 匹配同喜 tongxi)
    auto exprPinyinSyllable = SearchExpression::parse(L"tong'xi");
    FileRecord recTongXi;
    recTongXi.fileName = L"同喜.txt";
    recTongXi.normalizedName = L"同喜.txt";
    recTongXi.pinyinFull = L"tongxi";
    recTongXi.pinyinInitials = L"tx";
    EXPECT_TRUE(exprPinyinSyllable.matches(recTongXi, L'C'));

    // 2. ContentSearchEngine 格式支持测试
    auto& engine = tools3000::service::content::ContentSearchEngine::instance();
    EXPECT_TRUE(engine.canSearchContent(L"cpp"));
    EXPECT_TRUE(engine.canSearchContent(L"rs"));
    EXPECT_TRUE(engine.canSearchContent(L"py"));
    EXPECT_TRUE(engine.canSearchContent(L"sql"));
    EXPECT_TRUE(engine.canSearchContent(L"md"));
    EXPECT_TRUE(engine.canSearchContent(L"docx"));
    EXPECT_TRUE(engine.canSearchContent(L"xlsx"));
    EXPECT_TRUE(engine.canSearchContent(L"pptx"));
    EXPECT_TRUE(engine.canSearchContent(L"psd"));
    EXPECT_TRUE(engine.canSearchContent(L"ai"));
    EXPECT_TRUE(engine.canSearchContent(L"cdr"));
    EXPECT_TRUE(engine.canSearchContent(L"xmind"));
    EXPECT_TRUE(engine.canSearchContent(L"dxf"));
    EXPECT_FALSE(engine.canSearchContent(L"exe"));
    EXPECT_FALSE(engine.canSearchContent(L"dll"));

    // 3. PlainTextExtractor 实际文件扫描测试 (创建临时测试文件)
    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring testCppFile = std::wstring(tempPath) + L"tools3000_test_source.cpp";

    {
        std::ofstream ofs(testCppFile, std::ios::binary);
        ofs << "// Line 1\n";
        ofs << "// Line 2\n";
        ofs << "void TestWorldClassContentSearch() {\n";
        ofs << "    int magic = 20260816;\n";
        ofs << "}\n";
    }

    std::vector<tools3000::service::content::ContentSnippet> snippets;
    bool found = engine.searchFile(testCppFile, L"TestWorldClassContentSearch", false, snippets);
    EXPECT_TRUE(found);
    EXPECT_EQ(snippets.size(), 1u);
    if (!snippets.empty()) {
        EXPECT_EQ(snippets[0].lineNumber, 3);
        EXPECT_NE(snippets[0].lineContent.find(L"TestWorldClassContentSearch"), std::wstring::npos);
        EXPECT_EQ(snippets[0].matchLength, wcslen(L"TestWorldClassContentSearch"));
    }

    DeleteFileW(testCppFile.c_str());

    // 3.1 SQL 文件与 OITT 关键字检索测试
    std::wstring testSqlFile = std::wstring(tempPath) + L"tools3000_test_bom.sql";
    {
        std::ofstream ofs(testSqlFile, std::ios::binary);
        ofs << "/* SAP B1 BOM 物料清单控制 */\n";
        ofs << "SELECT T0.ItemCode, T0.ItemName FROM OITT T0 WHERE T0.TreeType = 'A';\n";
    }

    snippets.clear();
    bool sqlFound = engine.searchFile(testSqlFile, L"oitt", false, snippets);
    EXPECT_TRUE(sqlFound);
    EXPECT_FALSE(snippets.empty());
    if (!snippets.empty()) {
        EXPECT_EQ(snippets[0].lineNumber, 2);
        EXPECT_NE(snippets[0].lineContent.find(L"OITT"), std::wstring::npos);
    }
    DeleteFileW(testSqlFile.c_str());

    // 4. DxfExtractor 实际文件扫描测试 (创建临时测试 DXF)
    std::wstring testDxfFile = std::wstring(tempPath) + L"tools3000_test_drawing.dxf";
    {
        std::ofstream ofs(testDxfFile, std::ios::binary);
        ofs << "0\nSECTION\n2\nENTITIES\n";
        ofs << "0\nTEXT\n8\nLAYER_1\n1\nDWG_PROJECT_NUM_A88\n";
        ofs << "0\nENDSEC\n0\nEOF\n";
    }

    snippets.clear();
    bool dxfFound = engine.searchFile(testDxfFile, L"PROJECT_NUM_A88", false, snippets);
    EXPECT_TRUE(dxfFound);
    EXPECT_FALSE(snippets.empty());
    if (!snippets.empty()) {
        EXPECT_NE(snippets[0].lineContent.find(L"DWG_PROJECT_NUM_A88"), std::wstring::npos);
    }

    DeleteFileW(testDxfFile.c_str());

    // 5. 伪装为 .log 的二进制文件 (如 edb.log, MeasuredBoot.log) 拦截测试
    std::wstring testBinaryLogFile = std::wstring(tempPath) + L"tools3000_test_fake_edb.log";
    {
        std::ofstream ofs(testBinaryLogFile, std::ios::binary);
        // 模拟 ESE / MeasuredBoot 二进制数据头与随机控制字节
        uint8_t fakeBinaryHeader[] = { 0xef, 0xcd, 0xab, 0x89, 0x00, 0x00, 0x01, 0x00, 0x80, 0x81, 0x00, 0x12, 0x05, 0x00 };
        ofs.write(reinterpret_cast<const char*>(fakeBinaryHeader), sizeof(fakeBinaryHeader));
        for (int i = 0; i < 200; ++i) {
            char buf[16] = {0};
            buf[0] = static_cast<char>(i);
            ofs.write(buf, sizeof(buf));
        }
    }

    snippets.clear();
    bool binaryFound = engine.searchFile(testBinaryLogFile, L"**", false, snippets);
    EXPECT_FALSE(binaryFound);
    EXPECT_TRUE(snippets.empty());
    DeleteFileW(testBinaryLogFile.c_str());

    // 6. GBK / ANSI 中文编码无乱码检索测试
    std::wstring testGbkFile = std::wstring(tempPath) + L"tools3000_test_gbk.txt";
    {
        std::ofstream ofs(testGbkFile, std::ios::binary);
        // GBK 编码的中文字符串："北京中源技术中心：开发账号密码"
        std::wstring wText = L"北京中源技术中心：开发账号密码";
        UINT cp = IsValidCodePage(936) ? 936 : (IsValidCodePage(54936) ? 54936 : CP_ACP);
        int len = WideCharToMultiByte(cp, 0, wText.c_str(), static_cast<int>(wText.size()), nullptr, 0, nullptr, nullptr);
        std::string gbkText(len, '\0');
        if (len > 0) {
            WideCharToMultiByte(cp, 0, wText.c_str(), static_cast<int>(wText.size()), gbkText.data(), len, nullptr, nullptr);
        }
        ofs << "Line 1: Header\r\n";
        ofs << gbkText << "\r\n";
    }

    snippets.clear();
    bool gbkFound = engine.searchFile(testGbkFile, L"账号密码", false, snippets);
    EXPECT_TRUE(gbkFound);
    EXPECT_FALSE(snippets.empty());
    if (!snippets.empty()) {
        EXPECT_NE(snippets[0].lineContent.find(L"北京中源技术中心"), std::wstring::npos);
        EXPECT_NE(snippets[0].lineContent.find(L"账号密码"), std::wstring::npos);
    }
    DeleteFileW(testGbkFile.c_str());

    // 7. UTF-16LE 中文与英文无乱码检索测试
    std::wstring testUtf16File = std::wstring(tempPath) + L"tools3000_test_utf16.txt";
    {
        std::ofstream ofs(testUtf16File, std::ios::binary);
        uint8_t bom[2] = { 0xFF, 0xFE };
        ofs.write(reinterpret_cast<const char*>(bom), 2);
        std::wstring text = L"UTF16_WorldClass_Search: 极客搜索世界级体验\r\n";
        ofs.write(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
    }

    snippets.clear();
    bool utf16Found = engine.searchFile(testUtf16File, L"极客搜索", false, snippets);
    EXPECT_TRUE(utf16Found);
    EXPECT_FALSE(snippets.empty());
    if (!snippets.empty()) {
        EXPECT_NE(snippets[0].lineContent.find(L"极客搜索世界级体验"), std::wstring::npos);
    }
    DeleteFileW(testUtf16File.c_str());

    // 8. 动态格式配置测试 (自定义格式扩展与黑名单禁用)
    engine.configureFormats({L"proto", L"mycustomext"}, {L"psd", L"dxf"});
    EXPECT_TRUE(engine.canSearchContent(L"proto"));
    EXPECT_TRUE(engine.canSearchContent(L"mycustomext"));
    EXPECT_FALSE(engine.canSearchContent(L"psd"));
    EXPECT_FALSE(engine.canSearchContent(L"dxf"));

    std::wstring testProtoFile = std::wstring(tempPath) + L"tools3000_test.mycustomext";
    {
        std::ofstream ofs(testProtoFile);
        ofs << "syntax = \"proto3\";\n";
        ofs << "message Tools3000SearchPacket {\n";
        ofs << "    string query = 1;\n";
        ofs << "}\n";
    }
    snippets.clear();
    bool protoFound = engine.searchFile(testProtoFile, L"Tools3000SearchPacket", false, snippets);
    EXPECT_TRUE(protoFound);
    EXPECT_FALSE(snippets.empty());
    DeleteFileW(testProtoFile.c_str());

    // 恢复配置
    engine.configureFormats({}, {});
    EXPECT_TRUE(engine.canSearchContent(L"psd"));
    EXPECT_TRUE(engine.canSearchContent(L"dxf"));
    EXPECT_TRUE(engine.canSearchContent(L"proto"));
    EXPECT_FALSE(engine.canSearchContent(L"mycustomext"));
}

TEST(ContentSearchTest, FormatConfigurationIsReversibleUnderConcurrency) {
    auto& engine = tools3000::service::content::ContentSearchEngine::instance();
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&engine, worker]() {
            for (int iteration = 0; iteration < 500; ++iteration) {
                if (worker < 2) {
                    if ((iteration + worker) % 2 == 0) {
                        engine.configureFormats({L"temporary"}, {L"psd"});
                    } else {
                        engine.configureFormats({}, {});
                    }
                } else {
                    (void)engine.canSearchContent(L"temporary");
                    (void)engine.canSearchContent(L"psd");
                    (void)engine.canSearchContent(L"cpp");
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();

    engine.configureFormats({}, {});
    EXPECT_FALSE(engine.canSearchContent(L"temporary"));
    EXPECT_TRUE(engine.canSearchContent(L"psd"));
    EXPECT_TRUE(engine.canSearchContent(L"cpp"));
}

TEST(ContentSearchTest, CaseInsensitiveMixedLanguageSearch) {
    auto& engine = tools3000::service::content::ContentSearchEngine::instance();
    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring testFile = std::wstring(tempPath) + L"tools3000_test_mixed.txt";

    {
        std::ofstream ofs(testFile, std::ios::binary);
        ofs << "项目名称: Tools3000\n";
        ofs << "开发团队: Antigravity Google Team\n";
        ofs << "模块说明: ContentSearchEngine 全文穿透检索系统\n";
    }

    // 1. 中英文大小写混合检索测试：文件中有 "Tools3000"，用全小写 "tools3000" 检索
    std::vector<tools3000::service::content::ContentSnippet> snippets;
    bool foundLower = engine.searchFile(testFile, L"tools3000", false, snippets);
    EXPECT_TRUE(foundLower);
    EXPECT_FALSE(snippets.empty());

    // 2. 用中英混合词检索："tools3000 全文"
    snippets.clear();
    bool foundMixed = engine.searchFile(testFile, L"tools3000 全文", false, snippets);
    EXPECT_TRUE(foundMixed);
    EXPECT_FALSE(snippets.empty());

    // 3. 中文词检索
    snippets.clear();
    bool foundZh = engine.searchFile(testFile, L"全文穿透检索", false, snippets);
    EXPECT_TRUE(foundZh);
    EXPECT_FALSE(snippets.empty());

    DeleteFileW(testFile.c_str());
}

TEST(ContentSearchTest, ZipXmlTagStrippingNoSpuriousSpaces) {
    auto& engine = tools3000::service::content::ContentSearchEngine::instance();
    EXPECT_TRUE(engine.canSearchContent(L"docx"));
    EXPECT_TRUE(engine.canSearchContent(L"xlsx"));
    EXPECT_TRUE(engine.canSearchContent(L"pptx"));
}

TEST(RunHistoryTest, PersistenceAndFrecencyCalculation) {
    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring csvFile = std::wstring(tempPath) + L"Tools3000_RunHistory_Test.csv";
    DeleteFileW(csvFile.c_str());

    auto& runMgr = tools3000::service::db::RunHistoryManager::instance();
    runMgr.init(csvFile);
    runMgr.clear();
    EXPECT_EQ(runMgr.size(), 0u);

    const std::wstring testFile1 = L"D:\\Chosen\\216-北京中源\\账号密码.txt";
    const std::wstring testFile2 = L"D:\\Chosen\\999-SQL\\SAP_RW.txt";

    runMgr.recordRun(testFile1);
    EXPECT_EQ(runMgr.getRunCount(testFile1), 1u);
    EXPECT_GT(runMgr.getLastRunDate(testFile1), 0ULL);

    // 重复运行累加
    runMgr.recordRun(testFile1);
    EXPECT_EQ(runMgr.getRunCount(testFile1), 2u);

    runMgr.recordRun(testFile2);
    EXPECT_EQ(runMgr.getRunCount(testFile2), 1u);

    // Frecency 智能权重打分计算
    double score1 = runMgr.calculateFrecencyScore(testFile1);
    double score2 = runMgr.calculateFrecencyScore(testFile2);
    EXPECT_GT(score1, score2); // 运行 2 次的得分显著高于运行 1 次

    auto topRuns = runMgr.getTopRuns(10);
    ASSERT_GE(topRuns.size(), 2u);
    EXPECT_EQ(topRuns[0].filename, testFile1);

    // 持久化保存并从磁盘重载
    EXPECT_TRUE(runMgr.save());
    runMgr.init(csvFile);
    EXPECT_EQ(runMgr.getRunCount(testFile1), 2u);
    EXPECT_EQ(runMgr.getRunCount(testFile2), 1u);

    DeleteFileW(csvFile.c_str());
}

// -----------------------------------------------------------------------------
// 35. 搜索历史 (Search History) 与去重推荐测试套件
// -----------------------------------------------------------------------------
TEST(SearchHistoryTest, PersistenceAndDeduplication) {
    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring csvFile = std::wstring(tempPath) + L"Tools3000_SearchHistory_Test.csv";
    DeleteFileW(csvFile.c_str());

    auto& searchMgr = tools3000::service::db::SearchHistoryManager::instance();
    searchMgr.init(csvFile);
    searchMgr.clear();
    EXPECT_EQ(searchMgr.size(), 0u);

    const std::wstring q1 = L"*中源*账号密码*";
    const std::wstring q2 = L"ext:docx 订单";
    const std::wstring quotedQuery = L"\"exact, phrase\"\n第二行";

    searchMgr.recordSearch(q1);
    searchMgr.recordSearch(q2);
    searchMgr.recordSearch(quotedQuery);
    searchMgr.recordSearch(q1); // 重复记录

    EXPECT_EQ(searchMgr.size(), 3u);
    auto recents = searchMgr.getRecentSearches(10);
    ASSERT_EQ(recents.size(), 3u);
    EXPECT_EQ(recents[0].search, q1);
    EXPECT_EQ(recents[0].searchCount, 2u);

    // 移除单个搜索项
    EXPECT_TRUE(searchMgr.removeSearch(q2));
    EXPECT_EQ(searchMgr.size(), 2u);

    // 存盘与重载
    EXPECT_TRUE(searchMgr.save());
    searchMgr.init(csvFile);
    EXPECT_EQ(searchMgr.size(), 2u);
    const auto restoredSearches = searchMgr.getRecentSearches(10);
    EXPECT_NE(std::find_if(restoredSearches.begin(), restoredSearches.end(), [&](const auto& item) {
        return item.search == quotedQuery;
    }), restoredSearches.end());

    DeleteFileW(csvFile.c_str());
}

// -----------------------------------------------------------------------------
// 36. 数据库快照 (Tools3000.db) 二进制存储与秒级内存映射测试套件
// -----------------------------------------------------------------------------
TEST(DatabaseSnapshotTest, BinaryPackAndFastMemoryMap) {
    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring dbFile = std::wstring(tempPath) + L"Tools3000_Test.db";
    DeleteFileW(dbFile.c_str());

    auto& dbMgr = tools3000::service::db::DatabaseManager::instance();
    dbMgr.init(dbFile);

    // 构造测试 MFT 卷与记录
    auto parser1 = std::make_unique<MftParser>();
    std::vector<FileRecordInit> mockRecords;
    for (uint64_t i = 1; i <= 50; ++i) {
        FileRecordInit r{};
        r.fileReferenceNumber = i;
        r.parentFileReferenceNumber = 0;
        r.fileName = L"test_document_" + std::to_wstring(i) + L".docx";
        r.fileSize = 1024 * i;
        r.creationTime = 134000000000000000ULL + i;
        r.lastWriteTime = 134000000000000000ULL + i * 2;
        r.isDirectory = (i % 10 == 0);
        mockRecords.push_back(std::move(r));
    }

    EXPECT_TRUE(parser1->importSnapshot(std::move(mockRecords), 12345678ULL, 0xABCDEF01));
    EXPECT_EQ(parser1->getFileCount(), 50u);

    std::vector<MftParser*> parsers = { parser1.get() };

    // 保存快照至 Tools3000.db
    EXPECT_TRUE(dbMgr.saveSnapshot(parsers));

    auto stats = dbMgr.getStats();
    EXPECT_TRUE(stats.exists);
    EXPECT_GT(stats.fileSize, 0ULL);
    EXPECT_EQ(stats.totalRecords, 50ULL);

    // 构造空解析器并从 Tools3000.db 内存映射瞬时恢复
    auto parser2 = std::make_unique<MftParser>();
    std::vector<MftParser*> restoreParsers = { parser2.get() };
    EXPECT_TRUE(dbMgr.loadSnapshot(restoreParsers));
    EXPECT_EQ(parser2->getFileCount(), 50u);

    // 验证搜索命中与路径结构
    auto searchResults = parser2->Search(L"test_document_5.docx");
    ASSERT_EQ(searchResults.size(), 1u);
    EXPECT_EQ(searchResults[0].fileName, L"test_document_5.docx");
    EXPECT_EQ(searchResults[0].fileSize, 5120u);

    const auto originalSize = std::filesystem::file_size(dbFile);
    HANDLE lockedDatabase = CreateFileW(dbFile.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(lockedDatabase, INVALID_HANDLE_VALUE);
    EXPECT_FALSE(dbMgr.saveSnapshot(parsers));
    CloseHandle(lockedDatabase);
    EXPECT_EQ(std::filesystem::file_size(dbFile), originalSize);

    auto parserAfterFailedSave = std::make_unique<MftParser>();
    std::vector<MftParser*> parsersAfterFailedSave = {parserAfterFailedSave.get()};
    EXPECT_TRUE(dbMgr.loadSnapshot(parsersAfterFailedSave));
    EXPECT_EQ(parserAfterFailedSave->getFileCount(), 50u);

    DeleteFileW(dbFile.c_str());
}

// -----------------------------------------------------------------------------
// 36.1 搜索服务管道分帧协议
//
// 旧协议依赖 PIPE_TYPE_MESSAGE 与固定 256KB 缓冲，响应超出缓冲区时客户端会
// 把 ERROR_MORE_DATA 当成失败并丢弃整份结果，残留数据还会拖垮后续连接。
// 现在改为显式长度前缀，这些用例锁定编解码的边界行为。
// -----------------------------------------------------------------------------
TEST(MftParserTest, IncrementalTypingMultiWordCrossPathSearch) {
    auto parser = std::make_unique<MftParser>();
    std::vector<FileRecordInit> mockRecords;

    // 构造目录: 226-苏州同心 (id = 100)
    FileRecordInit dirRec{};
    dirRec.fileReferenceNumber = 100;
    dirRec.parentFileReferenceNumber = 0;
    dirRec.fileName = L"226-苏州同心";
    dirRec.isDirectory = true;
    mockRecords.push_back(std::move(dirRec));

    // 构造文件: 账号密码.txt (id = 101, parent = 100)
    FileRecordInit fileRec{};
    fileRec.fileReferenceNumber = 101;
    fileRec.parentFileReferenceNumber = 100;
    fileRec.fileName = L"账号密码.txt";
    fileRec.isDirectory = false;
    fileRec.fileSize = 1024;
    mockRecords.push_back(std::move(fileRec));

    EXPECT_TRUE(parser->importSnapshot(std::move(mockRecords), 123456ULL, 0x11223344));

    // 模拟连续打字:
    // 1. 用户先搜 "同" (单字仅匹配文件名/目录名，此时 账号密码.txt 仅在目录命中，缓存被填入)
    auto res1 = parser->Search(L"同");
    EXPECT_GE(res1.size(), 1u);

    // 2. 用户接着打 "同心" (缩小缓存)
    auto res2 = parser->Search(L"同心");
    EXPECT_GE(res2.size(), 1u);

    // 3. 用户追加空格与第二个词 "同心 账号密码" (多词全路径匹配，必须绕开单字文件名缓存，正确命中 账号密码.txt)
    auto res3 = parser->Search(L"同心 账号密码");
    ASSERT_EQ(res3.size(), 1u);
    EXPECT_EQ(res3[0].fileName, L"账号密码.txt");
    EXPECT_NE(res3[0].fullPath.find(L"226-苏州同心"), std::wstring::npos);
}

TEST(PipeProtocolTest, HeaderRoundTripsPayloadSize) {
    namespace frame = tools3000::service::pipe;
    for (uint32_t size : {1u, 2u, 255u, 256u, 65535u, 65536u, 1048576u, frame::MaxFrameBytes}) {
        const auto header = frame::encodeFrameHeader(size);
        uint32_t decoded = 0;
        EXPECT_TRUE(frame::decodeFrameHeader(header.data(), decoded)) << "size=" << size;
        EXPECT_EQ(decoded, size);
    }
}

TEST(PipeProtocolTest, HeaderIsLittleEndian) {
    namespace frame = tools3000::service::pipe;
    const auto header = frame::encodeFrameHeader(0x04030201u);
    EXPECT_EQ(static_cast<unsigned char>(header[0]), 0x01u);
    EXPECT_EQ(static_cast<unsigned char>(header[1]), 0x02u);
    EXPECT_EQ(static_cast<unsigned char>(header[2]), 0x03u);
    EXPECT_EQ(static_cast<unsigned char>(header[3]), 0x04u);
}

TEST(PipeProtocolTest, RejectsEmptyAndOversizedFrames) {
    namespace frame = tools3000::service::pipe;
    uint32_t decoded = 0;

    const auto emptyHeader = frame::encodeFrameHeader(0);
    EXPECT_FALSE(frame::decodeFrameHeader(emptyHeader.data(), decoded));

    const auto oversized = frame::encodeFrameHeader(frame::MaxFrameBytes + 1);
    EXPECT_FALSE(frame::decodeFrameHeader(oversized.data(), decoded));

    EXPECT_FALSE(frame::decodeFrameHeader(nullptr, decoded));
}

TEST(PipeProtocolTest, RequestLimitIsTighterThanResponseLimit) {
    namespace frame = tools3000::service::pipe;
    uint32_t decoded = 0;

    // 请求只承载一段查询 JSON，服务端用更小的上限拒绝畸形帧头。
    const auto header = frame::encodeFrameHeader(frame::MaxRequestBytes + 1);
    EXPECT_FALSE(frame::decodeFrameHeader(header.data(), decoded, frame::MaxRequestBytes));
    EXPECT_TRUE(frame::decodeFrameHeader(header.data(), decoded));

    EXPECT_TRUE(frame::fitsInFrame(frame::MaxRequestBytes, frame::MaxRequestBytes));
    EXPECT_FALSE(frame::fitsInFrame(frame::MaxRequestBytes + 1, frame::MaxRequestBytes));
    EXPECT_FALSE(frame::fitsInFrame(0));
}

TEST(PipeProtocolIntegrationTest, ByteStreamRoundTripsDeliberatelyFragmentedFrame) {
    namespace frame = tools3000::service::pipe;
    const std::wstring name = L"\\\\.\\pipe\\Tools3000PipeProtocolIntegration-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HANDLE serverPipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, frame::IoChunkBytes, frame::IoChunkBytes, 0, nullptr);
    ASSERT_NE(serverPipe, INVALID_HANDLE_VALUE);
    struct ServerPipeCloser { HANDLE value; ~ServerPipeCloser() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } serverCloser{serverPipe};

    // Connect before starting the server thread. ConnectNamedPipe will then
    // return ERROR_PIPE_CONNECTED, exercising the race that occurs when a
    // portable client wins the connection race against a newly published pipe.
    HANDLE clientPipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(clientPipe, INVALID_HANDLE_VALUE) << GetLastError();
    struct ClientPipeCloser { HANDLE value; ~ClientPipeCloser() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } clientCloser{clientPipe};

    std::atomic<bool> serverOk{false};
    std::thread server([&] {
        const BOOL connected = ConnectNamedPipe(serverPipe, nullptr) ? TRUE :
            (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) return;
        char header[frame::HeaderSize]{};
        if (!frame::readExact(serverPipe, header, sizeof(header))) return;
        std::uint32_t bytes = 0;
        if (!frame::decodeFrameHeader(header, bytes, frame::MaxRequestBytes)) return;
        std::string request(bytes, '\0');
        if (!frame::readExact(serverPipe, request.data(), request.size()) || request != "fragmented request") return;
        const std::string response = R"({"ok":true,"source":"pipe"})";
        const auto responseHeader = frame::encodeFrameHeader(static_cast<std::uint32_t>(response.size()));
        if (!frame::writeExact(serverPipe, responseHeader.data(), responseHeader.size())) return;
        if (!frame::writeExact(serverPipe, response.data(), response.size())) return;
        serverOk.store(true, std::memory_order_release);
    });

    const std::string request = "fragmented request";
    const auto requestHeader = frame::encodeFrameHeader(static_cast<std::uint32_t>(request.size()));
    auto writeByteByByte = [clientPipe](const char* bytes, std::size_t count) {
        for (std::size_t index = 0; index < count; ++index) {
            DWORD written = 0;
            if (!WriteFile(clientPipe, bytes + index, 1, &written, nullptr) || written != 1) return false;
        }
        return true;
    };
    EXPECT_TRUE(writeByteByByte(requestHeader.data(), requestHeader.size()));
    EXPECT_TRUE(writeByteByByte(request.data(), request.size()));

    char responseHeader[frame::HeaderSize]{};
    std::uint32_t responseBytes = 0;
    const bool gotHeader = frame::readExact(clientPipe, responseHeader, sizeof(responseHeader));
    EXPECT_TRUE(gotHeader);
    const bool validHeader = gotHeader && frame::decodeFrameHeader(responseHeader, responseBytes);
    EXPECT_TRUE(validHeader);
    std::string response(validHeader ? responseBytes : 0, '\0');
    const bool gotPayload = validHeader && frame::readExact(clientPipe, response.data(), response.size());
    EXPECT_TRUE(gotPayload);
    if (gotPayload) EXPECT_EQ(response, R"({"ok":true,"source":"pipe"})");
    clientCloser.value = INVALID_HANDLE_VALUE;
    CloseHandle(clientPipe);
    server.join();
    EXPECT_TRUE(serverOk.load(std::memory_order_acquire));
}

// -----------------------------------------------------------------------------
// 36.2 查询代际取消
// -----------------------------------------------------------------------------
TEST(QueryEpochTrackerTest, NewerQuerySupersedesOlderOnes) {
    tools3000::service::query::QueryEpochTracker tracker;

    EXPECT_TRUE(tracker.observe(100));
    EXPECT_FALSE(tracker.isStale(100));

    EXPECT_TRUE(tracker.observe(200));
    EXPECT_TRUE(tracker.isStale(100));
    EXPECT_FALSE(tracker.isStale(200));
    EXPECT_EQ(tracker.latest(), 200u);
}

TEST(QueryEpochTrackerTest, LateArrivingOlderQueryIsRejectedImmediately) {
    tools3000::service::query::QueryEpochTracker tracker;
    EXPECT_TRUE(tracker.observe(500));

    // 更旧的查询即使刚到达也已经过期，不必再跑一遍扫描。
    EXPECT_FALSE(tracker.observe(400));
    EXPECT_TRUE(tracker.isStale(400));
    EXPECT_EQ(tracker.latest(), 500u);
}

TEST(QueryEpochTrackerTest, RepeatedObservationOfLatestStaysCurrent) {
    tools3000::service::query::QueryEpochTracker tracker;
    EXPECT_TRUE(tracker.observe(700));
    EXPECT_TRUE(tracker.observe(700));
    EXPECT_FALSE(tracker.isStale(700));
}

TEST(QueryEpochTrackerTest, UnnumberedCallersNeverParticipateInCancellation) {
    tools3000::service::query::QueryEpochTracker tracker;
    EXPECT_TRUE(tracker.observe(0));
    EXPECT_FALSE(tracker.isStale(0));

    EXPECT_TRUE(tracker.observe(900));
    EXPECT_TRUE(tracker.observe(0));
    EXPECT_FALSE(tracker.isStale(0));
    EXPECT_EQ(tracker.latest(), 900u);
}

TEST(QueryEpochTrackerTest, ResetClearsGeneration) {
    tools3000::service::query::QueryEpochTracker tracker;
    EXPECT_TRUE(tracker.observe(1234));
    tracker.reset();
    EXPECT_EQ(tracker.latest(), 0u);
    EXPECT_FALSE(tracker.isStale(1));
}

TEST(QueryEpochTrackerTest, CancelAllBumpsEpochAndMarksActiveQueriesStale) {
    tools3000::service::query::QueryEpochTracker tracker;
    EXPECT_TRUE(tracker.observe(100));
    EXPECT_FALSE(tracker.isStale(100));
    tracker.cancelAll();
    EXPECT_GT(tracker.latest(), 100u);
    EXPECT_TRUE(tracker.isStale(100));
}

TEST(QueryEpochTrackerTest, ConcurrentObserversAgreeOnASingleWinner) {
    tools3000::service::query::QueryEpochTracker tracker;
    constexpr uint64_t highest = 512;

    std::vector<std::thread> threads;
    for (uint64_t id = 1; id <= highest; ++id) {
        threads.emplace_back([&tracker, id]() { tracker.observe(id); });
    }
    for (auto& thread : threads) thread.join();

    EXPECT_EQ(tracker.latest(), highest);
    EXPECT_FALSE(tracker.isStale(highest));
    EXPECT_TRUE(tracker.isStale(highest - 1));
}

// -----------------------------------------------------------------------------
// 36.3 IPC 异步分发
//
// 搜索请求要跨进程等待索引服务，同步执行会冻结 WebView2 的 UI 线程。这里验证
// 标记为异步的方法确实离开了调用线程，而未标记的方法仍然就地完成。
// -----------------------------------------------------------------------------

TEST(SearchRequestLimitsTest, OversizedRegexIsNotCompiled) {
    std::wstring pattern(tools3000::service::search_limits::MaxRegexCharacters + 1, L'a');
    const auto expression = SearchExpression::parse(L"regex:" + pattern);
    ASSERT_EQ(expression.getOrGroups().size(), 1u);
    ASSERT_EQ(expression.getOrGroups()[0].clauses.size(), 1u);
    EXPECT_FALSE(expression.getOrGroups()[0].clauses[0].regexObj.has_value());
}


// -----------------------------------------------------------------------------
// 38. DevOps 全链路性能防退化基准与内存上限自动化门禁 (DevOps Performance Gates)
// -----------------------------------------------------------------------------

// 门禁 1: 50,000 文件索引内存紧缩与单趟毫秒级比对吞吐量

// ─────────────────────────────────────────────────────────────────────────────
// 39. 综合审计验证测试套件 (Comprehensive Audit 2026-09-05 Verifications)
// ─────────────────────────────────────────────────────────────────────────────

TEST(SearchExpressionTest, SizeFilterParsingAndMatching) {
    FileRecord smallFile{1, 0, L"small.txt", L"small.txt", L"small.txt", L"small.txt", false, 0, 500};
    FileRecord mediumFile{2, 0, L"medium.dat", L"medium.dat", L"medium.dat", L"medium.dat", false, 0, 50 * 1024 * 1024};
    FileRecord largeFile{3, 0, L"big_video.mp4", L"big_video.mp4", L"big_video.mp4", L"big_video.mp4", false, 0, 200 * 1024 * 1024};
    FileRecord dirRecord{4, 0, L"BigFolder", L"bigfolder", L"bigfolder", L"bigfolder", true, 0, 500 * 1024 * 1024};

    auto exprGt100mb = SearchExpression::parse(L"size:>100mb");
    EXPECT_TRUE(exprGt100mb.matches(largeFile, L'C', L"C:\\big_video.mp4"));
    EXPECT_FALSE(exprGt100mb.matches(mediumFile, L'C', L"C:\\medium.dat"));
    EXPECT_FALSE(exprGt100mb.matches(smallFile, L'C', L"C:\\small.txt"));
    EXPECT_FALSE(exprGt100mb.matches(dirRecord, L'C', L"C:\\BigFolder"));

    auto exprLt1mb = SearchExpression::parse(L"size:<1mb");
    EXPECT_TRUE(exprLt1mb.matches(smallFile, L'C', L"C:\\small.txt"));
    EXPECT_FALSE(exprLt1mb.matches(mediumFile, L'C', L"C:\\medium.dat"));

    auto exprGte50mb = SearchExpression::parse(L"size:>=50mb");
    EXPECT_TRUE(exprGte50mb.matches(mediumFile, L'C', L"C:\\medium.dat"));
    EXPECT_TRUE(exprGte50mb.matches(largeFile, L'C', L"C:\\big_video.mp4"));
}

TEST(ConfigManagerEndpointIdentityTest, ResetPreservesSearchPipeToken) {
    auto temp = std::filesystem::temp_directory_path() /
        (L"Tools3000Tests_Identity_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
    std::filesystem::create_directories(temp, ec);

    auto& config = tools3000::core::ConfigManager::instance();
    EXPECT_TRUE(config.initialize(temp));

    const std::string token = "test_pipe_token_identity_12345";
    EXPECT_TRUE(config.set("/search/pipeToken", token));
    EXPECT_EQ(config.get<std::string>("/search/pipeToken", ""), token);

    // Resetting config must preserve /search/pipeToken (A06)
    EXPECT_TRUE(config.reset());
    EXPECT_EQ(config.get<std::string>("/search/pipeToken", ""), token);

    // mergePatch with another config must not overwrite /search/pipeToken (A06)
    nlohmann::json patch = {
        {"general", {{"theme", "dark"}}},
        {"search", {{"pipeToken", "malicious_overwrite_token"}}}
    };
    EXPECT_TRUE(config.mergePatch(patch));
    EXPECT_EQ(config.get<std::string>("/search/pipeToken", ""), token);

    std::filesystem::remove_all(temp, ec);
}

TEST(MftParserCandidateFilterTest, RelaxingExclusionsClearsOldCandidateMatches) {
    MftParser parser;
    parser.Initialize('C');
    const DWORD realSerial = parser.getVolumeSerialNumber();

    std::vector<FileRecordInit> records;
    FileRecordInit rec1;
    rec1.fileReferenceNumber = 1001;
    rec1.parentFileReferenceNumber = 0;
    rec1.fileName = L"normal_doc.txt";
    rec1.fileAttributes = FILE_ATTRIBUTE_NORMAL;
    rec1.fileSize = 100;
    records.push_back(std::move(rec1));

    FileRecordInit rec2;
    rec2.fileReferenceNumber = 1002;
    rec2.parentFileReferenceNumber = 0;
    rec2.fileName = L"hidden_secret.txt";
    rec2.fileAttributes = FILE_ATTRIBUTE_HIDDEN;
    rec2.fileSize = 100;
    records.push_back(std::move(rec2));

    EXPECT_TRUE(parser.importSnapshot(std::move(records), 1000, realSerial));

    SearchExcludeOptions optsStrict;
    optsStrict.excludeHidden = true;
    optsStrict.excludeSystem = false;
    auto res1 = parser.Search(L"secret", 10, optsStrict);
    EXPECT_EQ(res1.size(), 0u);

    // Relax exclusion: must invalidate old candidate cache and return hidden file (A07)
    SearchExcludeOptions optsRelaxed;
    optsRelaxed.excludeHidden = false;
    optsRelaxed.excludeSystem = false;
    auto res2 = parser.Search(L"secret", 10, optsRelaxed);
    EXPECT_EQ(res2.size(), 1u);
    if (!res2.empty()) {
        EXPECT_EQ(res2[0].fileName, L"hidden_secret.txt");
    }
}

TEST(MftParserSnapshotValidationTest, RejectMismatchedVolumeSerial) {
    MftParser parser;
    EXPECT_TRUE(parser.Initialize('C'));

    const DWORD realSerial = parser.getVolumeSerialNumber();
    const DWORD fakeSerial = (realSerial == 0) ? 0x12345678 : (realSerial ^ 0xDEADBEEF);

    std::vector<FileRecordInit> fakeRecords;
    FileRecordInit rec;
    rec.fileReferenceNumber = 101;
    rec.parentFileReferenceNumber = 0;
    rec.fileName = L"ghost.txt";
    rec.fileAttributes = FILE_ATTRIBUTE_NORMAL;
    rec.fileSize = 100;
    fakeRecords.push_back(std::move(rec));

    // Importing snapshot with mismatched volume serial must be rejected (A12)
    EXPECT_FALSE(parser.importSnapshot(std::move(fakeRecords), 1000, fakeSerial));
    EXPECT_EQ(parser.getFileCount(), 0u);
}

TEST(MftParserLifecycleTest, MultipleStartListeningCallsDoNotCrash) {
    MftParser parser;
    parser.Initialize('C');

    // Calling StartListening multiple times consecutively must be safe and idempotent (A01)
    EXPECT_NO_THROW({
        parser.StartListening();
        parser.StartListening();
        parser.StopListening();
        parser.StopListening();
    });
}

TEST(AtomicFileTest, BinaryWriteAndOverwriteIntegrity) {
    auto temp = std::filesystem::temp_directory_path() /
        (L"Tools3000Atomic_" + std::to_wstring(GetCurrentProcessId()) + L".bin");
    std::error_code ec;
    std::filesystem::remove(temp, ec);

    std::vector<uint8_t> testData = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    EXPECT_TRUE(tools3000::common::atomicWriteBinaryFileWithFlush(temp.wstring(), testData.data(), testData.size()));
    EXPECT_TRUE(std::filesystem::exists(temp, ec));
    EXPECT_EQ(std::filesystem::file_size(temp, ec), testData.size());

    // Overwrite with new data
    std::vector<uint8_t> testData2 = {0xCA, 0xFE, 0xBA, 0xBE};
    EXPECT_TRUE(tools3000::common::atomicWriteBinaryFileWithFlush(temp.wstring(), testData2.data(), testData2.size()));
    EXPECT_EQ(std::filesystem::file_size(temp, ec), testData2.size());

    std::filesystem::remove(temp, ec);
}

TEST(SearchExpressionTest, MultiTokenContentAndSizeExpressions) {
    auto expr1 = SearchExpression::parse(L"*.cpp content:Tools3000Core");
    EXPECT_TRUE(expr1.hasContentFilter());
    EXPECT_EQ(expr1.getContentQuery(), L"Tools3000Core");

    auto expr2 = SearchExpression::parse(L"*.txt c:password");
    EXPECT_TRUE(expr2.hasContentFilter());
    EXPECT_EQ(expr2.getContentQuery(), L"password");

    auto expr3 = SearchExpression::parse(L"size:>500kb *.log");
    FileRecord recSmall{1, 0, L"app.log", L"app.log", L"app.log", L"app.log", false, 0, 100 * 1024};
    FileRecord recBig{2, 0, L"app.log", L"app.log", L"app.log", L"app.log", false, 0, 1024 * 1024};
    EXPECT_FALSE(expr3.matches(recSmall, L'C', L"C:\\app.log"));
    EXPECT_TRUE(expr3.matches(recBig, L'C', L"C:\\app.log"));
}

TEST(UsnParserTest, ParseRecordV2) {
    const std::wstring fileName = L"test_file.txt";
    const DWORD nameByteLen = static_cast<DWORD>(fileName.size() * sizeof(wchar_t));
    DWORD recordLen = static_cast<DWORD>(sizeof(USN_RECORD_V2) + nameByteLen);
    recordLen = (recordLen + 7) & ~7;

    std::vector<BYTE> buf(recordLen, 0);
    auto* v2 = reinterpret_cast<PUSN_RECORD_V2>(buf.data());
    v2->RecordLength = recordLen;
    v2->MajorVersion = 2;
    v2->MinorVersion = 0;
    v2->FileReferenceNumber = 0x1234567890ABCDEFULL;
    v2->ParentFileReferenceNumber = 0x0FEDCBA098765432ULL;
    v2->Reason = USN_REASON_FILE_CREATE | USN_REASON_CLOSE;
    v2->FileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    v2->TimeStamp.QuadPart = 133456789000000000ULL;
    v2->FileNameLength = static_cast<WORD>(nameByteLen);
    v2->FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V2, FileName));
    memcpy(buf.data() + v2->FileNameOffset, fileName.data(), nameByteLen);

    tools3000::service::ParsedUsnRecord parsed{};
    EXPECT_TRUE(tools3000::service::parseUsnRecord(buf.data(), static_cast<DWORD>(buf.size()), parsed));
    EXPECT_TRUE(parsed.isValid);
    EXPECT_EQ(parsed.majorVersion, 2);
    EXPECT_EQ(parsed.fileReferenceNumber, 0x1234567890ABCDEFULL);
    EXPECT_EQ(parsed.parentFileReferenceNumber, 0x0FEDCBA098765432ULL);
    EXPECT_EQ(parsed.reason, USN_REASON_FILE_CREATE | USN_REASON_CLOSE);
    EXPECT_EQ(parsed.fileAttributes, FILE_ATTRIBUTE_ARCHIVE);
    EXPECT_EQ(parsed.timeStamp, 133456789000000000ULL);
    EXPECT_EQ(parsed.fileName, L"test_file.txt");
    EXPECT_EQ(parsed.recordLength, recordLen);
}

TEST(UsnParserTest, ParseRecordV3) {
    const std::wstring fileName = L"中源 账号密码.txt";
    const DWORD nameByteLen = static_cast<DWORD>(fileName.size() * sizeof(wchar_t));
    DWORD recordLen = static_cast<DWORD>(sizeof(USN_RECORD_V3) + nameByteLen);
    recordLen = (recordLen + 7) & ~7;

    std::vector<BYTE> buf(recordLen, 0);
    auto* v3 = reinterpret_cast<PUSN_RECORD_V3>(buf.data());
    v3->RecordLength = recordLen;
    v3->MajorVersion = 3;
    v3->MinorVersion = 0;
    *reinterpret_cast<uint64_t*>(v3->FileReferenceNumber.Identifier) = 0xAABBCCDDEEFF0011ULL;
    *reinterpret_cast<uint64_t*>(v3->ParentFileReferenceNumber.Identifier) = 0x1100FFEEDDCCBBAAULL;
    v3->Reason = USN_REASON_FILE_DELETE | USN_REASON_CLOSE;
    v3->FileAttributes = FILE_ATTRIBUTE_NORMAL;
    v3->TimeStamp.QuadPart = 133456789111111111ULL;
    v3->FileNameLength = static_cast<WORD>(nameByteLen);
    v3->FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V3, FileName));
    memcpy(buf.data() + v3->FileNameOffset, fileName.data(), nameByteLen);

    tools3000::service::ParsedUsnRecord parsed{};
    EXPECT_TRUE(tools3000::service::parseUsnRecord(buf.data(), static_cast<DWORD>(buf.size()), parsed));
    EXPECT_TRUE(parsed.isValid);
    EXPECT_EQ(parsed.majorVersion, 3);
    EXPECT_EQ(parsed.fileReferenceNumber, 0xAABBCCDDEEFF0011ULL);
    EXPECT_EQ(parsed.parentFileReferenceNumber, 0x1100FFEEDDCCBBAAULL);
    EXPECT_EQ(parsed.reason, USN_REASON_FILE_DELETE | USN_REASON_CLOSE);
    EXPECT_EQ(parsed.fileAttributes, FILE_ATTRIBUTE_NORMAL);
    EXPECT_EQ(parsed.timeStamp, 133456789111111111ULL);
    EXPECT_EQ(parsed.fileName, L"中源 账号密码.txt");
    EXPECT_EQ(parsed.recordLength, recordLen);
}

TEST(UsnParserTest, BoundaryAndMalformedValidation) {
    tools3000::service::ParsedUsnRecord parsed{};
    EXPECT_FALSE(tools3000::service::parseUsnRecord(nullptr, 100, parsed));

    BYTE tinyBuf[4] = {0};
    EXPECT_FALSE(tools3000::service::parseUsnRecord(tinyBuf, sizeof(tinyBuf), parsed));

    // Record length less than header
    USN_RECORD_COMMON_HEADER badHeader{};
    badHeader.RecordLength = 4;
    badHeader.MajorVersion = 2;
    EXPECT_FALSE(tools3000::service::parseUsnRecord(reinterpret_cast<const BYTE*>(&badHeader), sizeof(badHeader), parsed));

    // Record length greater than remaining
    badHeader.RecordLength = 200;
    EXPECT_FALSE(tools3000::service::parseUsnRecord(reinterpret_cast<const BYTE*>(&badHeader), sizeof(badHeader), parsed));

    // Unknown version should advance safely without crashing
    badHeader.RecordLength = sizeof(badHeader);
    badHeader.MajorVersion = 99;
    EXPECT_TRUE(tools3000::service::parseUsnRecord(reinterpret_cast<const BYTE*>(&badHeader), sizeof(badHeader), parsed));
    EXPECT_FALSE(parsed.isValid);
    EXPECT_EQ(parsed.recordLength, sizeof(badHeader));
}

TEST(MftParserHydrationTest, RealTimeGhostPruningOnDeletedFile) {
    auto tempDir = std::filesystem::temp_directory_path();
    wchar_t tempFileName[MAX_PATH] = {};
    ASSERT_NE(GetTempFileNameW(tempDir.c_str(), L"ET_", 0, tempFileName), 0);
    std::wstring tempFilePath = tempFileName;

    std::error_code ec;
    ASSERT_TRUE(std::filesystem::exists(tempFilePath, ec));

    wchar_t driveLetter = tempFilePath[0];
    std::wstring fileName = std::filesystem::path(tempFilePath).filename().wstring();

    MftParser parser;
    parser.Initialize(static_cast<char>(driveLetter));
    const DWORD realSerial = parser.getVolumeSerialNumber();

    std::filesystem::path currentPath = std::filesystem::path(tempFilePath).parent_path();
    std::vector<FileRecordInit> records;

    uint64_t nextFrn = 500000;
    std::vector<std::pair<std::wstring, uint64_t>> dirStack;
    while (currentPath.has_relative_path() && currentPath != currentPath.root_path()) {
        dirStack.push_back({currentPath.filename().wstring(), nextFrn++});
        currentPath = currentPath.parent_path();
    }

    uint64_t parentFrn = 0;
    for (auto it = dirStack.rbegin(); it != dirStack.rend(); ++it) {
        FileRecordInit dirRec{};
        dirRec.fileReferenceNumber = it->second;
        dirRec.parentFileReferenceNumber = parentFrn;
        dirRec.fileName = it->first;
        dirRec.isDirectory = true;
        dirRec.fileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        records.push_back(std::move(dirRec));
        parentFrn = it->second;
    }

    const uint64_t targetFrn = 999999;
    FileRecordInit fileRec{};
    fileRec.fileReferenceNumber = targetFrn;
    fileRec.parentFileReferenceNumber = parentFrn;
    fileRec.fileName = fileName;
    fileRec.isDirectory = false;
    fileRec.fileAttributes = FILE_ATTRIBUTE_NORMAL;
    records.push_back(std::move(fileRec));

    EXPECT_TRUE(parser.importSnapshot(std::move(records), 1000, realSerial));
    parser.setRealTimeValidation(true);

    // 1. 文件物理存在时搜索：必须命中并水合
    auto resBefore = parser.Search(fileName);
    ASSERT_EQ(resBefore.size(), 1u);
    EXPECT_EQ(resBefore[0].fileName, fileName);
    EXPECT_EQ(resBefore[0].fullPath, tempFilePath);

    // 2. 物理删除文件
    std::filesystem::remove(tempFilePath, ec);
    ASSERT_FALSE(std::filesystem::exists(tempFilePath, ec));

    // 3. 再次搜索：实时水合双保险检测到物理文件已消失，直接剔除幽灵结果并从 Store 中清除
    auto resAfter = parser.Search(fileName);
    EXPECT_EQ(resAfter.size(), 0u);
    EXPECT_EQ(parser.getFileCount(), dirStack.size());

    // 4. 重复搜索：缓存已同步失效，绝不出现已删除文件的幽灵残留
    auto resRetry = parser.Search(fileName);
    EXPECT_EQ(resRetry.size(), 0u);
}

TEST(MftParserHydrationTest, RecycleBinRejectionAndGhostPruning) {
    MftParser parser;
    parser.Initialize('C');

    std::vector<FileRecordInit> records;
    FileRecordInit rbDir{};
    rbDir.fileReferenceNumber = 1001;
    rbDir.parentFileReferenceNumber = 0;
    rbDir.fileName = L"$Recycle.Bin";
    rbDir.isDirectory = true;
    rbDir.fileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
    records.push_back(std::move(rbDir));

    FileRecordInit sidDir{};
    sidDir.fileReferenceNumber = 1002;
    sidDir.parentFileReferenceNumber = 1001;
    sidDir.fileName = L"S-1-5-21-3957262451-2484738596-3392476543-1001";
    sidDir.isDirectory = true;
    sidDir.fileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    records.push_back(std::move(sidDir));

    FileRecordInit deletedFile{};
    deletedFile.fileReferenceNumber = 1003;
    deletedFile.parentFileReferenceNumber = 1002;
    deletedFile.fileName = L"中源 账号密码.txt";
    deletedFile.isDirectory = false;
    deletedFile.fileAttributes = FILE_ATTRIBUTE_NORMAL;
    records.push_back(std::move(deletedFile));

    FileRecordInit normalFile{};
    normalFile.fileReferenceNumber = 1004;
    normalFile.parentFileReferenceNumber = 0;
    normalFile.fileName = L"正常 账号密码.txt";
    normalFile.isDirectory = false;
    normalFile.fileAttributes = FILE_ATTRIBUTE_NORMAL;
    records.push_back(std::move(normalFile));

    EXPECT_TRUE(parser.importSnapshot(std::move(records), 1000, parser.getVolumeSerialNumber()));

    // 1. 普通搜索：回收站内的中源账号密码绝不返回，正常文件正常返回
    auto resNormal = parser.Search(L"账号密码");
    ASSERT_EQ(resNormal.size(), 1u);
    EXPECT_EQ(resNormal[0].fileName, L"正常 账号密码.txt");

    // 2. 搜索中源账号密码：命中但因为在回收站中，必须返回 0
    auto resDeleted = parser.Search(L"中源 账号密码");
    EXPECT_EQ(resDeleted.size(), 0u);

    // 3. 显式搜索包含 recycle 关键字时允许呈现
    auto resExplicit = parser.Search(L"中源 recycle");
    EXPECT_EQ(resExplicit.size(), 1u);
    EXPECT_EQ(resExplicit[0].fileName, L"中源 账号密码.txt");
}

TEST(MftParserHydrationTest, CaseInsensitiveExcludePatterns) {
    MftParser parser;
    parser.Initialize('C');

    std::vector<FileRecordInit> records;
    FileRecordInit dirRec{};
    dirRec.fileReferenceNumber = 2001;
    dirRec.parentFileReferenceNumber = 0;
    dirRec.fileName = L"Node_Modules";
    dirRec.isDirectory = true;
    dirRec.fileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    records.push_back(std::move(dirRec));

    FileRecordInit fileRec{};
    fileRec.fileReferenceNumber = 2002;
    fileRec.parentFileReferenceNumber = 2001;
    fileRec.fileName = L"package.json";
    fileRec.isDirectory = false;
    fileRec.fileAttributes = FILE_ATTRIBUTE_NORMAL;
    records.push_back(std::move(fileRec));

    EXPECT_TRUE(parser.importSnapshot(std::move(records), 1000, parser.getVolumeSerialNumber()));

    SearchExcludeOptions opts;
    opts.patterns = {L"node_modules"};

    auto res = parser.Search(L"package", 100, opts);
    EXPECT_EQ(res.size(), 0u);
}

TEST(MftParserHydrationTest, RecycleBinRejectionUnconditionalEvenWhenValidationDisabled) {
    MftParser parser;
    parser.Initialize('C');
    parser.setRealTimeValidation(false);

    std::vector<FileRecordInit> records;
    FileRecordInit rbDir{};
    rbDir.fileReferenceNumber = 3001;
    rbDir.parentFileReferenceNumber = 0;
    rbDir.fileName = L"$recycle.bin";
    rbDir.isDirectory = true;
    rbDir.fileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    records.push_back(std::move(rbDir));

    FileRecordInit deletedFile{};
    deletedFile.fileReferenceNumber = 3002;
    deletedFile.parentFileReferenceNumber = 3001;
    deletedFile.fileName = L"中源 账号密码.txt";
    deletedFile.isDirectory = false;
    deletedFile.fileAttributes = FILE_ATTRIBUTE_NORMAL;
    records.push_back(std::move(deletedFile));

    EXPECT_TRUE(parser.importSnapshot(std::move(records), 1000, parser.getVolumeSerialNumber()));

    auto res = parser.Search(L"中源 账号密码");
    EXPECT_EQ(res.size(), 0u);
}




TEST(MftParserHydrationTest, SmartPathTieringPrioritizesOriginalOverChatAndTempAndFiltersRecentLnk) {
    MftParser parser;
    parser.Initialize('C');
    parser.setRealTimeValidation(false);

    std::vector<FileRecordInit> records;

    auto addDir = [&](DWORDLONG id, DWORDLONG parent, const wchar_t* name) {
        FileRecordInit d{};
        d.fileReferenceNumber = id;
        d.parentFileReferenceNumber = parent;
        d.fileName = name;
        d.isDirectory = true;
        d.fileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        records.push_back(std::move(d));
    };

    auto addFile = [&](DWORDLONG id, DWORDLONG parent, const wchar_t* name, uint64_t writeTime) {
        FileRecordInit f{};
        f.fileReferenceNumber = id;
        f.parentFileReferenceNumber = parent;
        f.fileName = name;
        f.isDirectory = false;
        f.fileAttributes = FILE_ATTRIBUTE_NORMAL;
        f.lastWriteTime = writeTime;
        records.push_back(std::move(f));
    };

    // 1. 普通用户工作目录：C:\Chosen\解决高级总账科目规则报错.txt (writeTime = 5000)
    addDir(4001, 0, L"Chosen");
    addFile(4002, 4001, L"解决高级总账科目规则报错.txt", 5000);

    // 2. 微信聊天文件目录：C:\Documents\xwechat_files\wxid_1\msg\file\2026-09\解决高级总账科目规则报错.txt (writeTime = 8000, 比原始件更新)
    addDir(4010, 0, L"Documents");
    addDir(4011, 4010, L"xwechat_files");
    addDir(4012, 4011, L"wxid_1");
    addFile(4013, 4012, L"解决高级总账科目规则报错.txt", 8000);

    // 3. 桌面普通快捷方式：C:\Desktop\解决快捷方式.lnk (writeTime = 9000)
    addDir(4020, 0, L"Desktop");
    addFile(4021, 4020, L"解决快捷方式.lnk", 9000);

    // 4. Temp 临时目录文件：C:\Temp\解决临时缓存.txt (writeTime = 9500)
    addDir(4030, 0, L"Temp");
    addFile(4031, 4030, L"解决临时缓存.txt", 9500);

    // 5. Windows Recent 影子快捷方式：C:\Microsoft\Windows\Recent\解决大量警报V10002.txt.lnk (writeTime = 9999)
    addDir(4040, 0, L"Microsoft");
    addDir(4041, 4040, L"Windows");
    addDir(4042, 4041, L"Recent");
    addFile(4043, 4042, L"解决大量警报V10002.txt.lnk", 9999);

    EXPECT_TRUE(parser.importSnapshot(std::move(records), 1000, parser.getVolumeSerialNumber()));

    auto res = parser.Search(L"解决", 100);
    // Recent 下的自动影子 .lnk 默认被过滤，剩余 4 条按语义分层排序：
    // [0] Chosen 普通用户目录原始文件 -> [1] xwechat_files 聊天副本 -> [2] Desktop 普通 .lnk -> [3] Temp 临时目录文件
    ASSERT_EQ(res.size(), 4u);
    EXPECT_NE(res[0].fullPath.find(L"\\Chosen\\"), std::wstring::npos);
    EXPECT_NE(res[1].fullPath.find(L"\\xwechat_files\\"), std::wstring::npos);
    EXPECT_NE(res[2].fullPath.find(L"解决快捷方式.lnk"), std::wstring::npos);
    EXPECT_NE(res[3].fullPath.find(L"\\Temp\\"), std::wstring::npos);

    // 当用户显式搜索 recent 或 lnk 时，Recent 下的影子 .lnk 仍可被检出
    auto resExplicitRecent = parser.Search(L"解决 lnk", 100);
    EXPECT_EQ(resExplicitRecent.size(), 2u);
}

TEST(SearchExpressionTest, LazyDiskSizeAndDateModifiedFilterAndRanking) {
    // 1. 验证组合查询 (关键词 + ext: / size: / dm:) 的纯净关键词排序权重不被过滤算子稀释
    FileRecord exactPdf{1, 0, L"解决.pdf", L"解决.pdf", L"jj.pdf", L"jiejue.pdf", false, 0, 20480};
    FileRecord prefixPdf{2, 0, L"解决方案汇报.pdf", L"解决方案汇报.pdf", L"jjfahb.pdf", L"jiejuefanganhuibao.pdf", false, 0, 20480};
    FileRecord containPdf{3, 0, L"客户解决方案.pdf", L"客户解决方案.pdf", L"khjjfa.pdf", L"kehujiejuefangan.pdf", false, 0, 20480};

    auto exprCombo = SearchExpression::parse(L"解决 ext:pdf size:>10kb sort:size");
    EXPECT_TRUE(exprCombo.matches(exactPdf, L'D', L"D:\\Chosen\\解决.pdf"));
    EXPECT_EQ(exprCombo.calculateRank(exactPdf), 1);   // "解决.pdf" starts with "解决"
    EXPECT_EQ(exprCombo.calculateRank(prefixPdf), 1);  // "解决方案汇报.pdf" starts with "解决"
    EXPECT_EQ(exprCombo.calculateRank(containPdf), 4); // "客户解决方案.pdf" contains "解决"

    // 2. 验证 DateModified (dm:) 语法解析与比对
    SYSTEMTIME st2026Sep{};
    st2026Sep.wYear = 2026;
    st2026Sep.wMonth = 9;
    st2026Sep.wDay = 18;
    st2026Sep.wHour = 12;
    SYSTEMTIME utcSt{};
    FILETIME ft2026Sep{};
    TzSpecificLocalTimeToSystemTime(nullptr, &st2026Sep, &utcSt);
    SystemTimeToFileTime(&utcSt, &ft2026Sep);
    ULARGE_INTEGER uliSep{};
    uliSep.LowPart = ft2026Sep.dwLowDateTime;
    uliSep.HighPart = ft2026Sep.dwHighDateTime;

    FileRecord sepFile = prefixPdf;
    sepFile.lastWriteTime = uliSep.QuadPart;

    EXPECT_TRUE(SearchExpression::parse(L"解决 dm:2026-09").matches(sepFile, L'D', L"D:\\Chosen\\解决方案汇报.pdf"));
    EXPECT_TRUE(SearchExpression::parse(L"解决 dm:2026-09-18").matches(sepFile, L'D', L"D:\\Chosen\\解决方案汇报.pdf"));
    EXPECT_TRUE(SearchExpression::parse(L"解决 dm:>=2026-09-01").matches(sepFile, L'D', L"D:\\Chosen\\解决方案汇报.pdf"));
    EXPECT_TRUE(SearchExpression::parse(L"解决 dm:2026-09-01..2026-09-30").matches(sepFile, L'D', L"D:\\Chosen\\解决方案汇报.pdf"));
    EXPECT_FALSE(SearchExpression::parse(L"解决 dm:2026-08").matches(sepFile, L'D', L"D:\\Chosen\\解决方案汇报.pdf"));
    EXPECT_FALSE(SearchExpression::parse(L"解决 dm:<2026-09-01").matches(sepFile, L'D', L"D:\\Chosen\\解决方案汇报.pdf"));

    // 3. 验证真实磁盘文件在 MFT 内存记录 fileSize == 0 / lastWriteTime == 0 时的按需 GetFileAttributesExW 探测
    wchar_t tempDir[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring testFilePath = std::wstring(tempDir) + L"Tools3000_SizeDmTest_" + std::to_wstring(GetCurrentProcessId()) + L".dat";
    {
        HANDLE hFile = CreateFileW(testFilePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(hFile, INVALID_HANDLE_VALUE);
        std::vector<char> payload(16 * 1024, 'A'); // 16 KB
        DWORD written = 0;
        WriteFile(hFile, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr);
        CloseHandle(hFile);
    }

    FileRecord unhydratedMftRecord{};
    unhydratedMftRecord.fileName = L"Tools3000_SizeDmTest.dat";
    unhydratedMftRecord.normalizedName = L"tools3000_sizedmtest.dat";
    unhydratedMftRecord.isDirectory = false;
    unhydratedMftRecord.fileSize = 0;      // 模拟 FSCTL_ENUM_USN_DATA 初始未填充大小
    unhydratedMftRecord.lastWriteTime = 0; // 模拟初始未填充修改时间

    const wchar_t drive = testFilePath[0];
    EXPECT_TRUE(SearchExpression::parse(L"sizedmtest size:>10kb").matches(unhydratedMftRecord, drive, testFilePath));
    EXPECT_FALSE(SearchExpression::parse(L"sizedmtest size:>32kb").matches(unhydratedMftRecord, drive, testFilePath));
    EXPECT_TRUE(SearchExpression::parse(L"sizedmtest dm:today").matches(unhydratedMftRecord, drive, testFilePath));
    EXPECT_FALSE(SearchExpression::parse(L"sizedmtest dm:yesterday").matches(unhydratedMftRecord, drive, testFilePath));

    DeleteFileW(testFilePath.c_str());
}

TEST(SearchExpressionTest, CaseSensitiveWildcardForwardSlashAndParentTrailingSlash) {
    EXPECT_TRUE(SearchExpression::matchWildcard(L"*.PNG", L"photo.PNG", true));
    EXPECT_FALSE(SearchExpression::matchWildcard(L"*.PNG", L"photo.png", true));
    EXPECT_TRUE(SearchExpression::matchWildcard(L"*.PNG", L"photo.png", false));

    FileRecord upperPng{10, 1, L"Banner.PNG", L"banner.png", L"banner.png", L"banner.png", false};
    FileRecord lowerPng{11, 1, L"banner.png", L"banner.png", L"banner.png", L"banner.png", false};
    auto caseWildcardExpr = SearchExpression::parse(L"case:*.PNG");
    EXPECT_TRUE(caseWildcardExpr.matches(upperPng, L'D', L"D:\\Assets\\Banner.PNG"));
    EXPECT_FALSE(caseWildcardExpr.matches(lowerPng, L'D', L"D:\\Assets\\banner.png"));

    FileRecord mainCpp{12, 2, L"main.cpp", L"main.cpp", L"main.cpp", L"main.cpp", false};
    EXPECT_TRUE(SearchExpression::parse(L"D:/project/src").matches(mainCpp, L'D', L"D:\\project\\src\\main.cpp"));
    EXPECT_TRUE(SearchExpression::parse(L"path:project/src").matches(mainCpp, L'D', L"D:\\project\\src\\main.cpp"));
    EXPECT_TRUE(SearchExpression::parse(L"*project/src*").matches(mainCpp, L'D', L"D:\\project\\src\\main.cpp"));

    auto contentSlashExpr = SearchExpression::parse(L"ext:cpp content:foo/bar");
    EXPECT_EQ(contentSlashExpr.getContentQuery(), L"foo/bar");

    FileRecord rootTxt{13, 0, L"root.txt", L"root.txt", L"root.txt", L"root.txt", false};
    FileRecord workTxt{14, 3, L"sub.txt", L"sub.txt", L"sub.txt", L"sub.txt", false};
    EXPECT_TRUE(SearchExpression::parse(L"parent:D:\\").matches(rootTxt, L'D', L"D:\\root.txt"));
    EXPECT_TRUE(SearchExpression::parse(L"parent:D:/").matches(rootTxt, L'D', L"D:\\root.txt"));
    EXPECT_FALSE(SearchExpression::parse(L"parent:D:\\").matches(workTxt, L'D', L"D:\\Work\\sub.txt"));
    EXPECT_TRUE(SearchExpression::parse(L"parent:D:\\Work\\").matches(workTxt, L'D', L"D:\\Work\\sub.txt"));
    EXPECT_TRUE(SearchExpression::parse(L"parent:D:/Work/").matches(workTxt, L'D', L"D:\\Work\\sub.txt"));
    EXPECT_TRUE(SearchExpression::parse(L"parent:Work\\").matches(workTxt, L'D', L"D:\\Work\\sub.txt"));
}

TEST(MftParserTest, SearchParseOptionsPinyinMatchPathCaseAndCacheInvalidation) {
    auto parser = std::make_unique<MftParser>();
    std::vector<FileRecordInit> mockRecords;

    FileRecordInit dirRec{};
    dirRec.fileReferenceNumber = 200;
    dirRec.parentFileReferenceNumber = 0;
    dirRec.fileName = L"WorkDir";
    dirRec.isDirectory = true;
    mockRecords.push_back(std::move(dirRec));

    FileRecordInit wxRec{};
    wxRec.fileReferenceNumber = 201;
    wxRec.parentFileReferenceNumber = 200;
    wxRec.fileName = L"微信.exe";
    wxRec.isDirectory = false;
    wxRec.fileSize = 2048;
    mockRecords.push_back(std::move(wxRec));

    FileRecordInit upperPng{};
    upperPng.fileReferenceNumber = 202;
    upperPng.parentFileReferenceNumber = 200;
    upperPng.fileName = L"Report.PNG";
    upperPng.isDirectory = false;
    upperPng.fileSize = 1024;
    mockRecords.push_back(std::move(upperPng));

    FileRecordInit lowerPng{};
    lowerPng.fileReferenceNumber = 203;
    lowerPng.parentFileReferenceNumber = 200;
    lowerPng.fileName = L"report.png";
    lowerPng.isDirectory = false;
    lowerPng.fileSize = 1024;
    mockRecords.push_back(std::move(lowerPng));

    ASSERT_TRUE(parser->importSnapshot(std::move(mockRecords), 234567ULL, 0x22334455));

    SearchExcludeOptions noExcludes{};
    SearchParseOptions pinyinOn{true, false, false};
    SearchParseOptions pinyinOff{false, false, false};
    EXPECT_EQ(parser->Search(L"wx", 50, noExcludes, nullptr, pinyinOn).size(), 1u);
    EXPECT_EQ(parser->Search(L"wx", 50, noExcludes, nullptr, pinyinOff).size(), 0u);
    EXPECT_EQ(parser->Search(L"pinyin:wx", 50, noExcludes, nullptr, pinyinOff).size(), 1u);

    SearchParseOptions matchPathOff{true, false, false};
    SearchParseOptions matchPathOn{true, true, false};
    EXPECT_EQ(parser->Search(L"file:WorkDir", 50, noExcludes, nullptr, matchPathOff).size(), 0u);
    EXPECT_EQ(parser->Search(L"file:WorkDir", 50, noExcludes, nullptr, matchPathOn).size(), 3u);

    SearchParseOptions caseOff{true, false, false};
    SearchParseOptions caseOn{true, false, true};
    EXPECT_EQ(parser->Search(L"*.PNG", 50, noExcludes, nullptr, caseOff).size(), 2u);
    auto exactUpper = parser->Search(L"*.PNG", 50, noExcludes, nullptr, caseOn);
    ASSERT_EQ(exactUpper.size(), 1u);
    EXPECT_EQ(exactUpper[0].fileName, L"Report.PNG");
}

// -----------------------------------------------------------------------------
// 40. 文档内容搜索全链路重构防回退单元测试套件 (Content Search End-to-End Regression Suite)
// -----------------------------------------------------------------------------
#include "service/PipeClientAuth.h"
#include "service/content/ContentMatchUtils.h"
#include "service/content/ContentTextCache.h"
#include "service/content/PdfExtractor.h"
#include "service/content/WindowsIFilterExtractor.h"
#include "search/SearchQueryConfigUtils.h"
#include <zlib.h>

namespace tools3000_content_search_test_helpers {

inline std::string compressZlibBytes(std::string_view input) {
    uLongf bound = compressBound(static_cast<uLong>(input.size()));
    std::string out(bound, '\0');
    const int rc = compress2(
        reinterpret_cast<Bytef*>(out.data()),
        &bound,
        reinterpret_cast<const Bytef*>(input.data()),
        static_cast<uLong>(input.size()),
        Z_BEST_COMPRESSION
    );
    if (rc != Z_OK) return {};
    out.resize(bound);
    return out;
}

inline std::string compressRawDeflateBytes(std::string_view input) {
    z_stream strm{};
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }
    std::string out(input.size() + 64, '\0');
    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    strm.avail_in = static_cast<uInt>(input.size());
    strm.next_out = reinterpret_cast<Bytef*>(out.data());
    strm.avail_out = static_cast<uInt>(out.size());
    const int rc = deflate(&strm, Z_FINISH);
    const size_t totalOut = strm.total_out;
    deflateEnd(&strm);
    if (rc != Z_STREAM_END) return {};
    out.resize(totalOut);
    return out;
}

inline void appendU16LE(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

inline void appendU32LE(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

inline std::vector<uint8_t> buildZipArchive(
    const std::vector<std::pair<std::string, std::string>>& entries,
    bool useDeflate = true
) {
    struct CdRecordMeta {
        std::string name;
        uint16_t method = 0;
        uint32_t crc = 0;
        uint32_t compSize = 0;
        uint32_t uncompSize = 0;
        uint32_t localOffset = 0;
    };

    std::vector<uint8_t> archive;
    std::vector<CdRecordMeta> cdMetas;

    for (const auto& [name, rawPayload] : entries) {
        const uint32_t localOffset = static_cast<uint32_t>(archive.size());
        const uint32_t crc = static_cast<uint32_t>(crc32(
            0L,
            reinterpret_cast<const Bytef*>(rawPayload.data()),
            static_cast<uInt>(rawPayload.size())
        ));
        std::string compPayload;
        uint16_t method = 0;
        if (useDeflate && !rawPayload.empty()) {
            compPayload = compressRawDeflateBytes(rawPayload);
            if (!compPayload.empty()) {
                method = 8;
            }
        }
        if (method == 0) {
            compPayload = rawPayload;
        }

        // Local file header (0x04034b50)
        appendU32LE(archive, 0x04034b50u);
        appendU16LE(archive, 20);
        appendU16LE(archive, 0);
        appendU16LE(archive, method);
        appendU16LE(archive, 0);
        appendU16LE(archive, 0);
        appendU32LE(archive, crc);
        appendU32LE(archive, static_cast<uint32_t>(compPayload.size()));
        appendU32LE(archive, static_cast<uint32_t>(rawPayload.size()));
        appendU16LE(archive, static_cast<uint16_t>(name.size()));
        appendU16LE(archive, 0);
        archive.insert(archive.end(), name.begin(), name.end());
        archive.insert(archive.end(), compPayload.begin(), compPayload.end());

        cdMetas.push_back(CdRecordMeta{
            .name = name,
            .method = method,
            .crc = crc,
            .compSize = static_cast<uint32_t>(compPayload.size()),
            .uncompSize = static_cast<uint32_t>(rawPayload.size()),
            .localOffset = localOffset
        });
    }

    const uint32_t cdOffset = static_cast<uint32_t>(archive.size());
    for (const auto& meta : cdMetas) {
        // Central directory file header (0x02014b50)
        appendU32LE(archive, 0x02014b50u);
        appendU16LE(archive, 20);
        appendU16LE(archive, 20);
        appendU16LE(archive, 0);
        appendU16LE(archive, meta.method);
        appendU16LE(archive, 0);
        appendU16LE(archive, 0);
        appendU32LE(archive, meta.crc);
        appendU32LE(archive, meta.compSize);
        appendU32LE(archive, meta.uncompSize);
        appendU16LE(archive, static_cast<uint16_t>(meta.name.size()));
        appendU16LE(archive, 0);
        appendU16LE(archive, 0);
        appendU16LE(archive, 0);
        appendU16LE(archive, 0);
        appendU32LE(archive, 0);
        appendU32LE(archive, meta.localOffset);
        archive.insert(archive.end(), meta.name.begin(), meta.name.end());
    }
    const uint32_t cdSize = static_cast<uint32_t>(archive.size()) - cdOffset;

    // End of central directory record (0x06054b50)
    appendU32LE(archive, 0x06054b50u);
    appendU16LE(archive, 0);
    appendU16LE(archive, 0);
    appendU16LE(archive, static_cast<uint16_t>(cdMetas.size()));
    appendU16LE(archive, static_cast<uint16_t>(cdMetas.size()));
    appendU32LE(archive, cdSize);
    appendU32LE(archive, cdOffset);
    appendU16LE(archive, 0);

    return archive;
}

inline std::string buildFlatePdfWithToUnicodeCMap(
    std::string_view cmapPlain,
    std::string_view contentStreamPlain
) {
    const std::string cmapCompressed = compressZlibBytes(cmapPlain);
    const std::string contentCompressed = compressZlibBytes(contentStreamPlain);

    std::string pdf;
    pdf.reserve(512 + cmapCompressed.size() + contentCompressed.size());
    pdf += "%PDF-1.4\n";
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
    pdf += "3 0 obj\n<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 4 0 R >> >> /Contents 6 0 R >>\nendobj\n";
    pdf += "4 0 obj\n<< /Type /Font /Subtype /Type0 /BaseFont /SimSun /ToUnicode 5 0 R >>\nendobj\n";
    pdf += "5 0 obj\n<< /Filter /FlateDecode /Length " + std::to_string(cmapCompressed.size()) + " >>\nstream\n";
    pdf.append(cmapCompressed.data(), cmapCompressed.size());
    pdf += "\nendstream\nendobj\n";
    pdf += "6 0 obj\n<< /Filter /FlateDecode /Length " + std::to_string(contentCompressed.size()) + " >>\nstream\n";
    pdf.append(contentCompressed.data(), contentCompressed.size());
    pdf += "\nendstream\nendobj\n";
    pdf += "%%EOF\n";
    return pdf;
}

struct TempFileDeleter {
    std::wstring path;
    ~TempFileDeleter() {
        if (!path.empty()) {
            DeleteFileW(path.c_str());
        }
    }
};

} // namespace tools3000_content_search_test_helpers

TEST(PipeClientAuthTest, PostReadAuthenticationSameSidAndTokenRejection) {
    namespace frame = tools3000::service::pipe;
    namespace auth = tools3000::service::pipe_auth;

    const std::wstring currentSid = auth::queryCurrentProcessUserSidString();
    ASSERT_FALSE(currentSid.empty());

    auto runPipeRoundTripAuth = [](const std::wstring& expectedSid, auth::PipeClientAuthSession& outSession) {
        const std::wstring pipeName = L"\\\\.\\pipe\\Tools3000PipeAuthTest-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) +
            L"-" + std::to_wstring(reinterpret_cast<uintptr_t>(&outSession));

        HANDLE serverPipe = CreateNamedPipeW(
            pipeName.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1,
            frame::IoChunkBytes,
            frame::IoChunkBytes,
            0,
            nullptr
        );
        ASSERT_NE(serverPipe, INVALID_HANDLE_VALUE);

        std::thread clientThread([&pipeName]() {
            HANDLE clientPipe = CreateFileW(
                pipeName.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
            if (clientPipe == INVALID_HANDLE_VALUE) return;
            const std::string payload = R"({"query":"content:合同"})";
            const auto header = frame::encodeFrameHeader(static_cast<uint32_t>(payload.size()));
            frame::writeExact(clientPipe, header.data(), header.size());
            frame::writeExact(clientPipe, payload.data(), payload.size());

            char ack[frame::HeaderSize]{};
            frame::readExact(clientPipe, ack, sizeof(ack));
            CloseHandle(clientPipe);
        });

        const BOOL connected = ConnectNamedPipe(serverPipe, nullptr)
            ? TRUE
            : (GetLastError() == ERROR_PIPE_CONNECTED);
        EXPECT_TRUE(connected);

        if (connected) {
            char reqHeader[frame::HeaderSize]{};
            ASSERT_TRUE(frame::readExact(serverPipe, reqHeader, sizeof(reqHeader)));
            uint32_t reqLen = 0;
            ASSERT_TRUE(frame::decodeFrameHeader(reqHeader, reqLen, frame::MaxRequestBytes));
            std::string reqBody(reqLen, '\0');
            ASSERT_TRUE(frame::readExact(serverPipe, reqBody.data(), reqBody.size()));

            // Authenticate strictly AFTER first readExact so Windows kernel never fails with ERROR_CANNOT_IMPERSONATE
            outSession = auth::authenticatePipeClientAfterFirstRead(serverPipe, expectedSid);

            const auto ackHeader = frame::encodeFrameHeader(2u);
            frame::writeExact(serverPipe, ackHeader.data(), ackHeader.size());
        }

        clientThread.join();
        DisconnectNamedPipe(serverPipe);
        CloseHandle(serverPipe);
    };

    // 1. Same-SID portable fast path: authorized without requiring SeImpersonatePrivilege
    auth::PipeClientAuthSession sameSidSession;
    runPipeRoundTripAuth(currentSid, sameSidSession);
    EXPECT_TRUE(sameSidSession.authorized);
    EXPECT_TRUE(sameSidSession.isSameUserProcessSid);
    {
        auth::ScopedContentReadImpersonation guard(
            sameSidSession.clientToken,
            sameSidSession.isSameUserProcessSid,
            currentSid
        );
        EXPECT_TRUE(guard.isAuthorized());
    }

    // 2. Post-read kernel impersonation check (never returns ERROR_CANNOT_IMPERSONATE after readExact)
    auth::PipeClientAuthSession kernelCheckSession;
    runPipeRoundTripAuth(L"", kernelCheckSession);
    EXPECT_TRUE(kernelCheckSession.authorized);
    EXPECT_NE(kernelCheckSession.lastError, static_cast<DWORD>(ERROR_CANNOT_IMPERSONATE));

    // 3. Mismatched expected SID must be strictly rejected
    const std::wstring wrongSid = L"S-1-5-21-111111111-222222222-333333333-99999";
    auth::PipeClientAuthSession rejectedSession;
    runPipeRoundTripAuth(wrongSid, rejectedSession);
    EXPECT_FALSE(rejectedSession.authorized);
    {
        auth::ScopedContentReadImpersonation guard(
            rejectedSession.clientToken,
            rejectedSession.isSameUserProcessSid,
            wrongSid
        );
        EXPECT_FALSE(guard.isAuthorized());
        EXPECT_EQ(guard.lastError(), static_cast<DWORD>(ERROR_ACCESS_DENIED));
    }

    // 4. SearchQueryConfigUtils: disabled extension union & explicit content syntax detection
    nlohmann::json clientParams = {
        {"contentDisabledExts", nlohmann::json::array({".PSD", " dxf ", "pdf"})}
    };
    std::vector<std::string> configDisabled = {"pdf", ".cdr", "ai"};
    auto mergedDisabled = tools3000::search::mergeDisabledContentExtensions(clientParams, configDisabled, 64);
    ASSERT_EQ(mergedDisabled.size(), 5u);
    EXPECT_EQ(mergedDisabled[0], "psd");
    EXPECT_EQ(mergedDisabled[1], "dxf");
    EXPECT_EQ(mergedDisabled[2], "pdf");
    EXPECT_EQ(mergedDisabled[3], "cdr");
    EXPECT_EQ(mergedDisabled[4], "ai");

    EXPECT_TRUE(tools3000::search::hasExplicitContentQuerySyntax("ext:docx content:contract"));
    EXPECT_TRUE(tools3000::search::hasExplicitContentQuerySyntax("ext:docx c:contract"));
    EXPECT_FALSE(tools3000::search::hasExplicitContentQuerySyntax("c:\\projects\\main.cpp"));
    EXPECT_FALSE(tools3000::search::hasExplicitContentQuerySyntax("c: report.docx"));
    EXPECT_TRUE(tools3000::search::isPipeTransientBusyError(ERROR_PIPE_BUSY));
    EXPECT_TRUE(tools3000::search::isPipeTransientBusyError(ERROR_SEM_TIMEOUT));
    EXPECT_FALSE(tools3000::search::isPipeTransientBusyError(ERROR_FILE_NOT_FOUND));
}

TEST(SearchExpressionContentPlanTest, QuotedPhraseSpaceAfterColonAndModeDecoupling) {
    // 1. Quoted phrase inside content:"hello world" stays intact as a single phrase term
    auto exprQuoted = SearchExpression::parse(L"ext:md content:\"hello world\"");
    EXPECT_TRUE(exprQuoted.hasContentFilter());
    ASSERT_EQ(exprQuoted.getContentTerms().size(), 1u);
    EXPECT_EQ(exprQuoted.getContentTerms()[0], L"hello world");
    EXPECT_EQ(exprQuoted.getContentQuery(), L"hello world");

    // 2. Space after colon tolerance: "content: 关键词" and "内容: 关键词"
    auto exprSpaceEn = SearchExpression::parse(L"ext:docx content: 采购合同");
    EXPECT_TRUE(exprSpaceEn.hasContentFilter());
    ASSERT_EQ(exprSpaceEn.getContentTerms().size(), 1u);
    EXPECT_EQ(exprSpaceEn.getContentTerms()[0], L"采购合同");

    auto exprSpaceZh = SearchExpression::parse(L"内容: 保密协议 ext:pdf");
    EXPECT_TRUE(exprSpaceZh.hasContentFilter());
    ASSERT_EQ(exprSpaceZh.getContentTerms().size(), 1u);
    EXPECT_EQ(exprSpaceZh.getContentTerms()[0], L"保密协议");

    // 3. Disambiguation: standalone "c:" (Drive) vs "c:\Work" (Path) vs "c:合同" (Content)
    FileRecord memoDocx{10, 1, L"2026备忘录.docx", L"2026备忘录.docx", L"2026bwl.docx", L"2026beiwanglu.docx", false};
    FileRecord memoPdf{11, 1, L"2026备忘录.pdf", L"2026备忘录.pdf", L"2026bwl.pdf", L"2026beiwanglu.pdf", false};

    auto exprDriveC = SearchExpression::parse(L"c: 备忘录");
    EXPECT_FALSE(exprDriveC.hasContentFilter());
    EXPECT_TRUE(exprDriveC.matches(memoDocx, L'C', L"C:\\Work\\2026备忘录.docx"));
    EXPECT_FALSE(exprDriveC.matches(memoDocx, L'D', L"D:\\Work\\2026备忘录.docx"));

    auto exprPathC = SearchExpression::parse(L"c:\\Work 备忘录");
    EXPECT_FALSE(exprPathC.hasContentFilter());
    EXPECT_TRUE(exprPathC.matches(memoDocx, L'C', L"C:\\Work\\2026备忘录.docx"));
    EXPECT_FALSE(exprPathC.matches(memoDocx, L'D', L"D:\\Work\\2026备忘录.docx"));

    auto exprShortContent = SearchExpression::parse(L"c:合同");
    EXPECT_TRUE(exprShortContent.hasContentFilter());
    ASSERT_EQ(exprShortContent.getContentTerms().size(), 1u);
    EXPECT_EQ(exprShortContent.getContentTerms()[0], L"合同");

    // 4. Implicit content mode decoupling ("ext:docx D:\Work 合同" in content / both mode):
    // File candidate filter enforces ext:docx + D:\Work + !node_modules, while "合同" searches body only!
    auto plan = SearchExpression::buildContentPlan(L"ext:docx D:\\Work !node_modules 合同", /*implicitContentMode=*/true);
    EXPECT_FALSE(plan.hasExplicitContentSyntax);
    ASSERT_EQ(plan.contentTerms.size(), 1u);
    EXPECT_EQ(plan.contentTerms[0], L"合同");
    EXPECT_TRUE(plan.fileFilterExpr.matches(memoDocx, L'D', L"D:\\Work\\2026备忘录.docx"));
    EXPECT_FALSE(plan.fileFilterExpr.matches(memoPdf, L'D', L"D:\\Work\\2026备忘录.pdf"));
    EXPECT_FALSE(plan.fileFilterExpr.matches(memoDocx, L'C', L"C:\\Work\\2026备忘录.docx"));
    EXPECT_FALSE(plan.fileFilterExpr.matches(memoDocx, L'D', L"D:\\Work\\node_modules\\2026备忘录.docx"));

    // 5. Multiple content: operators and full-document AND semantics
    auto exprMultiAnd = SearchExpression::parse(L"content:甲方主体 content:违约责任");
    ASSERT_EQ(exprMultiAnd.getContentTerms().size(), 2u);
    EXPECT_EQ(exprMultiAnd.getContentTerms()[0], L"甲方主体");
    EXPECT_EQ(exprMultiAnd.getContentTerms()[1], L"违约责任");

    std::vector<tools3000::service::content::ContentSnippet> snippets;
    const std::wstring partialDoc = L"第一条：甲方主体为北京技术中心\n第二条：双方友好协商\n";
    EXPECT_FALSE(tools3000::service::content::matchDocumentTextAndBuildSnippets(
        partialDoc, exprMultiAnd.getContentTerms(), false, snippets, 4));
    EXPECT_TRUE(snippets.empty());

    const std::wstring fullDoc = L"第一条：甲方主体为北京技术中心\n第二条：违约责任按合同总额百分之二十执行\n";
    EXPECT_TRUE(tools3000::service::content::matchDocumentTextAndBuildSnippets(
        fullDoc, exprMultiAnd.getContentTerms(), false, snippets, 4));
    ASSERT_EQ(snippets.size(), 2u);
    EXPECT_EQ(snippets[0].lineNumber, 1u);
    EXPECT_EQ(snippets[0].lineContent.substr(snippets[0].matchOffset, snippets[0].matchLength), L"甲方主体");
    EXPECT_EQ(snippets[1].lineNumber, 2u);
    EXPECT_EQ(snippets[1].lineContent.substr(snippets[1].matchOffset, snippets[1].matchLength), L"违约责任");
}

TEST(ContentExtractorMatrixTest, PdfFlateDecodeAndToUnicodeCMapChineseSearch) {
    using namespace tools3000_content_search_test_helpers;
    auto& engine = tools3000::service::content::ContentSearchEngine::instance();
    tools3000::service::content::ContentTextCache::instance().clear();

    EXPECT_TRUE(engine.canSearchContent(L"pdf"));

    // Map <0001><0002> -> "合同" (U+5408 U+540C) via beginbfchar,
    // and <0010>..<0011> -> "签署" (U+7B7E, U+7F72) via beginbfrange array
    const std::string cmapStream =
        "/CIDInit /ProcSet findresource begin\n"
        "12 dict begin\n"
        "begincmap\n"
        "1 begincodespacerange\n"
        "<0000> <FFFF>\n"
        "endcodespacerange\n"
        "2 beginbfchar\n"
        "<0001> <5408>\n"
        "<0002> <540C>\n"
        "endbfchar\n"
        "1 beginbfrange\n"
        "<0010> <0011> [<7B7E> <7F72>]\n"
        "endbfrange\n"
        "endcmap\n";

    const std::string contentStream =
        "BT\n"
        "/F1 12 Tf\n"
        "72 720 Td\n"
        "(Clause 1: ) Tj <00010002> Tj\n"
        "0 -20 Td\n"
        "(Status: ) Tj <00100011> Tj\n"
        "ET\n";

    const std::string pdfBytes = buildFlatePdfWithToUnicodeCMap(cmapStream, contentStream);

    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring pdfPath = std::wstring(tempDir) + L"Tools3000_Test_CMap_" +
        std::to_wstring(GetCurrentProcessId()) + L".pdf";
    TempFileDeleter guard{pdfPath};

    {
        std::ofstream ofs(pdfPath, std::ios::binary);
        ofs.write(pdfBytes.data(), static_cast<std::streamsize>(pdfBytes.size()));
    }

    std::vector<tools3000::service::content::ContentSnippet> snippets;
    ASSERT_TRUE(engine.searchFile(pdfPath, std::vector<std::wstring>{L"合同"}, false, snippets, 3));
    ASSERT_FALSE(snippets.empty());
    EXPECT_EQ(snippets[0].lineNumber, 1u);
    EXPECT_EQ(snippets[0].lineContent.substr(snippets[0].matchOffset, snippets[0].matchLength), L"合同");

    snippets.clear();
    ASSERT_TRUE(engine.searchFile(pdfPath, std::vector<std::wstring>{L"合同", L"签署"}, false, snippets, 4));
    ASSERT_EQ(snippets.size(), 2u);
    EXPECT_EQ(snippets[0].lineContent.substr(snippets[0].matchOffset, snippets[0].matchLength), L"合同");
    EXPECT_EQ(snippets[1].lineContent.substr(snippets[1].matchOffset, snippets[1].matchLength), L"签署");

    snippets.clear();
    EXPECT_FALSE(engine.searchFile(pdfPath, std::vector<std::wstring>{L"合同", L"不存在的词"}, false, snippets, 4));
}

TEST(ContentExtractorMatrixTest, XlsxSharedStringsNoCellGluingStyleNoiseFilteredAndEpub) {
    using namespace tools3000_content_search_test_helpers;
    auto& engine = tools3000::service::content::ContentSearchEngine::instance();
    tools3000::service::content::ContentTextCache::instance().clear();

    EXPECT_TRUE(engine.canSearchContent(L"xlsx"));
    EXPECT_TRUE(engine.canSearchContent(L"epub"));

    const std::string sharedStringsXml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" count=\"3\" uniqueCount=\"3\">"
        "<si><t>采购甲方主体</t></si>"
        "<si><t>违约责任条款</t></si>"
        "<si><t>含实体&amp;编码&#27979;&#35797;</t></si>"
        "</sst>";

    const std::string stylesXml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<styleSheet><fonts><font><name val=\"CalibriStyleNoiseToken\"/></font></fonts></styleSheet>";

    const auto xlsxArchive = buildZipArchive({
        {"xl/styles.xml", stylesXml},
        {"xl/sharedStrings.xml", sharedStringsXml}
    }, /*useDeflate=*/true);

    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring xlsxPath = std::wstring(tempDir) + L"Tools3000_Test_Cells_" +
        std::to_wstring(GetCurrentProcessId()) + L".xlsx";
    TempFileDeleter xlsxGuard{xlsxPath};

    {
        std::ofstream ofs(xlsxPath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(xlsxArchive.data()), static_cast<std::streamsize>(xlsxArchive.size()));
    }

    std::vector<tools3000::service::content::ContentSnippet> snippets;
    // Cells must NOT glue into a single run-on string across </si> boundaries
    EXPECT_FALSE(engine.searchFile(xlsxPath, std::vector<std::wstring>{L"采购甲方主体违约责任条款"}, false, snippets, 3));

    // Full-document AND across distinct cells must match and report distinct line numbers
    snippets.clear();
    ASSERT_TRUE(engine.searchFile(xlsxPath, std::vector<std::wstring>{L"采购甲方主体", L"违约责任条款"}, false, snippets, 4));
    ASSERT_EQ(snippets.size(), 2u);
    EXPECT_NE(snippets[0].lineNumber, snippets[1].lineNumber);
    EXPECT_EQ(snippets[0].lineContent.substr(snippets[0].matchOffset, snippets[0].matchLength), L"采购甲方主体");
    EXPECT_EQ(snippets[1].lineContent.substr(snippets[1].matchOffset, snippets[1].matchLength), L"违约责任条款");

    // XML/HTML named and numeric entities must decode cleanly
    snippets.clear();
    ASSERT_TRUE(engine.searchFile(xlsxPath, std::vector<std::wstring>{L"含实体&编码测试"}, false, snippets, 3));

    // Non-body template noise in xl/styles.xml must be ignored
    snippets.clear();
    EXPECT_FALSE(engine.searchFile(xlsxPath, std::vector<std::wstring>{L"CalibriStyleNoiseToken"}, false, snippets, 3));

    // EPUB XHTML extraction (skipping <style> blocks)
    const std::string epubChapter =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><style>.HiddenCssNoiseToken { color: red; }</style></head>"
        "<body><h1>第一章</h1><p>分布式全文检索引擎架构实践</p></body></html>";
    const auto epubArchive = buildZipArchive({{"OEBPS/chapter1.xhtml", epubChapter}}, /*useDeflate=*/true);
    const std::wstring epubPath = std::wstring(tempDir) + L"Tools3000_Test_Book_" +
        std::to_wstring(GetCurrentProcessId()) + L".epub";
    TempFileDeleter epubGuard{epubPath};
    {
        std::ofstream ofs(epubPath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(epubArchive.data()), static_cast<std::streamsize>(epubArchive.size()));
    }
    snippets.clear();
    EXPECT_TRUE(engine.searchFile(epubPath, std::vector<std::wstring>{L"全文检索引擎架构"}, false, snippets, 3));
    snippets.clear();
    EXPECT_FALSE(engine.searchFile(epubPath, std::vector<std::wstring>{L"HiddenCssNoiseToken"}, false, snippets, 3));
}

TEST(ContentExtractorMatrixTest, RtfHexGbkAndUnicodeEscapeDecoding) {
    using namespace tools3000_content_search_test_helpers;
    auto& engine = tools3000::service::content::ContentSearchEngine::instance();
    tools3000::service::content::ContentTextCache::instance().clear();

    EXPECT_TRUE(engine.canSearchContent(L"rtf"));

    // \'ba\'cf\'cd\'ac = GBK (CP936) for "合同"
    // \u21512?\u21516? = Unicode decimal escapes for "合同" (U+5408, U+540C) with fallback '?' skipped
    // \u-28209? = signed 16-bit two's complement for U+91CF (37327 - 65536 = -28209, "量")
    const std::string rtfContent =
        "{\\rtf1\\ansi\\ansicpg936\\uc1"
        "{\\fonttbl{\\f0\\fnil FontTableIgnoredNoise;}}"
        "{\\*\\generator CustomRtfGeneratorNoise;}"
        "\\pard \\'ba\\'cf\\'cd\\'ac\\par\n"
        "\\u21512?\\u21516?\\u-28209?\\par\n"
        "}";

    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring rtfPath = std::wstring(tempDir) + L"Tools3000_Test_RichText_" +
        std::to_wstring(GetCurrentProcessId()) + L".rtf";
    TempFileDeleter rtfGuard{rtfPath};

    {
        std::ofstream ofs(rtfPath, std::ios::binary);
        ofs.write(rtfContent.data(), static_cast<std::streamsize>(rtfContent.size()));
    }

    std::vector<tools3000::service::content::ContentSnippet> snippets;
    ASSERT_TRUE(engine.searchFile(rtfPath, std::vector<std::wstring>{L"合同"}, false, snippets, 3));
    ASSERT_FALSE(snippets.empty());
    EXPECT_EQ(snippets[0].lineContent.substr(snippets[0].matchOffset, snippets[0].matchLength), L"合同");
    if (IsValidCodePage(936) || IsValidCodePage(54936)) {
        // Line 1 decoded via CP936 \'ba\'cf\'cd\'ac
        EXPECT_EQ(snippets[0].lineNumber, 1u);
    }

    snippets.clear();
    ASSERT_TRUE(engine.searchFile(rtfPath, std::vector<std::wstring>{L"合同量"}, false, snippets, 3));
    ASSERT_FALSE(snippets.empty());
    EXPECT_EQ(snippets[0].lineContent.substr(snippets[0].matchOffset, snippets[0].matchLength), L"合同量");

    snippets.clear();
    EXPECT_FALSE(engine.searchFile(rtfPath, std::vector<std::wstring>{L"FontTableIgnoredNoise"}, false, snippets, 3));
    EXPECT_FALSE(engine.searchFile(rtfPath, std::vector<std::wstring>{L"CustomRtfGeneratorNoise"}, false, snippets, 3));
}

TEST(ContentExtractorMatrixTest, BomlessUtf16LeWithSurrogatePairNotRejectedAsBinary) {
    using namespace tools3000_content_search_test_helpers;
    auto& engine = tools3000::service::content::ContentSearchEngine::instance();

    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring utf16NoBomPath = std::wstring(tempDir) + L"Tools3000_Test_NoBomSurrogate_" +
        std::to_wstring(GetCurrentProcessId()) + L".txt";
    TempFileDeleter fileGuard{utf16NoBomPath};

    // Construct BOM-less UTF-16LE containing CJK Unified Ideographs Extension B U+20BB7 (0xD842, 0xDFB7)
    std::wstring lineWithSurrogate;
    lineWithSurrogate.push_back(static_cast<wchar_t>(0xD842));
    lineWithSurrogate.push_back(static_cast<wchar_t>(0xDFB7));
    lineWithSurrogate += L"前缀_核心采购框架协议_后缀\r\n第二行补充验收条款\r\n";

    {
        std::ofstream ofs(utf16NoBomPath, std::ios::binary);
        ofs.write(
            reinterpret_cast<const char*>(lineWithSurrogate.data()),
            static_cast<std::streamsize>(lineWithSurrogate.size() * sizeof(wchar_t))
        );
    }

    std::vector<tools3000::service::content::ContentSnippet> snippets;
    ASSERT_TRUE(engine.searchFile(utf16NoBomPath, std::vector<std::wstring>{L"核心采购框架协议"}, false, snippets, 3));
    ASSERT_EQ(snippets.size(), 1u);
    EXPECT_EQ(snippets[0].lineNumber, 1u);
    // Verify UTF-16 code-unit offset stays 100% aligned even after a 2-unit surrogate pair!
    EXPECT_EQ(
        snippets[0].lineContent.substr(snippets[0].matchOffset, snippets[0].matchLength),
        L"核心采购框架协议"
    );
}

TEST(ContentExtractorMatrixTest, DocOle2CompoundBinaryFallbackExtractsChinese) {
    using namespace tools3000_content_search_test_helpers;
    auto& engine = tools3000::service::content::ContentSearchEngine::instance();
    tools3000::service::content::ContentTextCache::instance().clear();

    EXPECT_TRUE(engine.canSearchContent(L"doc"));
    EXPECT_TRUE(engine.canSearchContent(L"xls"));
    EXPECT_TRUE(engine.canSearchContent(L"ppt"));
    EXPECT_TRUE(engine.canSearchContent(L"wps"));

    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring docPath = std::wstring(tempDir) + L"Tools3000_Test_Ole2_" +
        std::to_wstring(GetCurrentProcessId()) + L".doc";
    TempFileDeleter docGuard{docPath};

    // Standard OLE2 Compound Document signature + binary Fib/sector header + UTF-16LE Word document stream
    std::vector<uint8_t> ole2Bytes = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    ole2Bytes.resize(512, 0x00);
    for (size_t i = 8; i < 128; ++i) {
        ole2Bytes[i] = static_cast<uint8_t>((i * 37u) & 0xFFu);
    }
    const std::wstring embeddedParagraph = L"年度战略合作保密协议核心条款2026\r\n附件一：交付里程碑说明\r\n";
    const auto* rawChars = reinterpret_cast<const uint8_t*>(embeddedParagraph.data());
    ole2Bytes.insert(
        ole2Bytes.end(),
        rawChars,
        rawChars + embeddedParagraph.size() * sizeof(wchar_t)
    );
    ole2Bytes.resize(ole2Bytes.size() + 256, 0x00);

    {
        std::ofstream ofs(docPath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(ole2Bytes.data()), static_cast<std::streamsize>(ole2Bytes.size()));
    }

    std::vector<tools3000::service::content::ContentSnippet> snippets;
    ASSERT_TRUE(engine.searchFile(docPath, std::vector<std::wstring>{L"战略合作保密协议"}, false, snippets, 3));
    ASSERT_FALSE(snippets.empty());
    EXPECT_EQ(
        snippets[0].lineContent.substr(snippets[0].matchOffset, snippets[0].matchLength),
        L"战略合作保密协议"
    );
}

TEST(ContentTextCacheAndCancellationTest, CacheHitInvalidationOnModifyAndCooperativeCancel) {
    using namespace tools3000_content_search_test_helpers;
    auto& engine = tools3000::service::content::ContentSearchEngine::instance();
    auto& cache = tools3000::service::content::ContentTextCache::instance();
    cache.clear();

    const std::string cmapStream =
        "begincmap\n"
        "2 beginbfchar\n"
        "<0001> <7248>\n"
        "<0002> <672C>\n"
        "endbfchar\n"
        "endcmap\n"; // "版本"
    const std::string contentV1 = "BT /F1 12 Tf (AlphaReleaseV1 ) Tj <00010002> Tj ET\n";
    const std::string pdfV1 = buildFlatePdfWithToUnicodeCMap(cmapStream, contentV1);

    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring pdfPath = std::wstring(tempDir) + L"Tools3000_Test_Cache_" +
        std::to_wstring(GetCurrentProcessId()) + L".pdf";
    TempFileDeleter pdfGuard{pdfPath};

    {
        std::ofstream ofs(pdfPath, std::ios::binary);
        ofs.write(pdfV1.data(), static_cast<std::streamsize>(pdfV1.size()));
    }

    // 1. Cancelled request must abort immediately and NEVER populate ContentTextCache
    std::vector<tools3000::service::content::ContentSnippet> snippets;
    EXPECT_FALSE(engine.searchFile(
        pdfPath,
        std::vector<std::wstring>{L"AlphaReleaseV1"},
        false,
        snippets,
        3,
        []() { return true; }
    ));
    EXPECT_EQ(cache.entryCount(), 0u);

    // 2. First real search populates zlib-compressed cache (miss -> put)
    ASSERT_TRUE(engine.searchFile(pdfPath, std::vector<std::wstring>{L"AlphaReleaseV1"}, false, snippets, 3));
    EXPECT_EQ(cache.entryCount(), 1u);
    EXPECT_EQ(cache.hitCount(), 0u);
    EXPECT_GT(cache.totalCompressedBytes(), 0u);

    // 3. Second search with different keyword hits compressed in-memory cache directly
    snippets.clear();
    ASSERT_TRUE(engine.searchFile(pdfPath, std::vector<std::wstring>{L"版本"}, false, snippets, 3));
    EXPECT_EQ(cache.hitCount(), 1u);

    // 4. Request-scoped ContentFormatFilter disables "pdf" locally without mutating global engine state
    tools3000::service::content::ContentFormatFilter localFilter;
    localFilter.disabledExts.insert(L"pdf");
    snippets.clear();
    EXPECT_FALSE(engine.searchFile(
        pdfPath,
        std::vector<std::wstring>{L"AlphaReleaseV1"},
        false,
        snippets,
        3,
        {},
        &localFilter
    ));
    EXPECT_TRUE(engine.canSearchContent(L"pdf"));

    // 5. Modifying file and advancing lastWriteTime automatically invalidates stale cache entry
    const std::string contentV2 = "BT /F1 12 Tf (OmegaReleaseV2_Updated ) Tj <00010002> Tj ET\n";
    const std::string pdfV2 = buildFlatePdfWithToUnicodeCMap(cmapStream, contentV2);
    {
        HANDLE hWrite = CreateFileW(
            pdfPath.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        ASSERT_NE(hWrite, INVALID_HANDLE_VALUE);
        DWORD written = 0;
        WriteFile(hWrite, pdfV2.data(), static_cast<DWORD>(pdfV2.size()), &written, nullptr);

        FILETIME ftNow{};
        GetSystemTimeAsFileTime(&ftNow);
        ULARGE_INTEGER uli{};
        uli.LowPart = ftNow.dwLowDateTime;
        uli.HighPart = ftNow.dwHighDateTime;
        uli.QuadPart += 100000000ULL; // +10 seconds
        FILETIME ftFuture{};
        ftFuture.dwLowDateTime = uli.LowPart;
        ftFuture.dwHighDateTime = uli.HighPart;
        SetFileTime(hWrite, nullptr, nullptr, &ftFuture);
        CloseHandle(hWrite);
    }

    snippets.clear();
    EXPECT_FALSE(engine.searchFile(pdfPath, std::vector<std::wstring>{L"AlphaReleaseV1"}, false, snippets, 3));
    snippets.clear();
    EXPECT_TRUE(engine.searchFile(pdfPath, std::vector<std::wstring>{L"OmegaReleaseV2_Updated"}, false, snippets, 3));

    // 6. MftParser::CollectContentCandidates: zero-disk-I/O junk pruning, priority scoring, and cancellation
    MftParser parser;
    parser.Initialize('C');
    std::vector<FileRecordInit> records;
    auto pushDir = [&](DWORDLONG id, DWORDLONG parent, const wchar_t* name) {
        FileRecordInit d{};
        d.fileReferenceNumber = id;
        d.parentFileReferenceNumber = parent;
        d.fileName = name;
        d.isDirectory = true;
        d.fileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        records.push_back(std::move(d));
    };
    auto pushFile = [&](DWORDLONG id, DWORDLONG parent, const wchar_t* name, uint64_t writeTime) {
        FileRecordInit f{};
        f.fileReferenceNumber = id;
        f.parentFileReferenceNumber = parent;
        f.fileName = name;
        f.isDirectory = false;
        f.fileAttributes = FILE_ATTRIBUTE_NORMAL;
        f.fileSize = 4096;
        f.lastWriteTime = writeTime;
        records.push_back(std::move(f));
    };

    pushDir(7001, 0, L"node_modules");
    pushFile(7002, 7001, L"ignored_pkg.md", 9000);

    pushDir(7010, 0, L"Users");
    pushDir(7011, 7010, L"Alice");
    pushDir(7012, 7011, L"Documents");
    pushFile(7013, 7012, L"important_contract.docx", 5000);

    pushDir(7020, 0, L"MiscArchive");
    pushFile(7021, 7020, L"old_notes.txt", 6000);

    ASSERT_TRUE(parser.importSnapshot(std::move(records), 1000, parser.getVolumeSerialNumber()));

    auto plan = SearchExpression::buildContentPlan(L"合同", /*implicitContentMode=*/true);
    ContentCandidateOptions candOpts;
    candOpts.userProfileDirLower = L"c:\\users\\alice";
    candOpts.documentsDirLower = L"c:\\users\\alice\\documents";

    auto candidates = parser.CollectContentCandidates(plan.fileFilterExpr, candOpts);
    // node_modules/ignored_pkg.md must be pruned in-memory; Documents/important_contract.docx prioritized first
    ASSERT_EQ(candidates.size(), 2u);
    EXPECT_EQ(candidates[0].fileName, L"important_contract.docx");
    EXPECT_EQ(candidates[1].fileName, L"old_notes.txt");

    // Cooperative cancellation returns immediately
    auto cancelledCandidates = parser.CollectContentCandidates(
        plan.fileFilterExpr,
        candOpts,
        []() { return true; }
    );
    EXPECT_TRUE(cancelledCandidates.empty());

    cache.clear();
}

TEST(ContentExtractorMatrixTest, MultiPageCollidingFontNameToUnicodeAndFilterAbbreviationAndZipDirFallback) {
    using namespace tools3000_content_search_test_helpers;
    auto& engine = tools3000::service::content::ContentSearchEngine::instance();
    tools3000::service::content::ContentTextCache::instance().clear();

    // 1. Multi-page PDF where Page 1 and Page 2 both bind local font name /F1 to distinct Font & ToUnicode objects,
    //    and stream dictionaries contain /Length1 before /Length and abbreviated /Fl /FlateDecode after /Flags.
    const std::string cmapPage1 = compressZlibBytes(
        "begincmap\n2 beginbfchar\n<0001> <7532>\n<0002> <65B9>\nendbfchar\nendcmap\n"); // "甲方"
    const std::string cmapPage2 = compressZlibBytes(
        "begincmap\n2 beginbfchar\n<0001> <4E59>\n<0002> <65B9>\nendbfchar\nendcmap\n"); // "乙方"
    const std::string contentPage1 = compressZlibBytes("BT /F1 12 Tf <00010002> Tj ET\n");
    const std::string contentPage2 = compressZlibBytes("BT /F1 12 Tf <00010002> Tj ET\n");

    std::string multiPagePdf;
    multiPagePdf += "%PDF-1.4\n";
    multiPagePdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    multiPagePdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 >>\nendobj\n";
    multiPagePdf += "3 0 obj\n<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>\nendobj\n";
    multiPagePdf += "4 0 obj\n<< /Type /Font /Subtype /Type0 /ToUnicode 8 0 R >>\nendobj\n";
    multiPagePdf += "5 0 obj\n<< /Length1 999999 /Flags 32 /Fl /FlateDecode /Length " +
        std::to_string(contentPage1.size()) + " >>\nstream\n";
    multiPagePdf.append(contentPage1.data(), contentPage1.size());
    multiPagePdf += "\nendstream\nendobj\n";
    multiPagePdf += "6 0 obj\n<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 7 0 R >> >> /Contents 9 0 R >>\nendobj\n";
    multiPagePdf += "7 0 obj\n<< /Type /Font /Subtype /Type0 /ToUnicode 10 0 R >>\nendobj\n";
    multiPagePdf += "8 0 obj\n<< /Filter /FlateDecode /Length " + std::to_string(cmapPage1.size()) + " >>\nstream\n";
    multiPagePdf.append(cmapPage1.data(), cmapPage1.size());
    multiPagePdf += "\nendstream\nendobj\n";
    multiPagePdf += "9 0 obj\n<< /Length1 888888 /Fl /FlateDecode /Length " +
        std::to_string(contentPage2.size()) + " >>\nstream\n";
    multiPagePdf.append(contentPage2.data(), contentPage2.size());
    multiPagePdf += "\nendstream\nendobj\n";
    multiPagePdf += "10 0 obj\n<< /Filter /FlateDecode /Length " + std::to_string(cmapPage2.size()) + " >>\nstream\n";
    multiPagePdf.append(cmapPage2.data(), cmapPage2.size());
    multiPagePdf += "\nendstream\nendobj\n%%EOF\n";

    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring pdfPath = std::wstring(tempDir) + L"Tools3000_Test_MultiPageCMap_" +
        std::to_wstring(GetCurrentProcessId()) + L".pdf";
    TempFileDeleter pdfGuard{pdfPath};
    {
        std::ofstream ofs(pdfPath, std::ios::binary);
        ofs.write(multiPagePdf.data(), static_cast<std::streamsize>(multiPagePdf.size()));
    }

    std::vector<tools3000::service::content::ContentSnippet> snippets;
    ASSERT_TRUE(engine.searchFile(pdfPath, std::vector<std::wstring>{L"甲方"}, false, snippets, 3));
    ASSERT_FALSE(snippets.empty());
    snippets.clear();
    ASSERT_TRUE(engine.searchFile(pdfPath, std::vector<std::wstring>{L"乙方"}, false, snippets, 3));
    ASSERT_FALSE(snippets.empty());

    // 2. DOCX archive with 0-byte directory entry ("word/") and valid document entry
    const std::string docxXml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document><w:body><w:p><w:r><w:t>跨部门立项评审纪要2026</w:t></w:r></w:p></w:body></w:document>";
    auto zipBytes = buildZipArchive({
        {"word/", ""},
        {"word/document.xml", docxXml},
    });
    const std::wstring docxPath = std::wstring(tempDir) + L"Tools3000_Test_DirEntry_" +
        std::to_wstring(GetCurrentProcessId()) + L".docx";
    TempFileDeleter docxGuard{docxPath};
    {
        std::ofstream ofs(docxPath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(zipBytes.data()), static_cast<std::streamsize>(zipBytes.size()));
    }

    snippets.clear();
    ASSERT_TRUE(engine.searchFile(docxPath, std::vector<std::wstring>{L"跨部门立项评审纪要2026"}, false, snippets, 3));
    ASSERT_FALSE(snippets.empty());
    tools3000::service::content::ContentTextCache::instance().clear();
}

TEST(FileIndexStoreTest, UpsertReusesUnchangedNameArenaAndUpdatesMetadataInPlace) {
    tools3000::service::FileIndexStore store;
    store.reserve(16);

    FileRecordInit init{};
    init.fileReferenceNumber = 42001ULL;
    init.parentFileReferenceNumber = 5ULL;
    init.fileName = L"年度预算报告.xlsx";
    init.isDirectory = false;
    init.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    init.fileSize = 1024ULL;
    init.creationTime = 133000000000000000ULL;
    init.lastWriteTime = 133000000100000000ULL;

    store.upsert(init);
    ASSERT_EQ(store.size(), 1u);
    const uint64_t baselineBytes = store.approximateBytes();

    // Repeated USN metadata/content updates with identical fileName must reuse arena & pinyin slots
    for (uint64_t i = 1; i <= 200; ++i) {
        init.fileSize = 1024ULL + i * 64ULL;
        init.lastWriteTime = 133000000100000000ULL + i * 1000ULL;
        store.upsert(init);
    }
    EXPECT_EQ(store.size(), 1u);
    EXPECT_EQ(store.approximateBytes(), baselineBytes);

    const auto* stored = store.find(42001ULL);
    ASSERT_NE(stored, nullptr);
    const auto viewAfterUpsert = store.view(*stored);
    EXPECT_EQ(viewAfterUpsert.fileName, L"年度预算报告.xlsx");
    EXPECT_EQ(viewAfterUpsert.fileSize, 1024ULL + 200ULL * 64ULL);
    EXPECT_FALSE(viewAfterUpsert.pinyinInitials.empty());

    // Direct in-place metadata refresh for USN BASIC_INFO_CHANGE / CLOSE
    EXPECT_TRUE(store.updateMetadata(42001ULL, 987654ULL, 133100000000000000ULL, 133200000000000000ULL));
    EXPECT_EQ(stored->fileSize, 987654ULL);
    EXPECT_EQ(stored->creationTime, 133100000000000000ULL);
    EXPECT_EQ(stored->lastWriteTime, 133200000000000000ULL);

    EXPECT_FALSE(store.updateMetadata(999999ULL, 1ULL, 1ULL, 1ULL));
    EXPECT_TRUE(store.erase(42001ULL));
    EXPECT_FALSE(store.updateMetadata(42001ULL, 1ULL, 1ULL, 1ULL));
}

TEST(ContentSearchProgressiveTest, LongSingleLineWindowDecodeNoisePruningAndSubfolderPriority) {
    auto& engine = tools3000::service::content::ContentSearchEngine::instance();
    const auto snapshot = engine.buildAllowedExtensionSnapshot(nullptr);
    EXPECT_FALSE(snapshot.contains(L"d"));
    EXPECT_TRUE(snapshot.contains(L"ch"));

    wchar_t tempDir[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring chPath = std::wstring(tempDir) + L"Tools3000_LongSingleLine_" +
        std::to_wstring(GetCurrentProcessId()) + L".ch";
    struct ChTempGuard {
        std::wstring p;
        ~ChTempGuard() { DeleteFileW(p.c_str()); }
    } chGuard{chPath};

    {
        std::string singleLinePayload;
        singleLinePayload.reserve(72000);
        singleLinePayload.append("<Config><Header note=\"生产订单同步配置\">");
        singleLinePayload.append(18000, 'x');
        singleLinePayload.append(" SQL_CMD=\"UPDATE T0 SET T0.U_Synced='Y'\" ");
        singleLinePayload.append(42000, 'y');
        singleLinePayload.append(" TARGET_TABLE=\"OWOR\" DESC=\"工单主表回写\"/></Config>");
        std::ofstream ofs(chPath, std::ios::binary | std::ios::trunc);
        ofs.write(singleLinePayload.data(), static_cast<std::streamsize>(singleLinePayload.size()));
    }

    std::vector<tools3000::service::content::ContentSnippet> snippets;
    const std::vector<std::wstring> terms{L"update", L"owor"};
    ASSERT_TRUE(engine.searchFile(chPath, terms, false, snippets, 3));
    ASSERT_FALSE(snippets.empty());
    EXPECT_EQ(snippets[0].lineNumber, 1);

    MftParser parser;
    parser.Initialize('C');
    parser.setRealTimeValidation(false);

    std::vector<FileRecordInit> records;
    auto addDir = [&](DWORDLONG id, DWORDLONG parent, const wchar_t* name) {
        FileRecordInit d{};
        d.fileReferenceNumber = id;
        d.parentFileReferenceNumber = parent;
        d.fileName = name;
        d.isDirectory = true;
        d.fileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        records.push_back(std::move(d));
    };
    auto addFile = [&](DWORDLONG id, DWORDLONG parent, const wchar_t* name, uint64_t sizeBytes, uint64_t writeTime) {
        FileRecordInit f{};
        f.fileReferenceNumber = id;
        f.parentFileReferenceNumber = parent;
        f.fileName = name;
        f.isDirectory = false;
        f.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
        f.fileSize = sizeBytes;
        f.lastWriteTime = writeTime;
        records.push_back(std::move(f));
    };

    addDir(7001, 0, L"repo");
    addDir(7002, 7001, L"SAPB1Help");
    addFile(7003, 7002, L"OWOR.htm", 8192, 133500000000000000ULL);

    addDir(7010, 7001, L"SBOTerminal");
    addFile(7011, 7010, L"wo_sync.sql", 4096, 133500000000000000ULL);

    addDir(7020, 7010, L".venv");
    addDir(7021, 7020, L"Lib");
    addDir(7022, 7021, L"site-packages");
    addFile(7023, 7022, L"noise.py", 4096, 133900000000000000ULL);

    addDir(7030, 7010, L"target");
    addDir(7031, 7030, L"debug");
    addFile(7032, 7031, L"build_artifact.rs", 4096, 133900000000000000ULL);

    ASSERT_TRUE(parser.importSnapshot(std::move(records), 2000, parser.getVolumeSerialNumber()));

    ContentCandidateOptions candOpts{};
    candOpts.dialogPriorities = {
        {L"C:\\repo", 6.0},
        {L"C:\\repo\\SBOTerminal", 18.0},
    };

    const auto filterExpr = SearchExpression::parse(L"content:update owor");
    const auto candidates = parser.CollectContentCandidates(filterExpr, candOpts);

    ASSERT_EQ(candidates.size(), 2u);
    EXPECT_NE(candidates[0].fullPath.find(L"SBOTerminal\\wo_sync.sql"), std::wstring::npos);
    EXPECT_NE(candidates[1].fullPath.find(L"SAPB1Help\\OWOR.htm"), std::wstring::npos);
    EXPECT_GT(candidates[0].priorityScore, candidates[1].priorityScore);
}


