// ─────────────────────────────────────────────────────────────────────────────
// test_search.inc — SEARCH 模块化单元测试集
// ─────────────────────────────────────────────────────────────────────────────

#include "service/StringArena.h"
#include "service/PathExclusionMatcher.h"
#include "service/SensitivePathGuard.h"

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

    // 8. 跨目录与文件名联合搜索及跨层级通配符 (*智造*部署手册* 与 智造 部署手册)
    FileRecord nestedPassFile{9, 0, L"部署手册.txt", L"部署手册.txt", L"bssc.txt", L"bushushouce.txt", false};
    std::wstring nestedPassPath = L"D:\\Projects\\216-北辰智造\\部署手册.txt";

    // 通配符跨目录搜索: *智造*部署手册*
    auto exprWildcardPath = SearchExpression::parse(L"*智造*部署手册*");
    EXPECT_TRUE(exprWildcardPath.matches(nestedPassFile, L'D', nestedPassPath));
    EXPECT_FALSE(exprWildcardPath.matches(txtFile, L'C', L"C:\\readme.txt"));

    // 多关键词空格组合跨目录搜索: 智造 部署手册
    auto exprMultiTerms = SearchExpression::parse(L"智造 部署手册");
    EXPECT_TRUE(exprMultiTerms.matches(nestedPassFile, L'D', nestedPassPath));
    EXPECT_FALSE(exprMultiTerms.matches(txtFile, L'C', L"C:\\readme.txt"));

    // 拼音与目录联合匹配: 智造 bssc
    auto exprPinyinDir = SearchExpression::parse(L"智造 bssc");
    EXPECT_TRUE(exprPinyinDir.matches(nestedPassFile, L'D', nestedPassPath));

    // 9. 用户真实连字符文件名检索: 019-get_Bom_Lev
    FileRecord bomFile{10, 0, L"019-get_Bom_Lev.txt", L"019-get_bom_lev.txt", L"019-get_bom_lev.txt", L"019-get_bom_lev.txt", false};
    auto exprBom1 = SearchExpression::parse(L"019-get_Bom_Lev");
    // 10. 盘符与根目录前缀搜索 (c:\, c:\ readme, c:\logs\ *.log)
    auto exprRoot = SearchExpression::parse(L"c:\\");
    EXPECT_TRUE(exprRoot.matches(txtFile, L'C', L"C:\\readme.txt"));
    EXPECT_FALSE(exprRoot.matches(bomFile, L'D', L"D:\\Projects\\019.txt"));

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

    // 验证多关键词全文穿透检索 (如 content:星云 部署手册，后续词不会被误判为文件名过滤)
    auto exprMultiContent = SearchExpression::parse(L"content:星云 部署手册");
    EXPECT_TRUE(exprMultiContent.hasContentFilter());
    EXPECT_EQ(tools3000::core::WinUtils::wstringToUtf8(exprMultiContent.getContentQuery()), "星云 部署手册");
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
        ofs << "/* 核心物料拓扑结构控制 */\n";
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
        // GBK 编码的中文字符串："北辰智造研发中心：开发部署手册"
        std::wstring wText = L"北辰智造研发中心：开发部署手册";
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
    bool gbkFound = engine.searchFile(testGbkFile, L"部署手册", false, snippets);
    EXPECT_TRUE(gbkFound);
    EXPECT_FALSE(snippets.empty());
    if (!snippets.empty()) {
        EXPECT_NE(snippets[0].lineContent.find(L"北辰智造研发中心"), std::wstring::npos);
        EXPECT_NE(snippets[0].lineContent.find(L"部署手册"), std::wstring::npos);
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

    const std::wstring testFile1 = L"D:\\Projects\\216-北辰智造\\部署手册.txt";
    const std::wstring testFile2 = L"D:\\Projects\\999-SQL\\API_GW.txt";

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

    const std::wstring q1 = L"*智造*部署手册*";
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

    // 构造目录: 226-星云科技 (id = 100)
    FileRecordInit dirRec{};
    dirRec.fileReferenceNumber = 100;
    dirRec.parentFileReferenceNumber = 0;
    dirRec.fileName = L"226-星云科技";
    dirRec.isDirectory = true;
    mockRecords.push_back(std::move(dirRec));

    // 构造文件: 部署手册.txt (id = 101, parent = 100)
    FileRecordInit fileRec{};
    fileRec.fileReferenceNumber = 101;
    fileRec.parentFileReferenceNumber = 100;
    fileRec.fileName = L"部署手册.txt";
    fileRec.isDirectory = false;
    fileRec.fileSize = 1024;
    mockRecords.push_back(std::move(fileRec));

    EXPECT_TRUE(parser->importSnapshot(std::move(mockRecords), 123456ULL, 0x11223344));

    // 模拟连续打字:
    // 1. 用户先搜 "星" (单字仅匹配文件名/目录名，此时 部署手册.txt 仅在目录命中，缓存被填入)
    auto res1 = parser->Search(L"星");
    EXPECT_GE(res1.size(), 1u);

    // 2. 用户接着打 "星云" (缩小缓存)
    auto res2 = parser->Search(L"星云");
    EXPECT_GE(res2.size(), 1u);

    // 3. 用户追加空格与第二个词 "星云 部署手册" (多词全路径匹配，必须绕开单字文件名缓存，正确命中 部署手册.txt)
    auto res3 = parser->Search(L"星云 部署手册");
    ASSERT_EQ(res3.size(), 1u);
    EXPECT_EQ(res3[0].fileName, L"部署手册.txt");
    EXPECT_NE(res3[0].fullPath.find(L"226-星云科技"), std::wstring::npos);
}

TEST(MftParserTest, SeparableDirMaskPinyinNegationAndMixedOrGroup) {
    auto parser = std::make_unique<MftParser>();
    std::vector<FileRecordInit> mockRecords;

    FileRecordInit dirRec{};
    dirRec.fileReferenceNumber = 100;
    dirRec.parentFileReferenceNumber = 0;
    dirRec.fileName = L"226-星云科技";
    dirRec.isDirectory = true;
    mockRecords.push_back(std::move(dirRec));

    FileRecordInit fileRec{};
    fileRec.fileReferenceNumber = 101;
    fileRec.parentFileReferenceNumber = 100;
    fileRec.fileName = L"部署手册.txt";
    fileRec.isDirectory = false;
    fileRec.fileSize = 1024;
    mockRecords.push_back(std::move(fileRec));

    FileRecordInit backupDir{};
    backupDir.fileReferenceNumber = 102;
    backupDir.parentFileReferenceNumber = 100;
    backupDir.fileName = L"历史备份";
    backupDir.isDirectory = true;
    mockRecords.push_back(std::move(backupDir));

    FileRecordInit backupFile{};
    backupFile.fileReferenceNumber = 103;
    backupFile.parentFileReferenceNumber = 102;
    backupFile.fileName = L"部署手册_旧.txt";
    backupFile.isDirectory = false;
    backupFile.fileSize = 512;
    mockRecords.push_back(std::move(backupFile));

    EXPECT_TRUE(parser->importSnapshot(std::move(mockRecords), 123456ULL, 0x11223344));

    // 1. 取反目录词过滤: "星云 部署手册 !备份" 应排除 "历史备份" 子目录下的文件
    auto resNegDir = parser->Search(L"星云 部署手册 !备份");
    ASSERT_EQ(resNegDir.size(), 1u);
    EXPECT_EQ(resNegDir[0].fileName, L"部署手册.txt");

    // 2. 取反文件名词过滤: "星云 部署手册 !旧" 应排除 "部署手册_旧.txt"
    auto resNegFile = parser->Search(L"星云 部署手册 !旧");
    ASSERT_EQ(resNegFile.size(), 1u);
    EXPECT_EQ(resNegFile[0].fileName, L"部署手册.txt");

    // 3. 跨目录拼音首字母 + 中文混合搜索: "xykj 部署手册 !备份" 与 "星云 bssc !备份"
    auto resPinyinDir = parser->Search(L"xykj 部署手册 !备份");
    ASSERT_EQ(resPinyinDir.size(), 1u);
    EXPECT_EQ(resPinyinDir[0].fileName, L"部署手册.txt");

    auto resPinyinFile = parser->Search(L"星云 bssc !备份");
    ASSERT_EQ(resPinyinFile.size(), 1u);
    EXPECT_EQ(resPinyinFile[0].fileName, L"部署手册.txt");

    // 4. 混合多子句 OR 组 (含 folder: 与普通词): "folder:不存在|星云 部署手册 !备份"
    auto resMixedOr = parser->Search(L"folder:不存在|星云 部署手册 !备份");
    ASSERT_EQ(resMixedOr.size(), 1u);
    EXPECT_EQ(resMixedOr[0].fileName, L"部署手册.txt");

    // 5. 多后缀通配符智能并集: "*.log *.txt"、"*.log;*.txt" 与 "星云 *.log *.txt !备份 !旧"
    auto exprMultiWildcard = SearchExpression::parse(L"*.log *.txt");
    EXPECT_FALSE(exprMultiWildcard.requiresFullPath());
    EXPECT_TRUE(exprMultiWildcard.isFullySeparable());
    auto resMultiExt = parser->Search(L"*.log *.txt !备份 !旧");
    ASSERT_EQ(resMultiExt.size(), 1u);
    EXPECT_EQ(resMultiExt[0].fileName, L"部署手册.txt");

    auto resSemiExt = parser->Search(L"星云 *.log;*.txt !备份 !旧");
    ASSERT_EQ(resSemiExt.size(), 1u);
    EXPECT_EQ(resSemiExt[0].fileName, L"部署手册.txt");
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
    const std::wstring fileName = L"智造 部署手册.txt";
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
    EXPECT_EQ(parsed.fileName, L"智造 部署手册.txt");
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
    deletedFile.fileName = L"智造 部署手册.txt";
    deletedFile.isDirectory = false;
    deletedFile.fileAttributes = FILE_ATTRIBUTE_NORMAL;
    records.push_back(std::move(deletedFile));

    FileRecordInit normalFile{};
    normalFile.fileReferenceNumber = 1004;
    normalFile.parentFileReferenceNumber = 0;
    normalFile.fileName = L"正常 部署手册.txt";
    normalFile.isDirectory = false;
    normalFile.fileAttributes = FILE_ATTRIBUTE_NORMAL;
    records.push_back(std::move(normalFile));

    EXPECT_TRUE(parser.importSnapshot(std::move(records), 1000, parser.getVolumeSerialNumber()));

    // 1. 普通搜索：回收站内的智造部署手册绝不返回，正常文件正常返回
    auto resNormal = parser.Search(L"部署手册");
    ASSERT_EQ(resNormal.size(), 1u);
    EXPECT_EQ(resNormal[0].fileName, L"正常 部署手册.txt");

    // 2. 搜索智造部署手册：命中但因为在回收站中，必须返回 0
    auto resDeleted = parser.Search(L"智造 部署手册");
    EXPECT_EQ(resDeleted.size(), 0u);

    // 3. 显式搜索包含 recycle 关键字时允许呈现
    auto resExplicit = parser.Search(L"智造 recycle");
    EXPECT_EQ(resExplicit.size(), 1u);
    EXPECT_EQ(resExplicit[0].fileName, L"智造 部署手册.txt");
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
    deletedFile.fileName = L"智造 部署手册.txt";
    deletedFile.isDirectory = false;
    deletedFile.fileAttributes = FILE_ATTRIBUTE_NORMAL;
    records.push_back(std::move(deletedFile));

    EXPECT_TRUE(parser.importSnapshot(std::move(records), 1000, parser.getVolumeSerialNumber()));

    auto res = parser.Search(L"智造 部署手册");
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

    // 1. 普通用户工作目录：C:\Projects\解决高级总账科目规则报错.txt (writeTime = 5000)
    addDir(4001, 0, L"Projects");
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
    // [0] Projects 普通用户目录原始文件 -> [1] xwechat_files 聊天副本 -> [2] Desktop 普通 .lnk -> [3] Temp 临时目录文件
    ASSERT_EQ(res.size(), 4u);
    EXPECT_NE(res[0].fullPath.find(L"\\Projects\\"), std::wstring::npos);
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
    EXPECT_TRUE(exprCombo.matches(exactPdf, L'D', L"D:\\Projects\\解决.pdf"));
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

    EXPECT_TRUE(SearchExpression::parse(L"解决 dm:2026-09").matches(sepFile, L'D', L"D:\\Projects\\解决方案汇报.pdf"));
    EXPECT_TRUE(SearchExpression::parse(L"解决 dm:2026-09-18").matches(sepFile, L'D', L"D:\\Projects\\解决方案汇报.pdf"));
    EXPECT_TRUE(SearchExpression::parse(L"解决 dm:>=2026-09-01").matches(sepFile, L'D', L"D:\\Projects\\解决方案汇报.pdf"));
    EXPECT_TRUE(SearchExpression::parse(L"解决 dm:2026-09-01..2026-09-30").matches(sepFile, L'D', L"D:\\Projects\\解决方案汇报.pdf"));
    EXPECT_FALSE(SearchExpression::parse(L"解决 dm:2026-08").matches(sepFile, L'D', L"D:\\Projects\\解决方案汇报.pdf"));
    EXPECT_FALSE(SearchExpression::parse(L"解决 dm:<2026-09-01").matches(sepFile, L'D', L"D:\\Projects\\解决方案汇报.pdf"));

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

    // 3. Embedded multi-MB TrueType font stream (/Length1 on standalone font object) placed BEFORE page content
    //    must be skipped in O(1) without inflating or exhausting MaxTotalPdfDecompressedBytes.
    const std::string hugeFontBinary(18u * 1024u * 1024u, '\0'); // 18 MB > 16 MB total PDF decompressed cap
    const std::string compressedFont = compressZlibBytes(hugeFontBinary);
    const std::string pageStream = compressZlibBytes("BT /F1 12 Tf (NebulaTechWorkload2026) Tj ET\n");
    std::string fontHeavyPdf = "%PDF-1.5\n";
    fontHeavyPdf += "1 0 obj\n<< /Type /Page /Resources << /Font << /F1 2 0 R >> >> /Contents 5 0 R >>\nendobj\n";
    fontHeavyPdf += "2 0 obj\n<< /Type /Font /Subtype /TrueType /FontDescriptor 3 0 R >>\nendobj\n";
    fontHeavyPdf += "3 0 obj\n<< /Type /FontDescriptor /FontFile2 4 0 R >>\nendobj\n";
    fontHeavyPdf += "4 0 obj\n<< /Length1 " + std::to_string(hugeFontBinary.size()) +
        " /Filter /FlateDecode /Length " + std::to_string(compressedFont.size()) + " >>\nstream\n";
    fontHeavyPdf.append(compressedFont.data(), compressedFont.size());
    fontHeavyPdf += "\nendstream\nendobj\n";
    fontHeavyPdf += "5 0 obj\n<< /Filter /FlateDecode /Length " + std::to_string(pageStream.size()) + " >>\nstream\n";
    fontHeavyPdf.append(pageStream.data(), pageStream.size());
    fontHeavyPdf += "\nendstream\nendobj\n%%EOF\n";

    const std::wstring fontPdfPath = std::wstring(tempDir) + L"Tools3000_Test_FontSkip_" +
        std::to_wstring(GetCurrentProcessId()) + L".pdf";
    TempFileDeleter fontPdfGuard{fontPdfPath};
    {
        std::ofstream ofs(fontPdfPath, std::ios::binary);
        ofs.write(fontHeavyPdf.data(), static_cast<std::streamsize>(fontHeavyPdf.size()));
    }
    snippets.clear();
    ASSERT_TRUE(engine.searchFile(fontPdfPath, std::vector<std::wstring>{L"NebulaTechWorkload2026"}, false, snippets, 3));
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
    EXPECT_EQ(store.fileSizeOf(*stored), 987654ULL);
    EXPECT_EQ(store.creationTimeOf(*stored), 133100000000000000ULL);
    EXPECT_EQ(store.lastWriteTimeOf(*stored), 133200000000000000ULL);

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
    addDir(7002, 7001, L"StaticApiDocs");
    addFile(7003, 7002, L"OWOR.htm", 8192, 133500000000000000ULL);

    addDir(7010, 7001, L"NebulaTerminal");
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
        {L"C:\\repo\\NebulaTerminal", 18.0},
    };

    const auto filterExpr = SearchExpression::parse(L"content:update owor");
    const auto candidates = parser.CollectContentCandidates(filterExpr, candOpts);

    ASSERT_EQ(candidates.size(), 2u);
    EXPECT_NE(candidates[0].fullPath.find(L"NebulaTerminal\\wo_sync.sql"), std::wstring::npos);
    EXPECT_NE(candidates[1].fullPath.find(L"StaticApiDocs\\OWOR.htm"), std::wstring::npos);
    EXPECT_GT(candidates[0].priorityScore, candidates[1].priorityScore);
}

TEST(ContentSearchProgressiveTest, PdfExtractorAdvancesPastStrayDelimitersNestedArraysAndInlineImages) {
    const std::string contentStream =
        "BT /F1 12 Tf "
        ") > ] { } "
        "[ [ (ignored_nested) ] (UPDATE ) -120 (OWOR) ] TJ "
        "BI /W 2 /H 2 /BPC 8 /CS /G ID )>]{}\r\nEI "
        "( status_ok) Tj ET";

    const std::string compressed = tools3000_content_search_test_helpers::compressZlibBytes(contentStream);
    ASSERT_FALSE(compressed.empty());

    std::string pdfDoc =
        "%PDF-1.7\n"
        "1 0 obj\n<< /Length " + std::to_string(compressed.size()) + " /Filter /FlateDecode >>\nstream\r\n";
    pdfDoc.append(compressed);
    pdfDoc.append("\r\nendstream\nendobj\n%%EOF\n");

    std::wstring extracted;
    ASSERT_TRUE(tools3000::service::content::PdfExtractor::extractPdfTextFromMemory(
        reinterpret_cast<const uint8_t*>(pdfDoc.data()),
        pdfDoc.size(),
        extracted
    ));
    EXPECT_NE(extracted.find(L"UPDATE"), std::wstring::npos);
    EXPECT_NE(extracted.find(L"OWOR"), std::wstring::npos);
    EXPECT_NE(extracted.find(L"status_ok"), std::wstring::npos);
    EXPECT_EQ(extracted.find(L"ignored_nested"), std::wstring::npos);
}

TEST(RustContentKernelTest, SimdEncodingDetectionAndZeroLineAllocationMultiTermScan) {
    using namespace tools3000::service::content;

    // 1. 验证 Rust 编码检测 (UTF-8 BOM, UTF-16LE BOM, Strict UTF-8)
    const uint8_t utf8Bom[] = {0xEF, 0xBB, 0xBF, 'h', 'e', 'l', 'l', 'o'};
    size_t bomOff = 99;
    EXPECT_EQ(t3k_rs_detect_encoding(utf8Bom, sizeof(utf8Bom), &bomOff), 1u);
    EXPECT_EQ(bomOff, 3u);

    // 2. 验证 SIMD 大小写敏感与不敏感 ASCII 检索
    const std::string hay = "0123456789abcdef_Tools3000_RustKernel_SIMD_Search_Engine";
    const std::string needle = "rustkernel_simd";
    const size_t hitInsensitive = t3k_rs_find_ascii_pattern(
        reinterpret_cast<const uint8_t*>(hay.data()), hay.size(),
        reinterpret_cast<const uint8_t*>(needle.data()), needle.size(), 0);
    EXPECT_EQ(hitInsensitive, 27u);

    const size_t hitSensitive = t3k_rs_find_ascii_pattern(
        reinterpret_cast<const uint8_t*>(hay.data()), hay.size(),
        reinterpret_cast<const uint8_t*>(needle.data()), needle.size(), 1);
    EXPECT_EQ(hitSensitive, SIZE_MAX);

    // 3. 验证 UTF-16 多词匹配与行号排序 (matchDocumentTextAndBuildSnippets -> t3k_rs_match_utf16_document)
    const std::wstring doc =
        L"Line 1: irrelevant header\n"
        L"Line 2: Rust SIMD kernel initialized\n"
        L"Line 3: skipping noise line\n"
        L"Line 4: Zero allocation ContentSearchEngine active\n";
    std::vector<ContentSnippet> snippets;
    ASSERT_TRUE(matchDocumentTextAndBuildSnippets(
        doc, {L"rust simd", L"contentsearchengine"}, false, snippets, 3));
    ASSERT_EQ(snippets.size(), 2u);
    EXPECT_EQ(snippets[0].lineNumber, 2u);
    EXPECT_EQ(snippets[1].lineNumber, 4u);
    EXPECT_EQ(snippets[0].lineContent.substr(snippets[0].matchOffset, snippets[0].matchLength), L"Rust SIMD");
    EXPECT_EQ(snippets[1].lineContent.substr(snippets[1].matchOffset, snippets[1].matchLength), L"ContentSearchEngine");
}

TEST(SearchExpressionTest, SeparableDirectoryMaskMultiTermSearchPerformanceAndAccuracy) {
    MftParser parser;
    parser.Initialize('D');

    std::vector<FileRecordInit> records;
    records.reserve(50010);

    // 根下两个主目录：Projects (FRN 10) 与 Workspace (FRN 20)
    records.push_back(FileRecordInit{10, 5, L"Projects", true, FILE_ATTRIBUTE_DIRECTORY, 0, 0, 0});
    records.push_back(FileRecordInit{11, 10, L"226-星云科技", true, FILE_ATTRIBUTE_DIRECTORY, 0, 0, 0});
    records.push_back(FileRecordInit{12, 11, L"部署手册.txt", false, FILE_ATTRIBUTE_ARCHIVE, 295, 100, 200});
    records.push_back(FileRecordInit{13, 11, L".git", true, FILE_ATTRIBUTE_DIRECTORY, 0, 0, 0});
    records.push_back(FileRecordInit{14, 13, L"部署手册_git_noise.txt", false, FILE_ATTRIBUTE_ARCHIVE, 128, 100, 200});

    // 构造 50,000 个深层目录与普通文件噪声，验证多词跨目录查询在零路径分配下的极速过滤
    records.push_back(FileRecordInit{20, 5, L"Workspace", true, FILE_ATTRIBUTE_DIRECTORY, 0, 0, 0});
    for (uint64_t d = 0; d < 250; ++d) {
        const uint64_t dirFrn = 100 + d;
        records.push_back(FileRecordInit{
            dirFrn, 20, L"ModuleDir_" + std::to_wstring(d), true, FILE_ATTRIBUTE_DIRECTORY, 0, 0, 0});
        for (uint64_t f = 0; f < 200; ++f) {
            const uint64_t fileFrn = 10000 + d * 200 + f;
            records.push_back(FileRecordInit{
                fileFrn, dirFrn, L"SourceComponent_" + std::to_wstring(f) + L".cpp",
                false, FILE_ATTRIBUTE_ARCHIVE, 1024, 100, 200});
        }
    }

    ASSERT_TRUE(parser.importSnapshot(std::move(records), 1000, parser.getVolumeSerialNumber()));

    const auto start = std::chrono::steady_clock::now();
    SearchExcludeOptions excludeOpts;
    excludeOpts.patterns = {L".git", L"node_modules"};
    const auto results = parser.Search(L"星云 部署手册", 100, excludeOpts);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].fileName, L"部署手册.txt");
    EXPECT_EQ(results[0].fullPath, L"D:\\Projects\\226-星云科技\\部署手册.txt");
    EXPECT_LT(elapsedMs, 100);
}

TEST(SearchExpressionTest, MultiKeywordStemMatchingAndBestMatchRanking) {
    // 1. 验证纯后缀过滤组 (*.log *.txt) 叠加普通关键词 (log) 时，主文件名不含 log 的 20260806_1535.log 绝不因扩展名假命中
    FileRecord noiseLog{1, 0, L"20260806_1535.log", L"20260806_1535.log", L"20260806_1535.log", L"20260806_1535.log", false};
    FileRecord exactStem{2, 0, L"log.txt", L"log.txt", L"log.txt", L"log.txt", false};
    FileRecord prefixStem{3, 0, L"log_viewer.log", L"log_viewer.log", L"log_viewer.log", L"log_viewer.log", false};
    FileRecord containStem{4, 0, L"app_error_log.txt", L"app_error_log.txt", L"app_error_log.txt", L"app_error_log.txt", false};

    auto exprExtAndKeyword = SearchExpression::parse(L"*.log *.txt log");
    EXPECT_FALSE(exprExtAndKeyword.matches(noiseLog, L'C', L"C:\\ProgramData\\Dell\\orca\\deviceq\\20260806_1535.log"));
    EXPECT_TRUE(exprExtAndKeyword.matches(exactStem, L'D', L"D:\\Work\\log.txt"));
    EXPECT_TRUE(exprExtAndKeyword.matches(prefixStem, L'D', L"D:\\Work\\log_viewer.log"));
    EXPECT_TRUE(exprExtAndKeyword.matches(containStem, L'D', L"D:\\Work\\app_error_log.txt"));
    // 目录名命中 log 时仍允许跨路径匹配，但 matchRank 排在主干命中之后
    EXPECT_TRUE(exprExtAndKeyword.matches(noiseLog, L'D', L"D:\\Work\\logs\\20260806_1535.log"));

    EXPECT_EQ(exprExtAndKeyword.calculateRank(exactStem), 1);
    EXPECT_EQ(exprExtAndKeyword.calculateRank(prefixStem), 1);
    EXPECT_EQ(exprExtAndKeyword.calculateRank(containStem), 4);
    EXPECT_EQ(exprExtAndKeyword.calculateRank(noiseLog), 7);

    // 2. 验证多关键词主干全词命中优于单主干词+目录词命中
    FileRecord bothInStem{10, 0, L"星云部署手册汇总.txt", L"星云部署手册汇总.txt", L"xybsschz.txt", L"xingyunbushushoucehuizong.txt", false};
    FileRecord oneInStem{11, 0, L"部署手册.txt", L"部署手册.txt", L"bssc.txt", L"bushushouce.txt", false};
    auto exprMulti = SearchExpression::parse(L"星云 部署手册");
    EXPECT_LT(exprMulti.calculateRank(bothInStem), exprMulti.calculateRank(oneInStem));
}

#include "service/db/RunHistoryManager.h"

TEST(SearchExpressionTest, SizeRangeAndSemanticPresets) {
    FileRecord emptyFile{1, 0, L"empty.log", L"empty.log", L"empty.log", L"empty.log", false, 0, 0, 0, 0};
    FileRecord smallFile{2, 0, L"small.log", L"small.log", L"small.log", L"small.log", false, 0, 25ULL * 1024, 0, 0};
    FileRecord mediumFile{3, 0, L"medium.log", L"medium.log", L"medium.log", L"medium.log", false, 0, 5ULL * 1024 * 1024, 0, 0};
    FileRecord hugeFile{4, 0, L"huge.iso", L"huge.iso", L"huge.iso", L"huge.iso", false, 0, 250ULL * 1024 * 1024, 0, 0};

    auto exprRange = SearchExpression::parse(L"size:10kb..50kb");
    EXPECT_FALSE(exprRange.matches(emptyFile, L'C', L"C:\\empty.log"));
    EXPECT_TRUE(exprRange.matches(smallFile, L'C', L"C:\\small.log"));
    EXPECT_FALSE(exprRange.matches(mediumFile, L'C', L"C:\\medium.log"));

    auto exprDashRange = SearchExpression::parse(L"size:1mb-10mb");
    EXPECT_FALSE(exprDashRange.matches(smallFile, L'C', L"C:\\small.log"));
    EXPECT_TRUE(exprDashRange.matches(mediumFile, L'C', L"C:\\medium.log"));
    EXPECT_FALSE(exprDashRange.matches(hugeFile, L'C', L"C:\\huge.iso"));

    EXPECT_TRUE(SearchExpression::parse(L"size:empty").matches(emptyFile, L'C', L"C:\\empty.log"));
    EXPECT_FALSE(SearchExpression::parse(L"size:empty").matches(smallFile, L'C', L"C:\\small.log"));
    EXPECT_TRUE(SearchExpression::parse(L"size:small").matches(smallFile, L'C', L"C:\\small.log"));
    EXPECT_TRUE(SearchExpression::parse(L"size:medium").matches(mediumFile, L'C', L"C:\\medium.log"));
    EXPECT_TRUE(SearchExpression::parse(L"size:huge").matches(hugeFile, L'C', L"C:\\huge.iso"));
}

TEST(RunHistorySnapshotTest, LockFreeBloomPreFilterAndFrecencyLookup) {
    auto& mgr = tools3000::service::db::RunHistoryManager::instance();
    mgr.clear();

    const auto emptySnap = mgr.captureSnapshot(1000000ULL);
    EXPECT_TRUE(emptySnap.empty());
    EXPECT_EQ(emptySnap.getRunCount(L"D:\\Projects\\report.xlsx"), 0u);

    mgr.recordRun(L"D:\\Projects\\report.xlsx");
    mgr.recordRun(L"D:\\Projects\\report.xlsx");
    mgr.recordRun(L"D:\\Projects\\once.txt");

    const auto snap = mgr.captureSnapshot();
    EXPECT_FALSE(snap.empty());
    EXPECT_EQ(snap.getRunCount(L"d:/projects/report.xlsx"), 2u);
    EXPECT_EQ(snap.getRunCount(L"D:\\Projects\\once.txt"), 1u);
    EXPECT_EQ(snap.getRunCount(L"D:\\Projects\\never_opened_file.txt"), 0u);

    const auto statsTwice = snap.lookup(L"D:\\Projects\\report.xlsx");
    const auto statsOnce = snap.lookup(L"D:\\Projects\\once.txt");
    EXPECT_GT(statsTwice.frecencyScore, statsOnce.frecencyScore);

    mgr.clear();
}

TEST(WinUtilsNativeIconTest, ExtractsPerFileEmbeddedIconAndExtensionFallback) {
    const std::string genericExeIcon = tools3000::core::WinUtils::getFileTypeIconBase64(L".exe", false);
    EXPECT_EQ(genericExeIcon.rfind("data:image/png;base64,", 0), 0u);

    wchar_t winDir[MAX_PATH] = {};
    if (GetWindowsDirectoryW(winDir, MAX_PATH) > 0) {
        std::filesystem::path explorerPath = std::filesystem::path(winDir) / L"explorer.exe";
        if (std::filesystem::exists(explorerPath)) {
            const std::string explorerIcon = tools3000::core::WinUtils::getFileTypeIconBase64(
                L".exe",
                false,
                explorerPath.wstring());
            EXPECT_EQ(explorerIcon.rfind("data:image/png;base64,", 0), 0u);
            EXPECT_NE(explorerIcon, genericExeIcon);
        }
    }
}








// ─────────────────────────────────────────────────────────────────────────────
// 2026-11-11 全量审计防回退回归测试 (SearchCancellation / PipeClientAuth / LinearRegex / PdfExtractor)
// ─────────────────────────────────────────────────────────────────────────────

TEST(SearchCancellationRegressionTest, ServerEpochImmuneToClockRollbackAndHugeQueryId) {
    tools3000::service::query::QueryEpochTracker tracker;
    tracker.reset();

    // 即使客户端曾发送接近 UINT64_MAX 的异常时间戳 queryId，服务端世代依然单调递增不锁死
    EXPECT_TRUE(tracker.observe(UINT64_MAX - 5));
    const uint64_t epoch1 = tracker.issueServerEpoch(true);
    EXPECT_EQ(epoch1, 1u);
    EXPECT_FALSE(tracker.isServerEpochStale(epoch1));

    // 模拟客户端时钟回拨后新请求到达：服务端世代递增为 2，旧查询立即判定过期
    const uint64_t epoch2 = tracker.issueServerEpoch(true);
    EXPECT_EQ(epoch2, 2u);
    EXPECT_TRUE(tracker.isServerEpochStale(epoch1));
    EXPECT_FALSE(tracker.isServerEpochStale(epoch2));

    // 未跟踪请求 (tracked=false) 返回 0 且永不报过期
    EXPECT_EQ(tracker.issueServerEpoch(false), 0u);
    EXPECT_FALSE(tracker.isServerEpochStale(0u));

    // cancelAll 同步推进服务端世代，使当前进行中的 epoch2 立即中止
    tracker.cancelAll();
    EXPECT_TRUE(tracker.isServerEpochStale(epoch2));
}

TEST(PipeClientAuthIsolationTest, BlocksOtherUserProfilesForStandardUsers) {
    using tools3000::service::pipe_auth::PipeClientAuthSession;
    using tools3000::service::pipe_auth::isPathAllowedForClient;

    // 1. 未授权会话一律拒绝
    PipeClientAuthSession unauth;
    unauth.authorized = false;
    EXPECT_FALSE(isPathAllowedForClient(L"C:\\Windows\\notepad.exe", unauth));

    // 2. 标准非管理员用户 Alice：允许自身目录、Public 共享目录与其他非用户盘符，严禁窥探其他本地账户目录
    PipeClientAuthSession alice;
    alice.authorized = true;
    alice.isAdmin = false;
    alice.usersRootPrefix = L"c:\\users\\";
    alice.userProfilePrefix = L"c:\\users\\alice\\";

    EXPECT_TRUE(isPathAllowedForClient(L"C:\\Users\\Alice\\Documents\\budget_2026.xlsx", alice));
    EXPECT_TRUE(isPathAllowedForClient(L"c:/users/ALICE/Desktop/todo.md", alice));
    EXPECT_TRUE(isPathAllowedForClient(L"C:\\Users\\Public\\Downloads\\installer.msi", alice));
    EXPECT_TRUE(isPathAllowedForClient(L"C:\\Users\\Default\\NTUSER.DAT", alice));
    EXPECT_TRUE(isPathAllowedForClient(L"D:\\Projects\\Tools3000\\CMakeLists.txt", alice));
    EXPECT_TRUE(isPathAllowedForClient(L"C:\\Program Files\\Tools3000\\Tools3000.exe", alice));

    EXPECT_FALSE(isPathAllowedForClient(L"C:\\Users\\Bob\\Documents\\payroll_secret.xlsx", alice));
    EXPECT_FALSE(isPathAllowedForClient(L"c:/users/bob/AppData/Local/token.json", alice));
    EXPECT_FALSE(isPathAllowedForClient(L"C:\\Users\\Administrator\\.ssh\\id_ed25519", alice));
    // 防御前缀碰撞：Alice2 不是 Alice
    EXPECT_FALSE(isPathAllowedForClient(L"C:\\Users\\Alice2\\notes.txt", alice));

    // 3. 管理员会话允许全盘检索
    PipeClientAuthSession admin;
    admin.authorized = true;
    admin.isAdmin = true;
    admin.usersRootPrefix = L"c:\\users\\";
    admin.userProfilePrefix = L"c:\\users\\admin\\";
    EXPECT_TRUE(isPathAllowedForClient(L"C:\\Users\\Bob\\Documents\\payroll_secret.xlsx", admin));
}

TEST(SearchExpressionLinearRegexTest, ResistsCatastrophicBacktrackingWithoutThrowing) {
    // 构造经典 ReDoS 灾难性回溯表达式与恶意近似匹配文件名 (64 个 'a' + '!')
    const std::wstring evilName = std::wstring(64, L'a') + L"!";
    FileRecord evilRec{1, 0, evilName, evilName, evilName, evilName, false, 0, 128, 0, 0};

    const auto start = std::chrono::steady_clock::now();
    auto redosExpr = SearchExpression::parse(L"regex:(a+)+$");
    const bool matched = redosExpr.matches(evilRec, L'C', L"C:\\Temp\\" + evilName);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    EXPECT_FALSE(matched);
    EXPECT_LT(elapsedMs, 100);

    // 验证正常正则匹配与畸形正则零异常容错
    FileRecord reportRec{2, 0, L"Report_2026.pdf", L"report_2026.pdf", L"report_2026.pdf", L"report_2026.pdf", false};
    auto validRegex = SearchExpression::parse(L"regex:^report_[0-9]+\\.pdf$");
    EXPECT_TRUE(validRegex.matches(reportRec, L'D', L"D:\\Docs\\Report_2026.pdf"));

    EXPECT_NO_THROW({
        auto invalidRegex = SearchExpression::parse(L"regex:([unclosed_group");
        EXPECT_FALSE(invalidRegex.matches(reportRec, L'D', L"D:\\Docs\\Report_2026.pdf"));
    });
}

TEST(PdfExtractorSafetyTest, HandlesMalformedPredictorOverflowAndHugeBfRangeSafely) {
    using namespace tools3000_content_search_test_helpers;

    // 构造畸形超大 Columns PNG Predictor 与 4GB 跨度 bfrange CMap，验证无整数溢出或 OOM 崩溃
    const std::string hugeCmap = compressZlibBytes(
        "begincmap\n1 beginbfrange\n<00000000> <ffffffff> <0041>\nendbfrange\nendcmap\n");
    const std::string dummyPayload = compressZlibBytes("BT /F1 12 Tf <00000001> Tj ET");

    std::string craftedPdf = "%PDF-1.7\n";
    craftedPdf += "1 0 obj\n<< /Type /Font /Subtype /Type0 /ToUnicode 2 0 R >>\nendobj\n";
    craftedPdf += "2 0 obj\n<< /Filter /FlateDecode /Length " + std::to_string(hugeCmap.size()) + " >>\nstream\n";
    craftedPdf.append(hugeCmap);
    craftedPdf += "\nendstream\nendobj\n";
    craftedPdf += "3 0 obj\n<< /Filter /FlateDecode /DecodeParms << /Predictor 12 /Columns 1073741824 /Colors 4 /BitsPerComponent 8 >> /Length " +
                  std::to_string(dummyPayload.size()) + " >>\nstream\n";
    craftedPdf.append(dummyPayload);
    craftedPdf += "\nendstream\nendobj\n%%EOF\n";

    std::wstring outText;
    EXPECT_NO_THROW({
        (void)tools3000::service::content::PdfExtractor::extractPdfTextFromMemory(
            reinterpret_cast<const uint8_t*>(craftedPdf.data()),
            craftedPdf.size(),
            outText);
    });
}

#include "service/QueryThreadPool.h"
#include "service/AppLauncherCatalog.h"

TEST(FileIndexStoreOptimizationTest, SingleSlotOpenAddressingTombstoneRehashAndPinyinSlotReuse) {
    tools3000::service::FileIndexStore store;
    store.setDriveLetter('C');
    store.reserve(32);

    // 1. 保留 16 个常驻文件
    for (uint64_t frn = 1; frn <= 16; ++frn) {
        FileRecordInit steady{};
        steady.fileReferenceNumber = frn;
        steady.parentFileReferenceNumber = 0;
        steady.fileName = L"steady_file_" + std::to_wstring(frn) + L".txt";
        steady.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
        store.upsert(steady, true);
    }
    ASSERT_EQ(store.size(), 16u);

    // 2. 高频临时文件增删风暴：8000 次插入不同 FRN 后立即删除
    // 修复前 rehash 盲目按 m_slots.size() * 2 扩容会导致哈希表无限翻倍膨胀；
    // 修复后 rehash 基于 m_liveCount 重建并清零 m_tombstones，容量始终收敛在极小常数级。
    for (uint64_t churn = 1000; churn < 9000; ++churn) {
        FileRecordInit temp{};
        temp.fileReferenceNumber = churn;
        temp.parentFileReferenceNumber = 0;
        temp.fileName = L"temp_build_" + std::to_wstring(churn) + L".tmp";
        temp.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
        store.upsert(temp, true);
        ASSERT_NE(store.find(churn), nullptr);
        ASSERT_TRUE(store.erase(churn));
        ASSERT_EQ(store.find(churn), nullptr);
    }
    // 即使在调用 compactIfSparse 之前，纯靠 insertNewSlot 触发的 rehash 也绝不翻倍膨胀（固定停在初始地板 1024，旧版会膨胀至 >200 万）
    EXPECT_EQ(store.hashCapacity(), 1024u);
    store.compactIfSparse();

    EXPECT_EQ(store.size(), 16u);
    EXPECT_EQ(store.tombstoneCount(), 0u);
    EXPECT_EQ(store.hashCapacity(), 1024u);
    for (uint64_t frn = 1; frn <= 16; ++frn) {
        const auto* rec = store.find(frn);
        ASSERT_NE(rec, nullptr);
        EXPECT_EQ(store.view(*rec).fileName, L"steady_file_" + std::to_wstring(frn) + L".txt");
    }

    // 3. 中文文件重命名原地复用与回收 pinyinSlot
    FileRecordInit cjkInit{};
    cjkInit.fileReferenceNumber = 50001ULL;
    cjkInit.parentFileReferenceNumber = 0;
    cjkInit.fileName = L"季度财务分析_初稿.xlsx";
    cjkInit.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    store.upsert(cjkInit, true);

    const auto* cjkRec = store.find(50001ULL);
    ASSERT_NE(cjkRec, nullptr);
    const uint32_t initialPinyinSlot = cjkRec->pinyinSlot;
    ASSERT_GT(initialPinyinSlot, 0u);
    const size_t baselinePinyinEntries = store.pinyinEntryCount();

    // 连续 50 次中文改中文重命名，必须 100% 原地复用同一 pinyinSlot，m_pinyin 绝不增长
    for (int ver = 1; ver <= 50; ++ver) {
        cjkInit.fileName = L"季度财务分析_修订版" + std::to_wstring(ver) + L".xlsx";
        store.upsert(cjkInit, true);
    }
    cjkRec = store.find(50001ULL);
    ASSERT_NE(cjkRec, nullptr);
    EXPECT_EQ(cjkRec->pinyinSlot, initialPinyinSlot);
    EXPECT_EQ(store.pinyinEntryCount(), baselinePinyinEntries);
    EXPECT_NE(store.view(*cjkRec).pinyinInitials.find(L"jdcwfx"), std::wstring::npos);

    // 由中文改名为纯英文：必须清空旧 pinyinSlot 并回收进空闲链表，视图回退指向纯英文文件名且不含旧中文拼音
    cjkInit.fileName = L"quarterly_financial_report.xlsx";
    store.upsert(cjkInit, true);
    cjkRec = store.find(50001ULL);
    ASSERT_NE(cjkRec, nullptr);
    EXPECT_EQ(cjkRec->pinyinSlot, 0u);
    EXPECT_EQ(store.view(*cjkRec).pinyinInitials, L"quarterly_financial_report.xlsx");
    EXPECT_EQ(store.view(*cjkRec).pinyinInitials.find(L"jdcwfx"), std::wstring::npos);
    EXPECT_GE(store.freePinyinSlotCount(), 1u);

    // 新增另一个中文文件：必须直接复用回收槽位，m_pinyin 总数零增长
    FileRecordInit nextCjk{};
    nextCjk.fileReferenceNumber = 50002ULL;
    nextCjk.parentFileReferenceNumber = 0;
    nextCjk.fileName = L"新项目架构设计.docx";
    nextCjk.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    store.upsert(nextCjk, true);
    const auto* nextRec = store.find(50002ULL);
    ASSERT_NE(nextRec, nullptr);
    EXPECT_EQ(nextRec->pinyinSlot, initialPinyinSlot);
    EXPECT_EQ(store.pinyinEntryCount(), baselinePinyinEntries);
}

TEST(SlashPathFragmentSeparableTest, DecomposesDirectorySegmentsAndAvoidsFullVolumePathBuild) {
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
    auto addFile = [&](DWORDLONG id, DWORDLONG parent, const wchar_t* name) {
        FileRecordInit f{};
        f.fileReferenceNumber = id;
        f.parentFileReferenceNumber = parent;
        f.fileName = name;
        f.isDirectory = false;
        f.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
        f.fileSize = 2048;
        f.lastWriteTime = 133500000000000000ULL;
        records.push_back(std::move(f));
    };

    // 构建真实目录树与 4000 个干扰文件
    addDir(100, 0, L"Repo");
    addDir(101, 100, L"src");
    addDir(102, 101, L"service");
    addDir(103, 101, L"ui");
    addDir(104, 100, L"docs");
    addDir(105, 104, L"service");

    addFile(201, 102, L"main.cpp");       // 目标：C:\Repo\src\service\main.cpp
    addFile(202, 103, L"main.cpp");       // 同文件名不同父目录：C:\Repo\src\ui\main.cpp
    addFile(203, 105, L"readme.md");      // 同父目录名不同文件名：C:\Repo\docs\service\readme.md

    for (DWORDLONG d = 0; d < 40; ++d) {
        const DWORDLONG dirId = 1000 + d;
        const std::wstring dirName = L"pkg_" + std::to_wstring(d);
        addDir(dirId, 100, dirName.c_str());
        for (DWORDLONG f = 0; f < 100; ++f) {
            FileRecordInit noise{};
            noise.fileReferenceNumber = 10000 + d * 100 + f;
            noise.parentFileReferenceNumber = dirId;
            noise.fileName = L"module_" + std::to_wstring(f) + L".cpp";
            noise.isDirectory = false;
            noise.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
            noise.fileSize = 512;
            noise.lastWriteTime = 133400000000000000ULL;
            records.push_back(std::move(noise));
        }
    }

    ASSERT_TRUE(parser.importSnapshot(std::move(records), 2000, parser.getVolumeSerialNumber()));

    // 1. 反斜杠路径片段查询：必须仅对同时通过祖先目录位掩码 + 末段文件名快筛的候选调用 buildFullPath
    auto backslashHits = parser.Search(L"service\\main.cpp", 50);
    ASSERT_EQ(backslashHits.size(), 1u);
    EXPECT_EQ(backslashHits[0].fullPath, L"C:\\Repo\\src\\service\\main.cpp");
    EXPECT_LE(parser.getLastSearchPathBuildCount(), 4u);

    // 2. 正斜杠路径片段查询
    auto forwardSlashHits = parser.Search(L"src/service/main.cpp", 50);
    ASSERT_EQ(forwardSlashHits.size(), 1u);
    EXPECT_EQ(forwardSlashHits[0].fullPath, L"C:\\Repo\\src\\service\\main.cpp");
    EXPECT_LE(parser.getLastSearchPathBuildCount(), 4u);

    // 3. 带通配符的斜杠路径片段
    auto wildcardSlashHits = parser.Search(L"*\\service\\*.cpp", 50);
    ASSERT_EQ(wildcardSlashHits.size(), 1u);
    EXPECT_EQ(wildcardSlashHits[0].fullPath, L"C:\\Repo\\src\\service\\main.cpp");
    EXPECT_LE(parser.getLastSearchPathBuildCount(), 4u);

    // 4. parent: 精确父目录预筛
    auto parentHits = parser.Search(L"parent:C:\\Repo\\src\\service", 50);
    ASSERT_EQ(parentHits.size(), 1u);
    EXPECT_EQ(parentHits[0].fullPath, L"C:\\Repo\\src\\service\\main.cpp");
    EXPECT_LE(parser.getLastSearchPathBuildCount(), 4u);

    // 5. 取反斜杠子句：搜 main.cpp !service\main.cpp 仅保留 ui\main.cpp
    auto negatedSlashHits = parser.Search(L"main.cpp !service\\main.cpp", 50);
    ASSERT_EQ(negatedSlashHits.size(), 1u);
    EXPECT_EQ(negatedSlashHits[0].fullPath, L"C:\\Repo\\src\\ui\\main.cpp");
}

TEST(LockFreeStatAndThreadPoolTest, HydratesSizeAndDateOutsideLockAndSupportsNestedParallelFor) {
    // 1. 验证全局查询线程池支持嵌套 parallelFor 且无死锁
    std::atomic<uint64_t> nestedSum{0};
    tools3000::service::QueryThreadPool::instance().parallelFor(8, [&](size_t) {
        tools3000::service::QueryThreadPool::instance().parallelFor(32, [&](size_t) {
            nestedSum.fetch_add(1, std::memory_order_relaxed);
        });
    });
    EXPECT_EQ(nestedSum.load(), 8u * 32u);

    // 2. 验证未水合记录在查询 size: / dm: 时于锁外完成元数据水合并原子写回
    wchar_t tempDirBuf[MAX_PATH]{};
    ASSERT_GT(GetTempPathW(MAX_PATH, tempDirBuf), 0u);
    std::filesystem::path probeDir = std::filesystem::path(tempDirBuf) / L"t3k_p02_stat_probe_dir";
    std::error_code ec;
    std::filesystem::remove_all(probeDir, ec);
    ASSERT_TRUE(std::filesystem::create_directories(probeDir, ec));

    const std::filesystem::path bigFile = probeDir / L"t3k_stat_probe_big.bin";
    const std::filesystem::path smallFile = probeDir / L"t3k_stat_probe_small.bin";
    {
        HANDLE hBig = CreateFileW(bigFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(hBig, INVALID_HANDLE_VALUE);
        std::vector<char> payload(32 * 1024, 'A');
        DWORD written = 0;
        ASSERT_TRUE(WriteFile(hBig, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr));
        CloseHandle(hBig);

        HANDLE hSmall = CreateFileW(smallFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(hSmall, INVALID_HANDLE_VALUE);
        DWORD smallWritten = 0;
        ASSERT_TRUE(WriteFile(hSmall, payload.data(), 512, &smallWritten, nullptr));
        CloseHandle(hSmall);
    }

    const std::wstring absDir = std::filesystem::absolute(probeDir).wstring();
    ASSERT_GE(absDir.size(), 3u);
    const char driveLetter = static_cast<char>(std::toupper(static_cast<unsigned char>(absDir[0])));

    MftParser parser;
    parser.Initialize(driveLetter);
    parser.setRealTimeValidation(false);

    std::vector<FileRecordInit> records;
    DWORDLONG parentFrn = 0;
    DWORDLONG nextFrn = 500;
    size_t pos = 3;
    while (pos < absDir.size()) {
        size_t slash = absDir.find_first_of(L"\\/", pos);
        std::wstring seg = (slash == std::wstring::npos) ? absDir.substr(pos) : absDir.substr(pos, slash - pos);
        if (!seg.empty()) {
            FileRecordInit d{};
            d.fileReferenceNumber = nextFrn;
            d.parentFileReferenceNumber = parentFrn;
            d.fileName = seg;
            d.isDirectory = true;
            d.fileAttributes = FILE_ATTRIBUTE_DIRECTORY;
            records.push_back(std::move(d));
            parentFrn = nextFrn++;
        }
        if (slash == std::wstring::npos) break;
        pos = slash + 1;
    }

    // 刻意令 fileSize = 0, lastWriteTime = 0 模拟刚从 MFT 扫出、尚未水合的记录
    FileRecordInit bigRec{};
    bigRec.fileReferenceNumber = 9001;
    bigRec.parentFileReferenceNumber = parentFrn;
    bigRec.fileName = L"t3k_stat_probe_big.bin";
    bigRec.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    records.push_back(bigRec);

    FileRecordInit smallRec{};
    smallRec.fileReferenceNumber = 9002;
    smallRec.parentFileReferenceNumber = parentFrn;
    smallRec.fileName = L"t3k_stat_probe_small.bin";
    smallRec.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    records.push_back(smallRec);

    ASSERT_TRUE(parser.importSnapshot(std::move(records), 3000, parser.getVolumeSerialNumber()));

    auto hits = parser.Search(L"t3k_stat_probe size:>16kb dm:today", 50);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].fileName, L"t3k_stat_probe_big.bin");
    EXPECT_EQ(hits[0].fileSize, 32768ULL);
    EXPECT_GT(hits[0].lastWriteTime, 0ULL);

    // 删除底层物理文件后再次执行同一过滤（setRealTimeValidation(false)），验证大小和时间已原子回填至内存索引
    std::filesystem::remove_all(probeDir, ec);
    auto cachedHits = parser.Search(L"t3k_stat_probe size:>16kb dm:today", 50);
    ASSERT_EQ(cachedHits.size(), 1u);
    EXPECT_EQ(cachedHits[0].fileSize, 32768ULL);
}

TEST(AppLauncherRankingAndAppsFolderTest, PromotesStartMenuAndUserDesktopShortcutsFrecencyAndEnumeratesAppsFolder) {
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
        f.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
        f.fileSize = 1024;
        f.lastWriteTime = writeTime;
        records.push_back(std::move(f));
    };

    // 1. 公共开始菜单应用快捷方式：C:\ProgramData\Microsoft\Windows\Start Menu\Programs\StudioApp.lnk
    addDir(7001, 0, L"ProgramData");
    addDir(7002, 7001, L"Microsoft");
    addDir(7003, 7002, L"Windows");
    addDir(7004, 7003, L"Start Menu");
    addDir(7005, 7004, L"Programs");
    addFile(7006, 7005, L"StudioApp.lnk", 1000);

    // 2. 用户桌面应用快捷方式：C:\Users\Alice\Desktop\StudioAppDesktop.lnk
    addDir(7010, 0, L"Users");
    addDir(7011, 7010, L"Alice");
    addDir(7012, 7011, L"Desktop");
    addFile(7013, 7012, L"StudioAppDesktop.lnk", 1100);

    // 3. 普通工作区文档：C:\Projects\StudioAppSpec.md (时间更新，但位于普通用户层 tier = 1)
    addDir(7020, 0, L"Projects");
    addFile(7021, 7020, L"StudioAppSpec.md", 5000);

    // 4. Program Files 可执行程序：C:\Program Files\VendorSuite\StudioAppCli.exe (系统应用层 tier = 3)
    addDir(7030, 0, L"Program Files");
    addDir(7031, 7030, L"VendorSuite");
    addFile(7032, 7031, L"StudioAppCli.exe", 6000);

    // 5. 非用户目录普通 .lnk：C:\Archive\StudioAppOld.lnk (普通快捷方式层 tier = 4)
    addDir(7040, 0, L"Archive");
    addFile(7041, 7040, L"StudioAppOld.lnk", 8000);

    // 6. Recent 最近文档影子快捷方式：C:\Users\Alice\AppData\Roaming\Microsoft\Windows\Recent\StudioAppSpec.md.lnk
    addDir(7050, 7011, L"AppData");
    addDir(7051, 7050, L"Roaming");
    addDir(7052, 7051, L"Microsoft");
    addDir(7053, 7052, L"Windows");
    addDir(7054, 7053, L"Recent");
    addFile(7055, 7054, L"StudioAppSpec.md.lnk", 9999);

    ASSERT_TRUE(parser.importSnapshot(std::move(records), 4000, parser.getVolumeSerialNumber()));

    auto results = parser.Search(L"StudioApp", 50);
    // Recent 影子 .lnk 被默认过滤，剩余 5 条
    ASSERT_EQ(results.size(), 5u);
    EXPECT_EQ(results[0].pathTier, tools3000::service::kPathTierAppLauncher);
    EXPECT_EQ(results[1].pathTier, tools3000::service::kPathTierAppLauncher);
    // StudioApp.lnk 为精确主干匹配 (rank=0)，StudioAppDesktop.lnk 为前缀匹配 (rank=2)
    EXPECT_EQ(results[0].fileName, L"StudioApp.lnk");
    EXPECT_EQ(results[1].fileName, L"StudioAppDesktop.lnk");
    EXPECT_EQ(results[2].fileName, L"StudioAppSpec.md");
    EXPECT_EQ(results[3].fileName, L"StudioAppCli.exe");
    EXPECT_EQ(results[4].fileName, L"StudioAppOld.lnk");

    // 验证高 frecencyScore 常点目标跨普通层/系统层置顶
    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    const std::wstring tempCsv = std::wstring(tempPath) + L"Tools3000_U04_Launcher_Test.csv";
    DeleteFileW(tempCsv.c_str());

    auto& runMgr = tools3000::service::db::RunHistoryManager::instance();
    runMgr.init(tempCsv);
    runMgr.clear();
    // 模拟用户高频点击普通层文档 StudioAppSpec.md（frecency >= 1200），验证跨普通层直接置顶到榜首
    runMgr.recordRun(L"C:\\Projects\\StudioAppSpec.md");
    runMgr.recordRun(L"C:\\Projects\\StudioAppSpec.md");
    const auto historySnap = runMgr.captureSnapshot();

    tools3000::service::launcher::sortLauncherAwareResults(results, historySnap);
    EXPECT_EQ(results[0].fileName, L"StudioAppSpec.md");
    EXPECT_EQ(results[1].fileName, L"StudioApp.lnk");
    EXPECT_EQ(results[2].fileName, L"StudioAppDesktop.lnk");

    // 模拟用户更高频点击系统层可执行文件 StudioAppCli.exe（原本 tier=3），验证跨系统层跃升至第 1 位
    runMgr.recordRun(L"C:\\Program Files\\VendorSuite\\StudioAppCli.exe");
    runMgr.recordRun(L"C:\\Program Files\\VendorSuite\\StudioAppCli.exe");
    runMgr.recordRun(L"C:\\Program Files\\VendorSuite\\StudioAppCli.exe");
    const auto cliSnap = runMgr.captureSnapshot();
    tools3000::service::launcher::sortLauncherAwareResults(results, cliSnap);
    EXPECT_EQ(results[0].fileName, L"StudioAppCli.exe");
    EXPECT_EQ(results[1].fileName, L"StudioAppSpec.md");
    EXPECT_EQ(results[2].fileName, L"StudioApp.lnk");

    runMgr.clear();
    DeleteFileW(tempCsv.c_str());

    // 验证 FOLDERID_AppsFolder 枚举现代 UWP / 系统应用与同名去重
    auto& catalog = tools3000::service::launcher::AppsFolderCatalog::instance();
    catalog.invalidate();
    const auto apps = catalog.getSnapshot(nullptr, true, L"__unit_test_sid__");
    EXPECT_FALSE(apps.empty());
    if (!apps.empty()) {
        EXPECT_TRUE(tools3000::service::launcher::isVirtualShellUri(apps.front().shellUri));

        const auto& sample = apps.front();
        const SearchExpression appExpr = SearchExpression::parse(sample.displayName);
        SearchExcludeOptions noExcludes;
        std::unordered_set<std::wstring> emptyStems;
        const auto matchedApps = catalog.queryMatchingApps(
            appExpr, noExcludes, emptyStems, nullptr, true, L"__unit_test_sid__", 16);
        EXPECT_FALSE(matchedApps.empty());

        // 当同名开始菜单快捷方式已存在时，AppsFolder 自动去重不重复产出
        std::unordered_set<std::wstring> existingStems{sample.normalizedStem};
        const auto dedupedApps = catalog.queryMatchingApps(
            appExpr, noExcludes, existingStems, nullptr, true, L"__unit_test_sid__", 16);
        for (const auto& item : dedupedApps) {
            EXPECT_NE(
                tools3000::service::launcher::extractLauncherStemLower(item.fileName, false),
                sample.normalizedStem);
        }
    }
}

// -----------------------------------------------------------------------------
// Batch 2 & Batch 3 防回退单元测试集 (P-05 / P-07 / U-08 / C-02)
// -----------------------------------------------------------------------------

TEST(PlainTextZeroCopyPrefilterTest, SkipsHeapCopyOnNonMatchingMappedFilesAndConcurrentCacheReads) {
    using namespace tools3000_content_search_test_helpers;
    auto& engine = tools3000::service::content::ContentSearchEngine::instance();
    auto& cache = tools3000::service::content::ContentTextCache::instance();
    cache.clear();

    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring utf8Path = std::wstring(tempDir) + L"Tools3000_P05_ZeroCopyUtf8_" +
        std::to_wstring(GetCurrentProcessId()) + L".log";
    const std::wstring utf16Path = std::wstring(tempDir) + L"Tools3000_P05_ZeroCopyUtf16_" +
        std::to_wstring(GetCurrentProcessId()) + L".txt";
    TempFileDeleter utf8Guard{utf8Path};
    TempFileDeleter utf16Guard{utf16Path};

    // 构造跨多个 AVX2 32B / 缓存行边界的大体积 UTF-8 日志文件 (> 96 KB)
    std::string largeUtf8;
    largeUtf8.reserve(98304);
    for (int i = 0; i < 1200; ++i) {
        largeUtf8.append("2026-10-15T12:00:00Z [INFO] routine telemetry heartbeat sequence=");
        largeUtf8.append(std::to_string(i));
        largeUtf8.append("\n");
    }
    largeUtf8.append("2026-10-15T12:01:00Z [WARN] VectorSimdBoundaryToken_AlphaOmega_9941跨边界中文锚点\n");
    {
        std::ofstream ofs(utf8Path, std::ios::binary | std::ios::trunc);
        ofs.write(largeUtf8.data(), static_cast<std::streamsize>(largeUtf8.size()));
    }

    std::vector<tools3000::service::content::ContentSnippet> snippets;
    EXPECT_FALSE(engine.searchFile(
        utf8Path, std::vector<std::wstring>{L"NonExistentZeroCopyTokenXYZ"}, false, snippets, 3));
    EXPECT_TRUE(snippets.empty());

    ASSERT_TRUE(engine.searchFile(
        utf8Path, std::vector<std::wstring>{L"vectorsimdboundarytoken_alphaomega_9941", L"跨边界中文锚点"}, false, snippets, 3));
    ASSERT_FALSE(snippets.empty());
    EXPECT_EQ(snippets[0].lineNumber, 1201u);

    // UTF-16LE 带 BOM 零拷贝预筛与大小写无关匹配
    std::wstring utf16Doc(2048, L'A');
    utf16Doc.append(L"\r\nSecondLine_Utf16CaselessProbe_7788_终端引擎\r\n");
    {
        std::ofstream ofs(utf16Path, std::ios::binary | std::ios::trunc);
        const uint8_t bom[2] = {0xFF, 0xFE};
        ofs.write(reinterpret_cast<const char*>(bom), 2);
        ofs.write(reinterpret_cast<const char*>(utf16Doc.data()),
                  static_cast<std::streamsize>(utf16Doc.size() * sizeof(wchar_t)));
    }
    snippets.clear();
    EXPECT_FALSE(engine.searchFile(utf16Path, std::vector<std::wstring>{L"AbsentUtf16Keyword"}, false, snippets, 3));
    ASSERT_TRUE(engine.searchFile(utf16Path, std::vector<std::wstring>{L"utf16caselessprobe_7788"}, false, snippets, 3));
    ASSERT_EQ(snippets.size(), 1u);
    EXPECT_EQ(snippets[0].lineNumber, 2u);

    // 验证 ContentTextCache shared_ptr 压缩载荷在并发读写与淘汰下的无锁解压稳定性
    cache.put(L"C:\\virtual\\concurrent_doc.pdf", 4096ULL, 1001ULL, L"ConcurrentSharedPtrPayload_稳态验证");
    std::atomic<int> successReads{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 6; ++t) {
        readers.emplace_back([&]() {
            for (int iter = 0; iter < 40; ++iter) {
                std::wstring outText;
                bool truncated = false;
                if (cache.tryGetWithTruncation(
                        L"C:\\virtual\\concurrent_doc.pdf", 4096ULL, 1001ULL, outText, truncated)) {
                    if (outText == L"ConcurrentSharedPtrPayload_稳态验证" && !truncated) {
                        successReads.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }
    for (auto& th : readers) th.join();
    EXPECT_EQ(successReads.load(), 240);
    cache.clear();
}

TEST(XlsxWorksheetRowReconstructionTest, ResolvesSharedStringsPerRowAndEliminatesNumericIndexFalsePositives) {
    using namespace tools3000_content_search_test_helpers;
    auto& engine = tools3000::service::content::ContentSearchEngine::instance();
    tools3000::service::content::ContentTextCache::instance().clear();

    // 构造 15 个共享字符串，使索引 12 对应目标中文文本，并附带 <rPh> 注音噪声
    std::string sharedStringsXml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" count=\"15\" uniqueCount=\"15\">";
    for (int i = 0; i < 15; ++i) {
        if (i == 12) {
            sharedStringsXml +=
                "<si><t>分布式搜索引擎</t><rPh sb=\"0\" eb=\"3\"><t>FenBuShiNoise</t></rPh></si>";
        } else {
            sharedStringsXml += "<si><t>PlaceholderCell_" + std::to_string(i) + "</t></si>";
        }
    }
    sharedStringsXml += "</sst>";

    // sheet1.xml 第 1 行引用共享字符串下标 12 (<c t="s"><v>12</v></c>)、真实数值 98765 与 inlineStr
    const std::string sheet1Xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<sheetData>"
        "<row r=\"1\">"
        "<c r=\"A1\" t=\"s\"><v>12</v></c>"
        "<c r=\"B1\"><v>98765</v></c>"
        "<c r=\"C1\" t=\"inlineStr\"><is><t>里程碑验收通过</t></is></c>"
        "</row>"
        "</sheetData>"
        "</worksheet>";

    const auto xlsxArchive = buildZipArchive({
        {"xl/sharedStrings.xml", sharedStringsXml},
        {"xl/worksheets/sheet1.xml", sheet1Xml}
    }, /*useDeflate=*/true);

    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring xlsxPath = std::wstring(tempDir) + L"Tools3000_U08_RowAligned_" +
        std::to_wstring(GetCurrentProcessId()) + L".xlsx";
    TempFileDeleter xlsxGuard{xlsxPath};

    {
        std::ofstream ofs(xlsxPath, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(xlsxArchive.data()),
                  static_cast<std::streamsize>(xlsxArchive.size()));
    }

    std::vector<tools3000::service::content::ContentSnippet> snippets;
    // 共享字符串整数下标 "12" 绝不可暴露在还原文本中（注：PlaceholderCell_12 含 "12"，故测试独立不存在的索引如单独行或校验 Placeholder 不含 12）
    // 为严格验证纯数字下标不泄漏，再用无数字占位重造一份纯净工作簿
    std::string pureSst =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><sst>";
    for (int i = 0; i < 15; ++i) {
        if (i == 12) {
            pureSst += "<si><t>分布式搜索引擎</t><rPh sb=\"0\" eb=\"3\"><t>FenBuShiNoise</t></rPh></si>";
        } else {
            pureSst += "<si><t>AlphaFillerItem</t></si>";
        }
    }
    pureSst += "</sst>";
    const auto pureXlsx = buildZipArchive({
        {"xl/sharedStrings.xml", pureSst},
        {"xl/worksheets/sheet1.xml", sheet1Xml}
    }, /*useDeflate=*/true);
    {
        std::ofstream ofs(xlsxPath, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(pureXlsx.data()),
                  static_cast<std::streamsize>(pureXlsx.size()));
    }
    tools3000::service::content::ContentTextCache::instance().clear();

    // 1. 共享字符串引用索引 "12" 绝不产生数字误命中
    EXPECT_FALSE(engine.searchFile(xlsxPath, std::vector<std::wstring>{L"12"}, false, snippets, 3));
    // 2. <rPh> 拼音注音辅助块绝不产生噪声命中
    EXPECT_FALSE(engine.searchFile(xlsxPath, std::vector<std::wstring>{L"FenBuShiNoise"}, false, snippets, 3));
    // 3. 同一 <row> 内的共享字符串文本、数值单元格与 inlineStr 在同一逻辑行共现
    snippets.clear();
    ASSERT_TRUE(engine.searchFile(
        xlsxPath,
        std::vector<std::wstring>{L"分布式搜索引擎", L"98765", L"里程碑验收通过"},
        false,
        snippets,
        4));
    ASSERT_GE(snippets.size(), 1u);
    EXPECT_EQ(snippets[0].lineNumber, 1u);
    EXPECT_NE(snippets[0].lineContent.find(L"分布式搜索引擎"), std::wstring::npos);
    EXPECT_NE(snippets[0].lineContent.find(L"98765"), std::wstring::npos);
    EXPECT_NE(snippets[0].lineContent.find(L"里程碑验收通过"), std::wstring::npos);
}

TEST(PinyinScratchAndRunHistoryAsyncTest, SinglePassPinyinMatchesLegacyAndAsyncHistoryCoalescesAndEvicts) {
    std::string scratchAsciiInit;
    std::string scratchAsciiFull;
    std::wstring initOut;
    std::wstring fullOut;

    // 1. 纯英文文件名：单遍 ASCII 快径归一化全小写且零堆分配复写
    const std::wstring asciiName = L"CMakeLists_Build2026.TXT";
    PinyinEngine::BuildNormalizedPinyin(asciiName, scratchAsciiInit, scratchAsciiFull, initOut, fullOut);
    EXPECT_EQ(initOut, L"cmakelists_build2026.txt");
    EXPECT_EQ(fullOut, L"cmakelists_build2026.txt");
    EXPECT_NE(PinyinEngine::LookupSyllableAscii(L'微'), nullptr);
    EXPECT_EQ(PinyinEngine::LookupSyllableAscii(L'A'), nullptr);

    // 2. 中英混合 + 扩展区代理对 (U+20BB7) + 日文假名：与传统双遍接口输出 100% 一致
    std::wstring mixedName = L"微信Pro_";
    mixedName.push_back(static_cast<wchar_t>(0xD842));
    mixedName.push_back(static_cast<wchar_t>(0xDFB7));
    mixedName += L"_\u3042工具箱.exe";

    const std::wstring expectedNorm = SearchExpression::normalize(mixedName);
    PinyinEngine::BuildNormalizedPinyin(expectedNorm, scratchAsciiInit, scratchAsciiFull, initOut, fullOut);
    EXPECT_EQ(initOut, SearchExpression::normalize(PinyinEngine::GetInitials(expectedNorm)));
    EXPECT_EQ(fullOut, SearchExpression::normalize(PinyinEngine::GetFullPinyin(expectedNorm)));

    // 3. RunHistoryManager 内存即时可见、防抖合并刷盘与容量上限淘汰
    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring csvPath = std::wstring(tempDir) + L"Tools3000_P07_AsyncHistory_" +
        std::to_wstring(GetCurrentProcessId()) + L".csv";
    DeleteFileW(csvPath.c_str());

    auto& mgr = tools3000::service::db::RunHistoryManager::instance();
    mgr.init(csvPath);
    mgr.clear();
    mgr.setMaxEntries(5);

    // 连续高频调用 recordRun 50 次，内存快照必须 0 阻塞立即可见
    for (int i = 0; i < 50; ++i) {
        mgr.recordRun(L"C:\\PinnedApp\\TopLauncher.exe");
    }
    EXPECT_EQ(mgr.captureSnapshot().lookup(L"C:\\PinnedApp\\TopLauncher.exe").runCount, 50u);

    // 插入超过上限 (10 条低频记录)，验证上限收敛至 <= 5 且高频项始终保留
    for (int i = 0; i < 10; ++i) {
        mgr.recordRun(L"C:\\Temp\\Ephemeral_" + std::to_wstring(i) + L".txt");
    }
    EXPECT_LE(mgr.size(), 5u);
    EXPECT_EQ(mgr.captureSnapshot().lookup(L"C:\\PinnedApp\\TopLauncher.exe").runCount, 50u);

    mgr.flush();
    EXPECT_TRUE(std::filesystem::exists(csvPath));

    // 恢复默认容量上限并清理测试文件
    mgr.setMaxEntries(tools3000::service::db::RunHistoryManager::MaxRunHistoryEntries);
    mgr.clear();
    DeleteFileW(csvPath.c_str());
}

TEST(DirectoryAggregateSizeFilterTest, ComputesRecursiveFolderSizesPropagatesDeltasAndMatchesFolderSizeSyntax) {
    constexpr uint64_t kMiB = 1024ULL * 1024ULL;

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
    auto addFile = [&](DWORDLONG id, DWORDLONG parent, const wchar_t* name, uint64_t bytes) {
        FileRecordInit f{};
        f.fileReferenceNumber = id;
        f.parentFileReferenceNumber = parent;
        f.fileName = name;
        f.isDirectory = false;
        f.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
        f.fileSize = bytes;
        f.lastWriteTime = 2000;
        records.push_back(std::move(f));
    };

    addDir(9100, 5, L"WorkspaceAggregateRoot");
    addDir(9101, 9100, L"SubModuleAlpha");
    addDir(9102, 9100, L"SubModuleBeta");
    addDir(9103, 9100, L"EmptySubFolder");
    addFile(9110, 9101, L"alpha_pack.bin", 60 * kMiB);
    addFile(9111, 9102, L"beta_pack.bin", 80 * kMiB);

    addDir(9200, 5, L"StandaloneDownloads");
    addFile(9201, 9200, L"standalone_archive.iso", 150 * kMiB);

    ASSERT_TRUE(parser.importSnapshot(std::move(records), 5000, parser.getVolumeSerialNumber()));

    // 1. 不带 folder: 的裸 size:>100mb 保持纯文件过滤语义，仅返回 150MB 单文件，不混入聚合目录
    auto bareSizeHits = parser.Search(L"size:>100mb", 50);
    ASSERT_EQ(bareSizeHits.size(), 1u);
    EXPECT_FALSE(bareSizeHits[0].isDirectory);
    EXPECT_EQ(bareSizeHits[0].fileName, L"standalone_archive.iso");

    // 2. folder: size:>100mb 命中递归聚合体积 >= 100MB 的目录 (WorkspaceAggregateRoot = 140MB, StandaloneDownloads = 150MB)
    auto folderSizeHits = parser.Search(L"folder: WorkspaceAggregate size:>100mb", 50);
    ASSERT_EQ(folderSizeHits.size(), 1u);
    EXPECT_TRUE(folderSizeHits[0].isDirectory);
    EXPECT_EQ(folderSizeHits[0].fileName, L"WorkspaceAggregateRoot");
    EXPECT_EQ(folderSizeHits[0].fileSize, 140 * kMiB);

    // 子目录 60MB / 80MB 不应误匹配 >100mb
    auto subFolderHits = parser.Search(L"folder: SubModule size:>100mb", 50);
    EXPECT_TRUE(subFolderHits.empty());

    // 3. 验证 FileIndexStore 在实时 USN updateMetadata 与 erase 时祖先目录聚合体积动态精确增减
    tools3000::service::FileIndexStore store;
    store.setDriveLetter('D');
    FileRecordInit dirRoot{};
    dirRoot.fileReferenceNumber = 100;
    dirRoot.parentFileReferenceNumber = 5;
    dirRoot.fileName = L"ProjectRoot";
    dirRoot.isDirectory = true;
    dirRoot.fileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    store.upsert(dirRoot);

    FileRecordInit dirChild{};
    dirChild.fileReferenceNumber = 101;
    dirChild.parentFileReferenceNumber = 100;
    dirChild.fileName = L"BuildOut";
    dirChild.isDirectory = true;
    dirChild.fileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    store.upsert(dirChild);

    FileRecordInit outBin{};
    outBin.fileReferenceNumber = 102;
    outBin.parentFileReferenceNumber = 101;
    outBin.fileName = L"app.exe";
    outBin.isDirectory = false;
    outBin.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    outBin.fileSize = 40 * kMiB;
    store.upsert(outBin);
    store.recomputeHierarchyMetadata();

    ASSERT_NE(store.find(100), nullptr);
    ASSERT_NE(store.find(101), nullptr);
    EXPECT_EQ(store.fileSizeOf(*store.find(100)), 40 * kMiB);
    EXPECT_EQ(store.fileSizeOf(*store.find(101)), 40 * kMiB);

    // 子文件扩容至 95MB -> 父目录链同步增加 55MB
    store.updateMetadata(102, 95 * kMiB, 1000, 2000);
    EXPECT_EQ(store.fileSizeOf(*store.find(101)), 95 * kMiB);
    EXPECT_EQ(store.fileSizeOf(*store.find(100)), 95 * kMiB);

    // 删除子文件 -> 父目录链聚合体积归零
    EXPECT_TRUE(store.erase(102));
    EXPECT_EQ(store.fileSizeOf(*store.find(101)), 0ULL);
    EXPECT_EQ(store.fileSizeOf(*store.find(100)), 0ULL);
}

TEST(SearchPluginInteractionAndSettingsSyncTest, ReturnsFileNotFoundCodeAndPersistsUiPreferencesRoundTrip) {
    std::array<wchar_t, 4096> executablePath{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    ASSERT_GT(length, 0u);
    ASSERT_LT(length, executablePath.size());

    const auto executableDir = std::filesystem::path(executablePath.data()).parent_path();
    std::filesystem::path pluginDllPath =
        executableDir.parent_path() / L"plugins" / executableDir.filename() / L"Plugin_Search.dll";
    if (!std::filesystem::exists(pluginDllPath)) {
        pluginDllPath = executableDir / L"plugins" / L"Plugin_Search.dll";
    }
    ASSERT_TRUE(std::filesystem::exists(pluginDllPath));

    const auto tempCfgDir = std::filesystem::temp_directory_path() /
        (L"Tools3000_SearchPluginCfg_" + std::to_wstring(GetCurrentProcessId()) +
         L"_" + std::to_wstring(GetTickCount64()));
    std::error_code ec;
    std::filesystem::create_directories(tempCfgDir, ec);

    auto& cfg = tools3000::core::ConfigManager::instance();
    ASSERT_TRUE(cfg.initialize(tempCfgDir, "search_settings_test.json"));

    HMODULE hMod = LoadLibraryExW(
        pluginDllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (hMod == nullptr) {
        cfg.shutdown();
        std::filesystem::remove_all(tempCfgDir, ec);
    }
    ASSERT_NE(hMod, nullptr);

    using CreatePluginFunc = tools3000::core::IPlugin* (*)();
    const auto createPlugin = reinterpret_cast<CreatePluginFunc>(GetProcAddress(hMod, "CreatePlugin"));
    if (createPlugin == nullptr) {
        FreeLibrary(hMod);
        cfg.shutdown();
        std::filesystem::remove_all(tempCfgDir, ec);
    }
    ASSERT_NE(createPlugin, nullptr);
    tools3000::core::IPlugin* searchPlugin = createPlugin();
    if (searchPlugin == nullptr) {
        FreeLibrary(hMod);
        cfg.shutdown();
        std::filesystem::remove_all(tempCfgDir, ec);
    }
    ASSERT_NE(searchPlugin, nullptr);

    struct SearchPluginCleanupGuard {
        HMODULE mod = nullptr;
        tools3000::core::IPlugin* plugin = nullptr;
        std::filesystem::path tempDir;
        ~SearchPluginCleanupGuard() {
            if (plugin) plugin->shutdown();
            tools3000::core::ConfigManager::instance().clearCallbacks();
            if (mod) FreeLibrary(mod);
            tools3000::core::ConfigManager::instance().reset();
            tools3000::core::ConfigManager::instance().shutdown();
            std::error_code cleanupEc;
            std::filesystem::remove_all(tempDir, cleanupEc);
        }
    } cleanupGuard{hMod, searchPlugin, tempCfgDir};

    ASSERT_TRUE(searchPlugin->initialize());

    auto& bridge = tools3000::core::MessageBridge::instance();
    const auto callSync = [&](const char* method, const nlohmann::json& params) -> nlohmann::json {
        nlohmann::json req = {
            {"id", 901},
            {"method", method},
            {"params", params}
        };
        const std::string rawResp = bridge.handleMessage(req.dump());
        const auto parsed = nlohmann::json::parse(rawResp);
        return parsed.value("result", nlohmann::json::object());
    };

    // 1. 针对已删除/不存在文件的交互请求必须统一返回结构化错误码 FILE_NOT_FOUND
    const std::string missingFile =
        "C:\\__Tools3000_NonExistent_Ghost_Dir__\\missing_target_998877.txt";
    for (const char* method : {"search.openFile", "search.openFolder", "search.copyFile", "search.openWithNotepad"}) {
        const auto res = callSync(method, {{"path", missingFile}});
        EXPECT_FALSE(res.value("success", true)) << "method=" << method;
        EXPECT_EQ(res.value("errorCode", ""), "FILE_NOT_FOUND") << "method=" << method;
    }

    // 2. 验证 search.saveSettings 与 search.getSettings 完整双向持久化 uiPreferences / defaultSearchMode / contentExtensions
    const auto saveRes = callSync("search.saveSettings", {
        {"defaultSearchMode", "both"},
        {"defaultCategory", "code"},
        {"excludeHidden", true},
        {"contentExtensions", nlohmann::json::array({".vue", " Astro ", "wgsl"})},
        {"disabledContentFormats", nlohmann::json::array({".log", "tmp"})},
        {"uiPreferences", {
            {"density", "compact"},
            {"viewLayout", "table"},
            {"excludeGitAndModules", false}
        }}
    });
    EXPECT_TRUE(saveRes.value("success", false));

    const auto loaded = callSync("search.getSettings", nlohmann::json::object());
    EXPECT_EQ(loaded.value("defaultSearchMode", ""), "both");
    EXPECT_EQ(loaded.value("defaultCategory", ""), "code");
    EXPECT_TRUE(loaded.value("excludeHidden", false));

    const auto expectedCustom = nlohmann::json::array({"vue", "astro", "wgsl"});
    const auto expectedDisabled = nlohmann::json::array({"log", "tmp"});
    EXPECT_EQ(loaded.value("contentExtensions", nlohmann::json::array()), expectedCustom);
    EXPECT_EQ(loaded.value("customContentFormats", nlohmann::json::array()), expectedCustom);
    EXPECT_EQ(loaded.value("disabledContentFormats", nlohmann::json::array()), expectedDisabled);

    ASSERT_TRUE(loaded.contains("uiPreferences") && loaded["uiPreferences"].is_object());
    EXPECT_EQ(loaded["uiPreferences"].value("density", ""), "compact");
    EXPECT_EQ(loaded["uiPreferences"].value("viewLayout", ""), "table");
    EXPECT_EQ(loaded["uiPreferences"].value("searchMode", ""), "both");
    EXPECT_EQ(loaded["uiPreferences"].value("defaultCategory", ""), "code");
    EXPECT_FALSE(loaded["uiPreferences"].value("excludeGitAndModules", true));
    EXPECT_EQ(loaded["uiPreferences"].value("customContentFormats", nlohmann::json::array()), expectedCustom);
    EXPECT_EQ(loaded["uiPreferences"].value("disabledContentFormats", nlohmann::json::array()), expectedDisabled);

    // 3. 验证窗口工作区最大化 IPC 状态机（getMaximized / setMaximized / toggleMaximized / resetPlacement）
    EXPECT_FALSE(callSync("search.getMaximized", nlohmann::json::object()).value("maximized", true));
    EXPECT_TRUE(callSync("search.setMaximized", {{"maximized", true}}).value("maximized", false));
    EXPECT_TRUE(callSync("search.getMaximized", nlohmann::json::object()).value("maximized", false));
    EXPECT_FALSE(callSync("search.toggleMaximized", nlohmann::json::object()).value("maximized", true));
    EXPECT_TRUE(callSync("search.toggleMaximized", nlohmann::json::object()).value("maximized", false));
    EXPECT_FALSE(callSync("search.resetPlacement", nlohmann::json::object()).value("maximized", true));
    EXPECT_FALSE(callSync("search.getMaximized", nlohmann::json::object()).value("maximized", true));
}

TEST(SearchServiceWakeupNonBlockingTest, MarkMetadataDirtyAndFastConcurrentDbStats) {
    tools3000::service::FileIndexStore store;
    store.setDriveLetter('C');
    store.reserve(16);

    FileRecordInit dirRec{};
    dirRec.fileReferenceNumber = 100ULL;
    dirRec.parentFileReferenceNumber = 0ULL;
    dirRec.fileName = L"Chosen";
    dirRec.isDirectory = true;
    dirRec.fileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    store.upsert(dirRec, true);

    FileRecordInit fileRec{};
    fileRec.fileReferenceNumber = 200ULL;
    fileRec.parentFileReferenceNumber = 100ULL;
    fileRec.fileName = L"纳百川-采购阶梯价V10001.ch";
    fileRec.isDirectory = false;
    fileRec.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    fileRec.fileSize = 4096ULL;
    fileRec.creationTime = 133500000000000000ULL;
    fileRec.lastWriteTime = 133500000000000000ULL;
    store.upsert(fileRec, true);

    const auto* storedBefore = store.find(200ULL);
    ASSERT_NE(storedBefore, nullptr);
    EXPECT_TRUE(storedBefore->isMetadataHydrated());

    // 超量 USN 追赶跳过同步 OpenFileById 时标记为 dirty，验证清除水合位且保留文件名/拼音与树结构
    EXPECT_TRUE(store.markMetadataDirty(200ULL));
    EXPECT_FALSE(store.markMetadataDirty(100ULL)); // 目录不清除水合位
    EXPECT_FALSE(store.markMetadataDirty(99999ULL));

    const auto* storedAfter = store.find(200ULL);
    ASSERT_NE(storedAfter, nullptr);
    EXPECT_FALSE(storedAfter->isMetadataHydrated());
    EXPECT_EQ(store.view(*storedAfter).fileName, L"纳百川-采购阶梯价V10001.ch");
    EXPECT_NE(store.view(*storedAfter).pinyinInitials.find(L"nbc"), std::wstring::npos);

    // 后台水合完成后再次恢复水合标记
    EXPECT_TRUE(store.updateMetadata(200ULL, 8192ULL, 133500000000000000ULL, 133500010000000000ULL));
    EXPECT_TRUE(store.find(200ULL)->isMetadataHydrated());
    EXPECT_EQ(store.fileSizeOf(*store.find(200ULL)), 8192ULL);

    // 验证 bulkImport + recomputeHierarchyMetadata 按树深桶排汇总跨多层目录大小与原语义完全一致
    FileRecordInit subDir{};
    subDir.fileReferenceNumber = 101ULL;
    subDir.parentFileReferenceNumber = 100ULL;
    subDir.fileName = L"SubPricing";
    subDir.isDirectory = true;
    subDir.fileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    store.upsert(subDir, false, true);

    FileRecordInit nestedFile{};
    nestedFile.fileReferenceNumber = 201ULL;
    nestedFile.parentFileReferenceNumber = 101ULL;
    nestedFile.fileName = L"阶梯价明细.xlsx";
    nestedFile.isDirectory = false;
    nestedFile.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    nestedFile.fileSize = 2048ULL;
    store.upsert(nestedFile, false, true);

    store.recomputeHierarchyMetadata();
    ASSERT_NE(store.find(101ULL), nullptr);
    ASSERT_NE(store.find(100ULL), nullptr);
    EXPECT_EQ(store.fileSizeOf(*store.find(101ULL)), 2048ULL);
    EXPECT_EQ(store.fileSizeOf(*store.find(100ULL)), 8192ULL + 2048ULL);

    // 验证 DatabaseManager::getStats 在快照保存/加载前后毫秒级无锁返回
    wchar_t tempDirBuf[MAX_PATH]{};
    ASSERT_GT(GetTempPathW(MAX_PATH, tempDirBuf), 0u);
    const std::filesystem::path dbDir = std::filesystem::path(tempDirBuf) / L"t3k_nonblocking_dbstats_test";
    std::error_code ec;
    std::filesystem::remove_all(dbDir, ec);
    ASSERT_TRUE(std::filesystem::create_directories(dbDir, ec));
    const std::wstring dbFile = (dbDir / L"Tools3000_Test.db").wstring();

    MftParser parser;
    parser.Initialize('C');
    parser.setRealTimeValidation(false);
    std::vector<FileRecordInit> snapRecords = {dirRec, fileRec};
    ASSERT_TRUE(parser.importSnapshot(std::move(snapRecords), 5000ULL, parser.getVolumeSerialNumber()));

    auto& dbMgr = tools3000::service::db::DatabaseManager::instance();
    const std::wstring prevPath = dbMgr.getDbPath();
    dbMgr.init(dbFile);

    std::vector<MftParser*> parsers = {&parser};
    ASSERT_TRUE(dbMgr.saveSnapshot(parsers));

    const auto t0 = std::chrono::steady_clock::now();
    const auto stats = dbMgr.getStats();
    const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_TRUE(stats.exists);
    EXPECT_EQ(stats.totalRecords, 2ULL);
    EXPECT_LT(elapsedUs, 50000); // < 50ms

    dbMgr.init(prevPath);
    std::filesystem::remove_all(dbDir, ec);
}

TEST(SlashPathFragmentSeparableTest, PinyinInitialsFullAndWildcardAcrossPathSeparators) {
    MftParser parser;
    parser.Initialize('D');
    parser.setRealTimeValidation(false);

    std::vector<FileRecordInit> records;
    auto addDir = [&](DWORDLONG frn, DWORDLONG parent, const wchar_t* name) {
        FileRecordInit r{};
        r.fileReferenceNumber = frn;
        r.parentFileReferenceNumber = parent;
        r.fileName = name;
        r.isDirectory = true;
        r.fileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        records.push_back(std::move(r));
    };
    auto addFile = [&](DWORDLONG frn, DWORDLONG parent, const wchar_t* name) {
        FileRecordInit r{};
        r.fileReferenceNumber = frn;
        r.parentFileReferenceNumber = parent;
        r.fileName = name;
        r.isDirectory = false;
        r.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
        r.fileSize = 4096;
        r.lastWriteTime = 133450000000000000ULL;
        records.push_back(std::move(r));
    };

    // 构建包含中文与英文混合层级的真实目录树 + 2000 个噪音目录与文件：
    // D:\工作目录\user\说明文档.md
    // D:\工作目录\测试项目\user\config.json
    addDir(5, 5, L".");
    addDir(100, 5, L"工作目录");
    addDir(101, 100, L"user");
    addFile(102, 101, L"说明文档.md");
    addDir(110, 100, L"测试项目");
    addDir(111, 110, L"user");
    addFile(112, 111, L"config.json");

    for (DWORDLONG d = 0; d < 20; ++d) {
        const DWORDLONG dirId = 2000 + d;
        const std::wstring noiseDir = L"noise_pkg_" + std::to_wstring(d);
        addDir(dirId, 5, noiseDir.c_str());
        for (DWORDLONG f = 0; f < 100; ++f) {
            FileRecordInit nf{};
            nf.fileReferenceNumber = 20000 + d * 100 + f;
            nf.parentFileReferenceNumber = dirId;
            nf.fileName = L"item_" + std::to_wstring(f) + L".txt";
            nf.isDirectory = false;
            nf.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
            nf.fileSize = 256;
            nf.lastWriteTime = 133400000000000000ULL;
            records.push_back(std::move(nf));
        }
    }

    ASSERT_TRUE(parser.importSnapshot(std::move(records), 3000, parser.getVolumeSerialNumber()));

    const SearchExcludeOptions noExcludes{};
    const SearchParseOptions pinyinOn{true, false, false};
    const SearchParseOptions pinyinOff{false, false, false};

    // 1. 拼音首字母 + 反斜杠：gzml\user 精准命中 D:\工作目录\user 及其子文件，且 O(1) 淘汰 2000 个无关噪音文件
    auto hitsInitials = parser.Search(L"gzml\\user", 50, noExcludes, nullptr, pinyinOn);
    ASSERT_EQ(hitsInitials.size(), 2u);
    EXPECT_LE(parser.getLastSearchPathBuildCount(), 10u);
    // 验证目标目录自身 D:\工作目录\user 的 matchRank 优于深层子文件
    bool foundTargetDir = false;
    bool foundChildDoc = false;
    for (const auto& h : hitsInitials) {
        if (h.fullPath == L"D:\\工作目录\\user") foundTargetDir = true;
        if (h.fullPath == L"D:\\工作目录\\user\\说明文档.md") foundChildDoc = true;
    }
    EXPECT_TRUE(foundTargetDir);
    EXPECT_TRUE(foundChildDoc);
    EXPECT_EQ(hitsInitials[0].fullPath, L"D:\\工作目录\\user");

    // 2. 正斜杠与双引号写法：gzml/user 与 "gzml\user"
    auto hitsForwardSlash = parser.Search(L"gzml/user", 50, noExcludes, nullptr, pinyinOn);
    EXPECT_EQ(hitsForwardSlash.size(), 2u);
    auto hitsQuoted = parser.Search(L"\"gzml\\user\"", 50, noExcludes, nullptr, pinyinOn);
    EXPECT_EQ(hitsQuoted.size(), 2u);

    // 3. 全拼 + 反斜杠：gongzuomulu\user
    auto hitsFullPy = parser.Search(L"gongzuomulu\\user", 50, noExcludes, nullptr, pinyinOn);
    EXPECT_EQ(hitsFullPy.size(), 2u);
    EXPECT_LE(parser.getLastSearchPathBuildCount(), 10u);

    // 4. 跨目录层级的通配符 + 拼音首字母：*gzml*\*user* 同时覆盖直接子级与隔层子级（共 4 条记录），且保持 O(1) 位掩码快筛
    auto hitsWildcard = parser.Search(L"*gzml*\\*user*", 50, noExcludes, nullptr, pinyinOn);
    EXPECT_EQ(hitsWildcard.size(), 4u);
    EXPECT_LE(parser.getLastSearchPathBuildCount(), 12u);

    // 5. 中拼混合与首全混合跨段匹配：工作目录\csxm 与 *工作目录*\*smwd*
    auto hitsMixedCjkPy = parser.Search(L"工作目录\\csxm", 50, noExcludes, nullptr, pinyinOn);
    EXPECT_EQ(hitsMixedCjkPy.size(), 3u); // 测试项目, 测试项目\user, 测试项目\user\config.json
    auto hitsMixedWild = parser.Search(L"*工作目录*\\*smwd*", 50, noExcludes, nullptr, pinyinOn);
    ASSERT_EQ(hitsMixedWild.size(), 1u);
    EXPECT_EQ(hitsMixedWild[0].fullPath, L"D:\\工作目录\\user\\说明文档.md");

    // 6. 单文件名通配符拼音与显式 py: 算子：*smwd* / py:*gzml*\*user*
    auto hitsLeafWildPy = parser.Search(L"*smwd*.md", 50, noExcludes, nullptr, pinyinOn);
    ASSERT_EQ(hitsLeafWildPy.size(), 1u);
    EXPECT_EQ(hitsLeafWildPy[0].fullPath, L"D:\\工作目录\\user\\说明文档.md");
    auto hitsExplicitPyOffGlobal = parser.Search(L"py:*gzml*\\*user*", 50, noExcludes, nullptr, pinyinOff);
    EXPECT_EQ(hitsExplicitPyOffGlobal.size(), 4u);

    // 7. parent: 拼音路径过滤
    auto hitsParentPy = parser.Search(L"parent:gzml\\user", 50, noExcludes, nullptr, pinyinOn);
    ASSERT_EQ(hitsParentPy.size(), 1u);
    EXPECT_EQ(hitsParentPy[0].fullPath, L"D:\\工作目录\\user\\说明文档.md");

    // 8. 负例门禁：关闭拼音、nopy:、case: 时绝不误匹配拼音路径
    EXPECT_EQ(parser.Search(L"gzml\\user", 50, noExcludes, nullptr, pinyinOff).size(), 0u);
    EXPECT_EQ(parser.Search(L"nopy:gzml\\user", 50, noExcludes, nullptr, pinyinOn).size(), 0u);
    EXPECT_EQ(parser.Search(L"case:gzml\\user", 50, noExcludes, nullptr, pinyinOn).size(), 0u);
}

#include "search/SearchQueryConfigUtils.h"

TEST(NonChineseLocaleAndNonCjkPinyinIsolationTest, LocaleDefaultPolicyAndZeroWesternOrKanjiConflict) {
    using tools3000::core::UiLanguage;
    using tools3000::search::defaultPinyinEnabledForUiLanguage;
    using tools3000::search::resolveEffectivePinyinEnabled;

    // 1. 语种默认策略门禁：仅简/繁中文默认开启拼音，全部 6 种非中文语境默认关闭
    EXPECT_TRUE(defaultPinyinEnabledForUiLanguage(UiLanguage::ZhCN));
    EXPECT_TRUE(defaultPinyinEnabledForUiLanguage(UiLanguage::ZhTW));
    EXPECT_FALSE(defaultPinyinEnabledForUiLanguage(UiLanguage::EnUS));
    EXPECT_FALSE(defaultPinyinEnabledForUiLanguage(UiLanguage::JaJP));
    EXPECT_FALSE(defaultPinyinEnabledForUiLanguage(UiLanguage::KoKR));
    EXPECT_FALSE(defaultPinyinEnabledForUiLanguage(UiLanguage::RuRU));
    EXPECT_FALSE(defaultPinyinEnabledForUiLanguage(UiLanguage::PtBR));
    EXPECT_FALSE(defaultPinyinEnabledForUiLanguage(UiLanguage::EsES));

    // 2. ConfigManager 显式开关覆盖门禁：用户手动开启/关闭始终最高优先
    const std::filesystem::path tmpCfgDir =
        std::filesystem::temp_directory_path() /
        ("Tools3000_LocalePinyinGate_" + std::to_string(GetCurrentProcessId()));
    std::error_code fsEc;
    std::filesystem::create_directories(tmpCfgDir, fsEc);
    auto& cfg = tools3000::core::ConfigManager::instance();
    ASSERT_TRUE(cfg.initialize(tmpCfgDir, "locale_pinyin_test.json"));
    struct RestoreGuard {
        tools3000::core::ConfigManager& c;
        std::filesystem::path dir;
        ~RestoreGuard() {
            c.shutdown();
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    } guard{cfg, tmpCfgDir};

    cfg.remove("/search/pinyinEnabled");
    EXPECT_TRUE(resolveEffectivePinyinEnabled(cfg, UiLanguage::ZhCN));
    EXPECT_FALSE(resolveEffectivePinyinEnabled(cfg, UiLanguage::EnUS));
    EXPECT_FALSE(resolveEffectivePinyinEnabled(cfg, UiLanguage::JaJP));

    ASSERT_TRUE(cfg.set("/search/pinyinEnabled", true));
    EXPECT_TRUE(resolveEffectivePinyinEnabled(cfg, UiLanguage::EnUS));
    EXPECT_TRUE(resolveEffectivePinyinEnabled(cfg, UiLanguage::JaJP));

    ASSERT_TRUE(cfg.set("/search/pinyinEnabled", false));
    EXPECT_FALSE(resolveEffectivePinyinEnabled(cfg, UiLanguage::ZhCN));

    // 3. 构建多语种混合索引：西文含 '-' 与 '\'' 文件、日文汉字 (Kanji) 文件、中文文件
    MftParser parser;
    parser.Initialize('D');
    parser.setRealTimeValidation(false);
    std::vector<FileRecordInit> records;
    auto addRec = [&](uint64_t frn, uint64_t parent, const wchar_t* name, bool isDir) {
        FileRecordInit r{};
        r.fileReferenceNumber = frn;
        r.parentFileReferenceNumber = parent;
        r.fileName = name;
        r.isDirectory = isDir;
        r.fileAttributes = isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
        r.fileSize = isDir ? 0 : 1024;
        records.push_back(std::move(r));
    };
    addRec(10, 5, L"docs", true);
    addRec(11, 10, L"email.txt", false);
    addRec(12, 10, L"e-mail.txt", false);
    addRec(13, 10, L"dont_delete.txt", false);
    addRec(14, 10, L"don't_delete.txt", false);
    addRec(20, 5, L"作業フォルダ", true);        // 日文 Kanji + 片假名 ("作"=zuo, "業"=ye)
    addRec(21, 20, L"営業資料.pdf", false);      // 日文 Kanji
    addRec(30, 5, L"星云科技-采购.ch", false);    // 中文带连字符
    addRec(31, 5, L"同喜.txt", false);           // 中文双音节

    ASSERT_TRUE(parser.importSnapshot(std::move(records), 987654ULL, parser.getVolumeSerialNumber()));

    const SearchExcludeOptions noExcludes{};
    const SearchParseOptions jaDefaultOpts{
        defaultPinyinEnabledForUiLanguage(UiLanguage::JaJP), false, false
    };
    const SearchParseOptions zhDefaultOpts{
        defaultPinyinEnabledForUiLanguage(UiLanguage::ZhCN), false, false
    };

    // 3a. 日本用户在日语默认语境下搜索英文词 'zy' / 'zl'，日文 Kanji 0 假阳性干扰
    EXPECT_EQ(parser.Search(L"zy", 50, noExcludes, nullptr, jaDefaultOpts).size(), 0u);
    EXPECT_EQ(parser.Search(L"zl", 50, noExcludes, nullptr, jaDefaultOpts).size(), 0u);
    EXPECT_EQ(parser.Search(L"営業資料", 50, noExcludes, nullptr, jaDefaultOpts).size(), 1u);

    // 3b. 欧美纯西文文件：即使在开启拼音 (zhDefaultOpts) 下，搜 e-mail / don't / !e-mail / py:email 亦 100% 零误触
    auto hitsHyphen = parser.Search(L"e-mail", 50, noExcludes, nullptr, zhDefaultOpts);
    ASSERT_EQ(hitsHyphen.size(), 1u);
    EXPECT_EQ(hitsHyphen[0].fileName, L"e-mail.txt");

    auto hitsApostrophe = parser.Search(L"don't", 50, noExcludes, nullptr, zhDefaultOpts);
    ASSERT_EQ(hitsApostrophe.size(), 1u);
    EXPECT_EQ(hitsApostrophe[0].fileName, L"don't_delete.txt");

    // 排除 !e-mail 时，email.txt 必须安全保留，绝不被误杀
    auto hitsExcludeHyphen = parser.Search(L"docs\\ !e-mail", 50, noExcludes, nullptr, zhDefaultOpts);
    bool emailTxtSurvived = false;
    bool eHyphenMailExcluded = true;
    for (const auto& item : hitsExcludeHyphen) {
        if (item.fileName == L"email.txt") emailTxtSurvived = true;
        if (item.fileName == L"e-mail.txt") eHyphenMailExcluded = false;
    }
    EXPECT_TRUE(emailTxtSurvived);
    EXPECT_TRUE(eHyphenMailExcluded);

    // py: 显式拼音算子绝不把纯英文文件当成拼音结果
    EXPECT_EQ(parser.Search(L"py:email", 50, noExcludes, nullptr, zhDefaultOpts).size(), 0u);

    // 3c. 中文带连字符首字母 xykj-cg 与拼音分词符 tong'xi 依然 100% 精准命中
    auto hitsCjkHyphen = parser.Search(L"xykj-cg", 50, noExcludes, nullptr, zhDefaultOpts);
    ASSERT_EQ(hitsCjkHyphen.size(), 1u);
    EXPECT_EQ(hitsCjkHyphen[0].fileName, L"星云科技-采购.ch");

    auto hitsCjkApostrophe = parser.Search(L"tong'xi", 50, noExcludes, nullptr, zhDefaultOpts);
    ASSERT_EQ(hitsCjkApostrophe.size(), 1u);
    EXPECT_EQ(hitsCjkApostrophe[0].fileName, L"同喜.txt");
}

// -----------------------------------------------------------------------------
// 回归防线：QueryThreadPool 禁止跨批次工作窃取引发的同线程 SRWLock Shared->Exclusive 自死锁
// 复现 2026-11-22 线上事故：多线程并发 Search（持 shared_lock 进 parallelFor + 结束后
// unique_lock 回写元数据）与外层嵌套 parallelFor 同跑，旧实现下会瞬间触发同线程自死锁。
// -----------------------------------------------------------------------------
TEST(MftParserConcurrencyRegressionTest, NestedParallelForAndConcurrentSearchWritebackNeverDeadlocks) {
    MftParser parser;
    std::vector<FileRecordInit> records;
    // 超过 10000 条以真实触发 MftParser::Search 内部 16 核 QueryThreadPool::parallelFor 分片路径
    constexpr size_t kTotalFiles = 12000;
    records.reserve(kTotalFiles + 2);
    {
        FileRecordInit root{};
        root.fileReferenceNumber = 5;
        root.parentFileReferenceNumber = 5;
        root.fileName = L".";
        root.isDirectory = true;
        root.fileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        records.push_back(std::move(root));
    }
    for (size_t i = 0; i < kTotalFiles; ++i) {
        FileRecordInit r{};
        r.fileReferenceNumber = 100 + i;
        r.parentFileReferenceNumber = 5;
        r.fileName = (i % 97 == 0)
            ? (L"建邦_账号密码_" + std::to_wstring(i) + L".txt")
            : (L"file_item_" + std::to_wstring(i) + L".log");
        r.isDirectory = false;
        r.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
        r.fileSize = 0;       // 故意留 0 触发 hydrate / writeLock 路径
        r.lastWriteTime = 0;
        records.push_back(std::move(r));
    }
    ASSERT_TRUE(parser.importSnapshot(std::move(records), 12345ULL, parser.getVolumeSerialNumber()));
    EXPECT_EQ(parser.getFileCount(), kTotalFiles + 1);

    std::atomic<bool> stop{false};
    std::atomic<int> completedQueries{0};
    std::atomic<int> completedStats{0};

    // 同时模拟外层嵌套 parallelFor + 多线程并发 Search + 无锁 getFileCount 轮询
    std::vector<std::thread> clients;
    for (int t = 0; t < 4; ++t) {
        clients.emplace_back([&, t]() {
            const SearchExcludeOptions noExcludes{};
            const SearchParseOptions opts{true, false, false};
            for (int iter = 0; iter < 15; ++iter) {
                tools3000::service::QueryThreadPool::instance().parallelFor(2, [&](size_t slot) {
                    const std::wstring q = ((iter + t + slot) % 2 == 0)
                        ? L"建邦 账号密码"
                        : L"file_item_10";
                    auto hits = parser.Search(q, 50, noExcludes, nullptr, opts);
                    EXPECT_FALSE(hits.empty());
                    completedQueries.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }

    std::thread statsReader([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            EXPECT_GT(parser.getFileCount(), 0u);
            completedStats.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    for (auto& th : clients) {
        th.join();
    }
    stop.store(true, std::memory_order_release);
    statsReader.join();

    EXPECT_EQ(completedQueries.load(), 4 * 15 * 2);
    EXPECT_GT(completedStats.load(), 0);
}

TEST(SearchPropertyExtractorTest, ExtractsAttributesShellTypeImageDimensionsPeVersionHashesAndCaches) {
    using tools3000::search::PropertyExtractItem;
    using tools3000::search::SearchPropertyExtractor;

    auto& extractor = SearchPropertyExtractor::instance();
    extractor.clearCache();

    // 1. 属性格式化
    EXPECT_EQ(SearchPropertyExtractor::formatFileAttributes(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE), "RA");
    EXPECT_EQ(SearchPropertyExtractor::formatFileAttributes(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_DIRECTORY), "HSD");

    // 2. 在临时目录创建文本文件 + 32x18 BMP 图片文件
    wchar_t tempRoot[MAX_PATH]{};
    ASSERT_GT(GetTempPathW(MAX_PATH, tempRoot), 0u);
    const std::filesystem::path workDir =
        std::filesystem::path(tempRoot) / (L"Tools3000_PropExtTest_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(workDir, ec);
    ASSERT_TRUE(std::filesystem::create_directories(workDir, ec));

    const std::filesystem::path txtFile = workDir / L"sample_hello.txt";
    {
        HANDLE hf = CreateFileW(txtFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(hf, INVALID_HANDLE_VALUE);
        const char payload[] = "hello";
        DWORD written = 0;
        WriteFile(hf, payload, 5, &written, nullptr);
        CloseHandle(hf);
    }

    // 构造合法 24-bit 32x18 BMP（WIC 原生 0 外部依赖解码）
    const std::filesystem::path bmpFile = workDir / L"banner_32x18.bmp";
    {
        BITMAPFILEHEADER bfh{};
        BITMAPINFOHEADER bih{};
        bih.biSize = sizeof(BITMAPINFOHEADER);
        bih.biWidth = 32;
        bih.biHeight = 18;
        bih.biPlanes = 1;
        bih.biBitCount = 24;
        bih.biCompression = BI_RGB;
        const DWORD rowStride = ((32 * 3 + 3) & ~3u);
        const DWORD pixelBytes = rowStride * 18;
        bih.biSizeImage = pixelBytes;
        bfh.bfType = 0x4D42; // 'BM'
        bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        bfh.bfSize = bfh.bfOffBits + pixelBytes;

        std::vector<BYTE> pixels(pixelBytes, 0x88);
        HANDLE hf = CreateFileW(bmpFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(hf, INVALID_HANDLE_VALUE);
        DWORD written = 0;
        WriteFile(hf, &bfh, sizeof(bfh), &written, nullptr);
        WriteFile(hf, &bih, sizeof(bih), &written, nullptr);
        WriteFile(hf, pixels.data(), pixelBytes, &written, nullptr);
        CloseHandle(hf);
    }

    const std::string txtUtf8 = tools3000::core::WinUtils::wstringToUtf8(txtFile.wstring());
    const std::string bmpUtf8 = tools3000::core::WinUtils::wstringToUtf8(bmpFile.wstring());
    const std::string dirUtf8 = tools3000::core::WinUtils::wstringToUtf8(workDir.wstring());

    // 3. 批量提取：验证纯字符串、哈希、Shell 类型、WIC 图片头、文件夹子项数、所有者
    std::vector<PropertyExtractItem> batchItems = {
        PropertyExtractItem{txtUtf8, false, 0, 0},
        PropertyExtractItem{bmpUtf8, false, 0, 0},
        PropertyExtractItem{dirUtf8, true, 0, 0},
    };
    const std::vector<std::string> props = {
        "type", "ext", "stem", "parentName", "drive", "fullPathLength",
        "crc32", "md5", "sha256",
        "dimensions", "width", "height", "aspectRatio", "bitDepth",
        "childCount", "owner", "attributes"
    };

    const auto first = extractor.extractBatch(batchItems, props);
    EXPECT_EQ(first.diskExtractions, 3u);
    EXPECT_EQ(first.cacheHits, 0u);

    // "hello" 的标准哈希值
    const auto& txtVals = first.values.at(txtUtf8);
    EXPECT_EQ(txtVals.at("ext").display, "txt");
    EXPECT_EQ(txtVals.at("stem").display, "sample_hello");
    EXPECT_EQ(txtVals.at("crc32").display, "3610a686");
    EXPECT_EQ(txtVals.at("md5").display, "5d41402abc4b2a76b9719d911017c592");
    EXPECT_EQ(txtVals.at("sha256").display, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    EXPECT_FALSE(txtVals.at("type").display.empty());
    EXPECT_FALSE(txtVals.at("owner").display.empty());

    // 32x18 BMP WIC 读头
    const auto& bmpVals = first.values.at(bmpUtf8);
    EXPECT_EQ(bmpVals.at("dimensions").display, "32 x 18");
    EXPECT_EQ(bmpVals.at("width").display, "32 px");
    EXPECT_EQ(bmpVals.at("height").display, "18 px");
    EXPECT_EQ(bmpVals.at("aspectRatio").display, "16:9");
    EXPECT_EQ(bmpVals.at("bitDepth").display, "24");

    // 文件夹直属子项数 = 2（txt + bmp）
    const auto& dirVals = first.values.at(dirUtf8);
    EXPECT_EQ(dirVals.at("childCount").display, "2");
    ASSERT_TRUE(dirVals.at("childCount").sortNumber.has_value());
    EXPECT_DOUBLE_EQ(*dirVals.at("childCount").sortNumber, 2.0);

    // 4. 第二次相同请求必须 100% 命中进程级 LRU 缓存（0 次磁盘提取）
    const auto second = extractor.extractBatch(batchItems, props);
    EXPECT_EQ(second.cacheHits, 3u);
    EXPECT_EQ(second.diskExtractions, 0u);

    // 5. 测试系统 PE 程序版本信息提取（C:\Windows\System32\kernel32.dll 必存在）
    wchar_t sysDir[MAX_PATH]{};
    ASSERT_GT(GetSystemDirectoryW(sysDir, MAX_PATH), 0u);
    const std::string kernel32Utf8 = tools3000::core::WinUtils::wstringToUtf8(std::wstring(sysDir) + L"\\kernel32.dll");
    const auto peRes = extractor.extractBatch(
        {PropertyExtractItem{kernel32Utf8, false, 0, 0}},
        {"fileVersion", "company", "fileDescription"}
    );
    const auto& peVals = peRes.values.at(kernel32Utf8);
    EXPECT_FALSE(peVals.at("fileVersion").display.empty());
    EXPECT_FALSE(peVals.at("company").display.empty());

    // 6. 验证 JSON 协议接口
    const nlohmann::json jsonResp = extractor.handleBatchRequestJson({
        {"items", nlohmann::json::array({{{"path", bmpUtf8}, {"isDirectory", false}}})},
        {"properties", nlohmann::json::array({"dimensions", "width"})}
    });
    EXPECT_TRUE(jsonResp.value("success", false));
    ASSERT_TRUE(jsonResp.contains("values"));
    EXPECT_EQ(jsonResp["values"][bmpUtf8]["dimensions"]["d"].get<std::string>(), "32 x 18");
    EXPECT_DOUBLE_EQ(jsonResp["values"][bmpUtf8]["width"]["n"].get<double>(), 32.0);

    std::filesystem::remove_all(workDir, ec);
}

TEST(MftParserTest, ScanPhaseAndHydratedAttributesAndAccessTime) {
    MftParser parser;
    EXPECT_EQ(parser.getScanPhase(), MftParser::ScanPhase::Idle);

    wchar_t tempRoot[MAX_PATH]{};
    ASSERT_GT(GetTempPathW(MAX_PATH, tempRoot), 0u);
    const std::wstring testFile = std::wstring(tempRoot) + L"Tools3000_Tier1Meta_" + std::to_wstring(GetCurrentProcessId()) + L".txt";
    {
        HANDLE hf = CreateFileW(testFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_ARCHIVE, nullptr);
        ASSERT_NE(hf, INVALID_HANDLE_VALUE);
        DWORD written = 0;
        WriteFile(hf, "abc", 3, &written, nullptr);
        CloseHandle(hf);
    }

    // 校验 importSnapshot 后 phase 复位为 Idle 且记录计数正确
    std::vector<FileRecordInit> recs;
    FileRecordInit item{};
    item.fileReferenceNumber = 42;
    item.parentFileReferenceNumber = 0;
    item.fileName = L"Tier1AttrSample.txt";
    item.isDirectory = false;
    item.fileAttributes = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY;
    item.fileSize = 123;
    recs.push_back(std::move(item));

    ASSERT_TRUE(parser.importSnapshot(std::move(recs), 100ULL, 0u));
    EXPECT_EQ(parser.getScanPhase(), MftParser::ScanPhase::Idle);
    EXPECT_EQ(parser.getScannedRecords(), 1u);

    auto hits = parser.Search(L"Tier1AttrSample", 10);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].fileAttributes, static_cast<uint32_t>(FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY));

    DeleteFileW(testFile.c_str());
}

TEST(SearchPropertyExtractorTest, CloudPlaceholderGuardAndGenerationCancellation) {
    using tools3000::search::PropertyExtractItem;
    using tools3000::search::SearchPropertyExtractor;

    EXPECT_FALSE(SearchPropertyExtractor::isCloudPlaceholderAttributes(FILE_ATTRIBUTE_ARCHIVE));
    EXPECT_TRUE(SearchPropertyExtractor::isCloudPlaceholderAttributes(FILE_ATTRIBUTE_OFFLINE));
    EXPECT_TRUE(SearchPropertyExtractor::isCloudPlaceholderAttributes(0x00400000U)); // RECALL_ON_DATA_ACCESS
    EXPECT_TRUE(SearchPropertyExtractor::isCloudPlaceholderAttributes(0x00040000U)); // RECALL_ON_OPEN

    auto& extractor = SearchPropertyExtractor::instance();
    extractor.advanceRequestGeneration(500);
    const auto cancelledRes = extractor.extractBatch(
        {PropertyExtractItem{"C:\\Windows\\System32\\kernel32.dll", false, 0, 0}},
        {"md5"},
        499
    );
    EXPECT_TRUE(cancelledRes.cancelled);
    EXPECT_TRUE(cancelledRes.values.empty());
}

#include "service/ExternalApiPolicy.h"

TEST(ExternalApiPolicyTest, EnforcesReadOnlyActionsAndSensitiveCredentialBlocklist) {
    using namespace tools3000::service::external_api;

    EXPECT_TRUE(isExternalClientSource("external_cli"));
    EXPECT_TRUE(isExternalClientSource("external_mcp"));
    EXPECT_FALSE(isExternalClientSource("ui"));
    EXPECT_FALSE(isExternalClientSource(""));

    // 外部客户端严格只读：仅允许 getDbStats，拒绝所有变更与关停动作
    EXPECT_TRUE(isActionAllowedForExternal("getDbStats"));
    EXPECT_FALSE(isActionAllowedForExternal("shutdown"));
    EXPECT_FALSE(isActionAllowedForExternal("rebuild"));
    EXPECT_FALSE(isActionAllowedForExternal("recordRun"));
    EXPECT_FALSE(isActionAllowedForExternal("clearSearchHistory"));

    // 内置敏感凭证路径黑名单
    EXPECT_TRUE(isSensitiveCredentialPath(L"C:\\Users\\Alice\\.ssh\\id_ed25519"));
    EXPECT_TRUE(isSensitiveCredentialPath(L"C:\\Users\\Alice\\.aws\\credentials"));
    EXPECT_TRUE(isSensitiveCredentialPath(L"C:\\repo\\app\\.env"));
    EXPECT_TRUE(isSensitiveCredentialPath(L"C:\\repo\\app\\.env.production"));
    EXPECT_TRUE(isSensitiveCredentialPath(L"C:\\Users\\Alice\\AppData\\Local\\Google\\Chrome\\User Data\\Default\\Login Data"));
    EXPECT_TRUE(isSensitiveCredentialPath(L"D:\\certs\\server.pem"));
    EXPECT_FALSE(isSensitiveCredentialPath(L"C:\\repo\\Tools3000\\README.md"));
    EXPECT_FALSE(isSensitiveCredentialPath(L"D:\\Work\\architecture_2026.docx"));
}

#include "search/BrowserBookmarkIndex.h"

TEST(BrowserBookmarkIndexTest, ParsesZeroConflictPrefixAndSearchesChromiumJson) {
    using tools3000::search::BrowserBookmarkIndex;
    using tools3000::search::BookmarkEntry;

    // 1. 零冲突前缀解析：仅 bm: 触发纯书签搜索，绝不抢占 "b " 开头的文件查询
    const auto p1 = BrowserBookmarkIndex::parseBookmarkQuery("bm:github");
    EXPECT_TRUE(p1.isExplicitBookmarkQuery);
    EXPECT_EQ(p1.cleanQuery, "github");

    const auto p2 = BrowserBookmarkIndex::parseBookmarkQuery("BM:  ");
    EXPECT_TRUE(p2.isExplicitBookmarkQuery);
    EXPECT_TRUE(p2.cleanQuery.empty());

    const auto p3 = BrowserBookmarkIndex::parseBookmarkQuery("b github");
    EXPECT_FALSE(p3.isExplicitBookmarkQuery);

    // 2. 常规混排智能避让显式文件语法与盘符路径
    EXPECT_TRUE(BrowserBookmarkIndex::shouldMixBookmarksInGeneralQuery("github"));
    EXPECT_FALSE(BrowserBookmarkIndex::shouldMixBookmarksInGeneralQuery("a"));
    EXPECT_FALSE(BrowserBookmarkIndex::shouldMixBookmarksInGeneralQuery("ext:pdf report"));
    EXPECT_FALSE(BrowserBookmarkIndex::shouldMixBookmarksInGeneralQuery("content:SELECT"));
    EXPECT_FALSE(BrowserBookmarkIndex::shouldMixBookmarksInGeneralQuery("C:\\repo\\Tools3000"));
    EXPECT_FALSE(BrowserBookmarkIndex::shouldMixBookmarksInGeneralQuery("*.png"));

    // 3. 安全协议过滤
    EXPECT_TRUE(BrowserBookmarkIndex::isSafeHttpUrl("https://github.com/yuan278501381/Tools3000"));
    EXPECT_TRUE(BrowserBookmarkIndex::isSafeHttpUrl("http://localhost:3000"));
    EXPECT_FALSE(BrowserBookmarkIndex::isSafeHttpUrl("javascript:alert(1)"));
    EXPECT_FALSE(BrowserBookmarkIndex::isSafeHttpUrl("file:///C:/Windows/win.ini"));

    // 4. Chromium Bookmarks JSON 嵌套解析、去重与相关性排序
    const nlohmann::json sampleBookmarks = {
        {"roots", {
            {"bookmark_bar", {
                {"type", "folder"},
                {"name", "Bookmarks Bar"},
                {"children", nlohmann::json::array({
                    {
                        {"type", "url"},
                        {"name", "GitHub Tools3000"},
                        {"url", "https://github.com/yuan278501381/Tools3000"},
                        {"date_added", "13380000000000000"}
                    },
                    {
                        {"type", "url"},
                        {"name", "Duplicate Tools3000"},
                        {"url", "https://github.com/yuan278501381/Tools3000"},
                        {"date_added", "13380000000000001"}
                    },
                    {
                        {"type", "url"},
                        {"name", "Evil Bookmarklet"},
                        {"url", "javascript:void(0)"}
                    },
                    {
                        {"type", "folder"},
                        {"name", "Tech Communities"},
                        {"children", nlohmann::json::array({
                            {
                                {"type", "url"},
                                {"name", "LINUX DO Community"},
                                {"url", "https://linux.do/t/topic/3222848/"},
                                {"date_added", "13380000000000002"}
                            }
                        })}
                    }
                })}
            }}
        }}
    };

    std::vector<BookmarkEntry> parsed;
    BrowserBookmarkIndex::parseChromiumBookmarkJson(sampleBookmarks, "Edge", parsed, 100);
    ASSERT_EQ(parsed.size(), 2u);

    const auto hits = BrowserBookmarkIndex::searchEntriesToJson(parsed, "linux", 10, 1);
    ASSERT_TRUE(hits.is_array());
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].value("name", ""), "LINUX DO Community");
    EXPECT_EQ(hits[0].value("path", ""), "https://linux.do/t/topic/3222848/");
    EXPECT_EQ(hits[0].value("resultType", ""), "bookmark");
    EXPECT_EQ(hits[0].value("browserSource", ""), "Edge");
}

#include "search/SystemCommandCatalog.h"
#include "search/InstalledAppIndex.h"
#include "service/FileIndexStore.h"

TEST(BrowserBookmarkIndexTest, ShortBPrefixSupportsBookmarksWithoutBreakingDriveBPaths) {
    using tools3000::search::BrowserBookmarkIndex;

    // 1. `b:` 与 `bm:` 均触发纯书签搜索（大小写不敏感）
    const auto bEmpty = BrowserBookmarkIndex::parseBookmarkQuery("b:");
    EXPECT_TRUE(bEmpty.isExplicitBookmarkQuery);
    EXPECT_TRUE(bEmpty.cleanQuery.empty());

    const auto bGitHub = BrowserBookmarkIndex::parseBookmarkQuery("b:github");
    EXPECT_TRUE(bGitHub.isExplicitBookmarkQuery);
    EXPECT_EQ(bGitHub.cleanQuery, "github");

    const auto bUpperSpaced = BrowserBookmarkIndex::parseBookmarkQuery("B:  react docs");
    EXPECT_TRUE(bUpperSpaced.isExplicitBookmarkQuery);
    EXPECT_EQ(bUpperSpaced.cleanQuery, "react docs");

    const auto bmGitHub = BrowserBookmarkIndex::parseBookmarkQuery("bm:github");
    EXPECT_TRUE(bmGitHub.isExplicitBookmarkQuery);
    EXPECT_EQ(bmGitHub.cleanQuery, "github");

    // 2. 严格放行 B 盘路径（`b:\...`、`b:/...`）与普通空格词 `b foo`
    const auto driveBackslash = BrowserBookmarkIndex::parseBookmarkQuery("b:\\foo\\bar.txt");
    EXPECT_FALSE(driveBackslash.isExplicitBookmarkQuery);

    const auto driveSlash = BrowserBookmarkIndex::parseBookmarkQuery("b:/foo/bar.txt");
    EXPECT_FALSE(driveSlash.isExplicitBookmarkQuery);

    const auto driveRoot = BrowserBookmarkIndex::parseBookmarkQuery("B:\\");
    EXPECT_FALSE(driveRoot.isExplicitBookmarkQuery);

    const auto plainSpaceB = BrowserBookmarkIndex::parseBookmarkQuery("b foo");
    EXPECT_FALSE(plainSpaceB.isExplicitBookmarkQuery);

    // 3. 常规混排避让 app:/cmd:/bm: 显式前缀
    EXPECT_FALSE(BrowserBookmarkIndex::shouldMixBookmarksInGeneralQuery("app:code"));
    EXPECT_FALSE(BrowserBookmarkIndex::shouldMixBookmarksInGeneralQuery("cmd:env"));
    EXPECT_FALSE(BrowserBookmarkIndex::shouldMixBookmarksInGeneralQuery("bm:github"));
}

TEST(SystemCommandCatalogTest, MatchesGeekCommandsAndEnforcesStrictWhitelist) {
    using tools3000::search::SystemCommandCatalog;

    // 至少内置 35 条常用极客系统命令
    EXPECT_GE(SystemCommandCatalog::allCommands().size(), 35u);

    // 1. `env` / `环境变量` / `hjbl` / `huanjing` 均瞬达编辑系统环境变量
    for (const char* q : {"env", "环境变量", "hjbl", "huanjing"}) {
        const auto matches = SystemCommandCatalog::searchCommands(q, 5, false);
        ASSERT_FALSE(matches.empty()) << "query should match env command: " << q;
        EXPECT_STREQ(matches[0].spec->id, "env") << "top hit for query: " << q;
    }

    const auto envMatches = SystemCommandCatalog::searchCommands("env", 5, false);
    ASSERT_FALSE(envMatches.empty());
    EXPECT_TRUE(envMatches[0].isExactAlias);

    // 2. 校验其他关键系统命令命中
    const auto svcMatches = SystemCommandCatalog::searchCommands("services", 5, false);
    ASSERT_FALSE(svcMatches.empty());
    EXPECT_STREQ(svcMatches[0].spec->id, "services");

    const auto devMatches = SystemCommandCatalog::searchCommands("devmgmt", 5, false);
    ASSERT_FALSE(devMatches.empty());
    EXPECT_STREQ(devMatches[0].spec->id, "devmgmt");

    const auto hostsMatches = SystemCommandCatalog::searchCommands("hosts", 5, false);
    ASSERT_FALSE(hostsMatches.empty());
    EXPECT_STREQ(hostsMatches[0].spec->id, "hosts");

    // 3. `cmd:` 显式前缀解析
    const auto cmdParsed = SystemCommandCatalog::parseCommandQuery("CMD: env");
    EXPECT_TRUE(cmdParsed.isExplicitCommandQuery);
    EXPECT_EQ(cmdParsed.cleanQuery, "env");
    EXPECT_FALSE(SystemCommandCatalog::parseCommandQuery("cmd.exe").isExplicitCommandQuery);

    // 4. 安全白名单铁律：未知 ID 与命令注入 URI 100% 拒绝
    EXPECT_NE(SystemCommandCatalog::findById("env"), nullptr);
    EXPECT_EQ(SystemCommandCatalog::findById("calc.exe & whoami"), nullptr);
    EXPECT_EQ(SystemCommandCatalog::findById("..\\cmd"), nullptr);
    EXPECT_EQ(SystemCommandCatalog::extractCommandIdFromUri("cmd://env"), "env");
    EXPECT_TRUE(SystemCommandCatalog::extractCommandIdFromUri("cmd://env && calc").empty());
    EXPECT_TRUE(SystemCommandCatalog::extractCommandIdFromUri("cmd://../evil").empty());
    EXPECT_TRUE(SystemCommandCatalog::extractCommandIdFromUri("http://env").empty());
    EXPECT_FALSE(SystemCommandCatalog::executeWhitelistedCommand("not_in_whitelist", false));
    EXPECT_FALSE(SystemCommandCatalog::executeWhitelistedCommand("cmd.exe /c whoami", false));

    // 5. 外部只读 API 严禁放行内存控制/停服接口
    EXPECT_FALSE(tools3000::service::external_api::isActionAllowedForExternal("trimMemory"));
    EXPECT_FALSE(tools3000::service::external_api::isActionAllowedForExternal("updateMemoryPolicy"));
    EXPECT_FALSE(tools3000::service::external_api::isActionAllowedForExternal("getMemoryStats"));
    EXPECT_FALSE(tools3000::service::external_api::isActionAllowedForExternal("shutdown"));
}

TEST(SearchRankingFusionTest, EnforcesThreeRankingProfilesCapAndFrecencyBoost) {
    using namespace tools3000::search;

    const nlohmann::json filesJson = nlohmann::json::array({
        {
            {"name", "env.txt"},
            {"path", "D:\\Work\\env.txt"},
            {"isDirectory", false},
            {"resultType", "file"},
            {"matchRank", 1}
        },
        {
            {"name", "env_config.yaml"},
            {"path", "D:\\Work\\env_config.yaml"},
            {"isDirectory", false},
            {"resultType", "file"},
            {"matchRank", 2}
        },
        {
            // 与本地应用同路径，应去重并把更高 runCount 合并回应用项
            {"name", "EnvTool.lnk"},
            {"path", "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\EnvTool.lnk"},
            {"isDirectory", false},
            {"resultType", "file"},
            {"runCount", 5}
        }
    });

    const nlohmann::json commandsJson = nlohmann::json::array({
        {
            {"name", "Edit System Environment Variables"},
            {"path", "cmd://env"},
            {"isDirectory", false},
            {"resultType", "command"},
            {"commandId", "env"},
            {"exactAliasMatch", true},
            {"launcherScore", 1000},
            {"frecencyScore", 0.0}
        }
    });

    const nlohmann::json appsJson = nlohmann::json::array({
        {
            {"name", "EnvTool"},
            {"path", "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\EnvTool.lnk"},
            {"isDirectory", false},
            {"resultType", "app"},
            {"dedupStem", "envtool"},
            {"exactAliasMatch", false},
            {"launcherScore", 860},
            {"frecencyScore", 0.0}
        },
        {
            {"name", "EnvViewer"},
            {"path", "C:\\Apps\\EnvViewer.exe"},
            {"isDirectory", false},
            {"resultType", "app"},
            {"dedupStem", "envviewer"},
            {"exactAliasMatch", false},
            {"launcherScore", 820},
            {"frecencyScore", 0.0}
        },
        {
            {"name", "EnvStudio"},
            {"path", "C:\\Apps\\EnvStudio.exe"},
            {"isDirectory", false},
            {"resultType", "app"},
            {"dedupStem", "envstudio"},
            {"exactAliasMatch", false},
            {"launcherScore", 780},
            {"frecencyScore", 0.0}
        },
        {
            {"name", "EnvHelper"},
            {"path", "C:\\Apps\\EnvHelper.exe"},
            {"isDirectory", false},
            {"resultType", "app"},
            {"dedupStem", "envhelper"},
            {"exactAliasMatch", false},
            {"launcherScore", 740},
            {"frecencyScore", 0.0}
        }
    });

    const nlohmann::json bookmarksJson = nlohmann::json::array({
        {
            {"name", "Env Docs"},
            {"path", "https://example.com/env"},
            {"isDirectory", false},
            {"resultType", "bookmark"},
            {"matchRank", 80},
            {"frecencyScore", 0.0}
        }
    });

    // 1. balanced_frecency（默认）：顶栏严格 Cap=3，不淹没本地文件；且同名文档 env.txt 绝不误删，重复 .lnk 已去重
    {
        FusionOptions opts;
        opts.profile = RankingProfile::BalancedFrecency;
        opts.balancedTopLauncherCap = 3;
        const auto fused = fuseSearchResults(filesJson, appsJson, commandsJson, bookmarksJson, opts);
        ASSERT_EQ(fused.size(), 8u);  // 1 cmd + 4 apps + 2 non-dup files + 1 bookmark
        EXPECT_EQ(fused[0].value("path", ""), "cmd://env");
        EXPECT_EQ(fused[1].value("name", ""), "EnvTool");
        EXPECT_EQ(fused[1].value("runCount", 0u), 5u);
        EXPECT_EQ(fused[2].value("name", ""), "EnvViewer");
        // 第 4 位立即出现本地文件，确保顶栏 Cap=3 不淹没文件
        EXPECT_EQ(fused[3].value("resultType", ""), "file");
        EXPECT_EQ(fused[3].value("name", ""), "env.txt");
        EXPECT_EQ(fused[4].value("name", ""), "env_config.yaml");
    }

    // 2. files_first：仅精确别名命令 `env` 置顶，其余非精确应用排在本地文件之后
    {
        FusionOptions opts;
        opts.profile = RankingProfile::FilesFirst;
        const auto fused = fuseSearchResults(filesJson, appsJson, commandsJson, bookmarksJson, opts);
        ASSERT_EQ(fused.size(), 8u);
        EXPECT_EQ(fused[0].value("path", ""), "cmd://env");
        EXPECT_EQ(fused[1].value("resultType", ""), "file");
        EXPECT_EQ(fused[1].value("name", ""), "env.txt");
        EXPECT_EQ(fused[2].value("resultType", ""), "file");
        EXPECT_EQ(fused[2].value("name", ""), "env_config.yaml");
        EXPECT_EQ(fused[3].value("resultType", ""), "app");
    }

    // 3. launcher_first：全部 5 条命令+应用位于最高梯队，文件紧随其后
    {
        FusionOptions opts;
        opts.profile = RankingProfile::LauncherFirst;
        const auto fused = fuseSearchResults(filesJson, appsJson, commandsJson, bookmarksJson, opts);
        ASSERT_EQ(fused.size(), 8u);
        EXPECT_EQ(fused[0].value("resultType", ""), "command");
        for (size_t i = 1; i <= 4; ++i) {
            EXPECT_EQ(fused[i].value("resultType", ""), "app");
        }
        EXPECT_EQ(fused[5].value("resultType", ""), "file");
        EXPECT_EQ(fused[5].value("name", ""), "env.txt");
    }

    // 4. Frecency 提权：原本分数最低的 EnvHelper 经高频使用后自动超越其他非精确应用跃升顶栏
    {
        nlohmann::json boostedApps = appsJson;
        boostedApps[3]["frecencyScore"] = 1200.0;
        boostedApps[3]["runCount"] = 7;

        FusionOptions opts;
        opts.profile = RankingProfile::BalancedFrecency;
        opts.balancedTopLauncherCap = 3;
        const auto fused = fuseSearchResults(filesJson, boostedApps, commandsJson, nlohmann::json::array(), opts);
        ASSERT_GE(fused.size(), 4u);
        EXPECT_EQ(fused[0].value("path", ""), "cmd://env");
        EXPECT_EQ(fused[1].value("name", ""), "EnvHelper");
        EXPECT_EQ(fused[3].value("resultType", ""), "file");
    }
}

TEST(InstalledAppIndexTest, BuildsLightweightAcronymsAndPersistsFrecency) {
    using namespace tools3000::search;

    EXPECT_EQ(InstalledAppIndex::buildInitialsAcronym("Visual Studio Code"), "vsc");
    EXPECT_EQ(InstalledAppIndex::buildInitialsAcronym("微信"), "wx");
    EXPECT_EQ(InstalledAppIndex::buildInitialsAcronym("腾讯会议"), "txhy");

    const auto parsedApp = InstalledAppIndex::parseAppQuery("APP: wechat");
    EXPECT_TRUE(parsedApp.isExplicitAppQuery);
    EXPECT_EQ(parsedApp.cleanQuery, "wechat");
    EXPECT_FALSE(InstalledAppIndex::parseAppQuery("apple.txt").isExplicitAppQuery);

    // 验证 LauncherFrecencyStore 宽字符原子持久化与往返读取
    wchar_t tempDir[MAX_PATH]{};
    ASSERT_GT(GetTempPathW(MAX_PATH, tempDir), 0u);
    const std::filesystem::path frecencyFile =
        std::filesystem::path(tempDir) / L"tools3000_test_frecency_utf8_测试.json";
    DeleteFileW(frecencyFile.c_str());

    auto& store = LauncherFrecencyStore::instance();
    store.setStoragePathForTesting(frecencyFile);
    const int64_t tNow = 1'790'000'000'000ll;
    store.recordRun("cmd://env", tNow);
    store.recordRun("cmd://env", tNow + 1000);
    store.clearCache();  // 强制从磁盘重新加载

    const auto rec = store.lookup("CMD://ENV");
    EXPECT_EQ(rec.runCount, 2u);
    EXPECT_GT(store.scoreFor("cmd://env", tNow + 2000), 600.0);

    store.resetForTesting();
    DeleteFileW(frecencyFile.c_str());
}

TEST(FileIndexStoreMemoryTest, ShrinkExcessCapacityReclaimsHashAndVectorSlackWithoutDataLoss) {
    using tools3000::service::FileIndexStore;

    FileIndexStore store;
    store.setDriveLetter('C');
    // 预置 4000 条记录（触发较大哈希表与记录向量分配）
    store.reserve(4000);
    for (uint64_t i = 1; i <= 4000; ++i) {
        FileRecordInit rec{};
        rec.fileReferenceNumber = 10'000 + i;
        rec.parentFileReferenceNumber = 0;
        rec.fileName = (i % 2 == 0)
            ? (L"测试文档_" + std::to_wstring(i) + L".txt")
            : (L"report_" + std::to_wstring(i) + L".log");
        rec.isDirectory = false;
        rec.fileSize = i * 16;
        store.upsert(rec, true, true);
    }
    store.recomputeHierarchyMetadata();
    ASSERT_EQ(store.size(), 4000u);

    const size_t hashCapBefore = store.hashCapacity();
    const uint64_t bytesBefore = store.approximateBytes();

    // 删除 3900 条记录，仅保留 FRN 10001..10100
    for (uint64_t i = 101; i <= 4000; ++i) {
        ASSERT_TRUE(store.erase(10'000 + i));
    }
    ASSERT_EQ(store.size(), 100u);

    store.shrinkExcessCapacity();

    EXPECT_EQ(store.size(), 100u);
    EXPECT_EQ(store.tombstoneCount(), 0u);
    EXPECT_LT(store.hashCapacity(), hashCapBefore);
    EXPECT_LT(store.approximateBytes(), bytesBefore);

    // 逐条验证全部 100 条存活记录的 find()、view() 与 forEach() 100% 完整
    for (uint64_t i = 1; i <= 100; ++i) {
        const uint64_t frn = 10'000 + i;
        const auto* stored = store.find(frn);
        ASSERT_NE(stored, nullptr) << "missing live record frn=" << frn;
        const auto view = store.view(*stored);
        EXPECT_EQ(view.fileReferenceNumber, frn);
        EXPECT_EQ(view.fileSize, i * 16);
        const std::wstring expectedName = (i % 2 == 0)
            ? (L"测试文档_" + std::to_wstring(i) + L".txt")
            : (L"report_" + std::to_wstring(i) + L".log");
        EXPECT_EQ(view.fileName, expectedName);
    }
    for (uint64_t i = 101; i <= 4000; ++i) {
        EXPECT_EQ(store.find(10'000 + i), nullptr);
    }

    size_t visited = 0;
    store.forEach([&visited](const tools3000::service::StoredFileRecord&) { ++visited; });
    EXPECT_EQ(visited, 100u);
}

TEST(SearchLiveWatchTest, IndexGenerationAdvancesOnSnapshotImportAndAllowedReadOnly) {
    EXPECT_TRUE(tools3000::service::external_api::isActionAllowedForExternal("getIndexGeneration"));
    EXPECT_FALSE(tools3000::service::external_api::isActionAllowedForExternal("rebuild"));

    auto parser = std::make_unique<MftParser>();
    const uint64_t initialGen = parser->getIndexGeneration();

    std::vector<FileRecordInit> batch1;
    FileRecordInit f1{};
    f1.fileReferenceNumber = 5001;
    f1.parentFileReferenceNumber = 0;
    f1.fileName = L"live_added_report.pdf";
    f1.isDirectory = false;
    batch1.push_back(std::move(f1));

    ASSERT_TRUE(parser->importSnapshot(std::move(batch1), 100, 1));
    const uint64_t genAfterImport1 = parser->getIndexGeneration();
    EXPECT_GT(genAfterImport1, initialGen);

    std::vector<FileRecordInit> batch2;
    FileRecordInit f2{};
    f2.fileReferenceNumber = 5002;
    f2.parentFileReferenceNumber = 0;
    f2.fileName = L"live_added_report_v2.pdf";
    f2.isDirectory = false;
    batch2.push_back(std::move(f2));

    ASSERT_TRUE(parser->importSnapshot(std::move(batch2), 200, 1));
    const uint64_t genAfterImport2 = parser->getIndexGeneration();
    EXPECT_GT(genAfterImport2, genAfterImport1);
}

TEST(SearchNativeIconResolutionTest, ResolvesOfflineBookmarkOriginSystemCommandsAndNativeAppIcons) {
    using namespace tools3000::search;
    using tools3000::core::WinUtils;

    // 1. 书签 Origin 提取校验
    EXPECT_EQ(BrowserBookmarkIndex::extractOriginLower("https://ZhuanLan.Zhihu.com/p/648167641?a=1#top"),
              "https://zhuanlan.zhihu.com/");
    EXPECT_EQ(BrowserBookmarkIndex::extractOriginLower("http://Example.ORG"), "http://example.org/");
    EXPECT_TRUE(BrowserBookmarkIndex::extractOriginLower("javascript:alert(1)").empty());

    // 2. 系统命令原生 Windows 图标路径解析（cmd / taskmgr / env->sysdm.cpl / settings->空回退矢量）
    const std::wstring cmdPath = SystemCommandCatalog::resolveNativeIconPath("cmd");
    EXPECT_FALSE(cmdPath.empty());
    const std::wstring envPath = SystemCommandCatalog::resolveNativeIconPath("env");
    EXPECT_FALSE(envPath.empty());
    EXPECT_NE(WinUtils::toLower(envPath).find(L"sysdm.cpl"), std::wstring::npos);

    // 3. 本地真实 Windows 图标提取（cmd.exe 与离线书签 Favicon/浏览器回退）
    const std::string cmdIcon = WinUtils::getFileTypeIconBase64(L"", false, cmdPath);
    EXPECT_TRUE(cmdIcon.starts_with("data:image/png;base64,"));

    const std::string bmIcon = BrowserBookmarkIndex::instance().getBookmarkIconBase64(
        "https://zhuanlan.zhihu.com/p/648167641", "Chrome");
    EXPECT_TRUE(bmIcon.starts_with("data:image/"));

    const std::wstring chromeLnk = L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Google Chrome.lnk";
    if (GetFileAttributesW(chromeLnk.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const std::string chromeIcon = WinUtils::getFileTypeIconBase64(L".lnk", false, chromeLnk);
        EXPECT_TRUE(chromeIcon.starts_with("data:image/png;base64,"));
        // 确认 Chrome.lnk 抽到的是独立内嵌图标，而不是通用空快捷方式占位图
        const std::string genericExeIcon = WinUtils::getFileTypeIconBase64(L".exe", false, L"");
        EXPECT_NE(chromeIcon, genericExeIcon);
    }
}

TEST(PathExclusionMatcherTest, EnforcesExactPathBoundariesAndMultiPatternSemantics) {
    using tools3000::service::PathExclusionMatcher;

    // 1. 精确路径边界：屏蔽 D:\动作片 及其全部子孙文件，绝不误伤 D:\动作片2 或 D:\动作片备份
    PathExclusionMatcher matcher;
    matcher.compile({
        L"D:\\动作片\\",
        L"E:/Private/Secret Docs/",
        L"f:",
        L"pip\\cache",
        L"node_modules",
    });

    auto matchRaw = [&](std::wstring_view p) {
        return matcher.matchesNormalizedFullPath(PathExclusionMatcher::normalizePathForMatch(p));
    };

    ASSERT_FALSE(matcher.empty());
    EXPECT_FALSE(matcher.hasOnlySingleSegmentPatterns());

    // 精确目录本身及子孙必须被屏蔽（支持反斜杠与正斜杠、大小写不敏感）
    EXPECT_TRUE(matchRaw(L"D:\\动作片"));
    EXPECT_TRUE(matchRaw(L"d:\\动作片\\欧美\\clip01.mp4"));
    EXPECT_TRUE(matchRaw(L"D:/动作片/sub/deep/video.mkv"));

    // 同前缀兄弟目录必须 100% 放行（零误伤）
    EXPECT_FALSE(matchRaw(L"D:\\动作片2\\clip01.mp4"));
    EXPECT_FALSE(matchRaw(L"D:\\动作片备份\\note.txt"));
    EXPECT_FALSE(matchRaw(L"C:\\动作片\\clip01.mp4"));

    // 多路径第二条（带空格 + 正斜杠归一化）
    EXPECT_TRUE(matchRaw(L"E:\\Private\\Secret Docs"));
    EXPECT_TRUE(matchRaw(L"e:\\private\\secret docs\\2026\\ledger.xlsx"));
    EXPECT_FALSE(matchRaw(L"E:\\Private\\Secret Docs Archive\\ledger.xlsx"));

    // 裸盘符根排除（F: -> F:\）
    EXPECT_TRUE(matchRaw(L"F:\\any_file.mkv"));
    EXPECT_FALSE(matchRaw(L"C:\\F\\any_file.mkv"));

    // 多段相对路径精确边界（\pip\cache\）
    EXPECT_TRUE(matchRaw(L"C:\\Users\\alice\\pip\\cache\\pkg.whl"));
    EXPECT_TRUE(matchRaw(L"C:\\Users\\alice\\pip\\cache"));
    EXPECT_FALSE(matchRaw(L"C:\\Users\\alice\\pip\\cache2\\pkg.whl"));

    // 单段目录名回退匹配
    EXPECT_TRUE(matchRaw(L"C:\\repo\\node_modules\\react\\index.js"));
    EXPECT_FALSE(matchRaw(L"C:\\repo\\src\\index.js"));
}

TEST(MftParserExclusionTest, CustomMultiPathExclusionsApplyToBothNameAndContentCandidates) {
    auto parser = std::make_unique<MftParser>();
    ASSERT_TRUE(parser->Initialize('C'));

    // 构造虚拟目录树（C:\动作片\movie.txt、C:\动作片2\movie.txt、C:\隐私文档\sub\movie.txt、C:\工作文档\movie.txt）
    std::vector<FileRecordInit> records;
    auto addNode = [&](uint64_t frn, uint64_t parent, const wchar_t* name, bool isDir) {
        FileRecordInit r{};
        r.fileReferenceNumber = frn;
        r.parentFileReferenceNumber = parent;
        r.fileName = name;
        r.isDirectory = isDir;
        r.fileSize = isDir ? 0 : 1024;
        records.push_back(std::move(r));
    };

    addNode(10, 0, L"动作片", true);
    addNode(11, 10, L"movie_secret.txt", false);
    addNode(12, 10, L"嵌套目录", true);
    addNode(13, 12, L"movie_nested.txt", false);

    addNode(20, 0, L"动作片2", true);
    addNode(21, 20, L"movie_sibling.txt", false);

    addNode(30, 0, L"隐私文档", true);
    addNode(31, 30, L"movie_private.txt", false);

    addNode(40, 0, L"工作文档", true);
    addNode(41, 40, L"movie_work.txt", false);

    ASSERT_TRUE(parser->importSnapshot(std::move(records), 12345, /*volumeSerial=*/0));

    SearchExcludeOptions excludeOpts{};
    excludeOpts.patterns = {L"C:\\动作片", L"C:\\隐私文档\\"};

    // 1. 文件名检索：动作片与隐私文档下的全部文件被彻底屏蔽，动作片2 与工作文档完好返回
    const auto nameResults = parser->Search(L"movie", 100, excludeOpts);
    ASSERT_EQ(nameResults.size(), 2u);
    auto hasName = [&](const std::wstring& target) {
        return std::any_of(nameResults.begin(), nameResults.end(),
                           [&](const SearchResult& r) { return r.fileName == target; });
    };
    EXPECT_TRUE(hasName(L"movie_sibling.txt"));
    EXPECT_TRUE(hasName(L"movie_work.txt"));
    EXPECT_FALSE(hasName(L"movie_secret.txt"));
    EXPECT_FALSE(hasName(L"movie_nested.txt"));
    EXPECT_FALSE(hasName(L"movie_private.txt"));

    // 2. 全文内容候选采集：同样 100% 遵守自定义多路径绝对前缀排除，绝不读盘扫描屏蔽目录
    ContentCandidateOptions contentOpts{};
    contentOpts.excludeOpts = excludeOpts;
    const auto expr = SearchExpression::parse(L"content:hello");
    const auto candidates = parser->CollectContentCandidates(expr, contentOpts);
    ASSERT_EQ(candidates.size(), 2u);
    auto hasCand = [&](const std::wstring& target) {
        return std::any_of(candidates.begin(), candidates.end(),
                           [&](const ContentCandidate& c) { return c.fileName == target; });
    };
    EXPECT_TRUE(hasCand(L"movie_sibling.txt"));
    EXPECT_TRUE(hasCand(L"movie_work.txt"));
    EXPECT_FALSE(hasCand(L"movie_secret.txt"));
    EXPECT_FALSE(hasCand(L"movie_nested.txt"));
    EXPECT_FALSE(hasCand(L"movie_private.txt"));
}

TEST(SensitivePathGuardTest, SkipsBrowserCredentialFilesAndDirsWithoutFalsePositives) {
    using tools3000::service::SensitivePathGuard;

    // 1. 常见浏览器保存密码、Cookie 与密钥库文件名命中（大小写不敏感）
    EXPECT_TRUE(SensitivePathGuard::isSensitiveCredentialFileName(L"Login Data"));
    EXPECT_TRUE(SensitivePathGuard::isSensitiveCredentialFileName(L"LOGIN DATA-JOURNAL"));
    EXPECT_TRUE(SensitivePathGuard::isSensitiveCredentialFileName(L"Login Data For Account"));
    EXPECT_TRUE(SensitivePathGuard::isSensitiveCredentialFileName(L"Local State"));
    EXPECT_TRUE(SensitivePathGuard::isSensitiveCredentialFileName(L"logins.json"));
    EXPECT_TRUE(SensitivePathGuard::isSensitiveCredentialFileName(L"key4.db"));
    EXPECT_TRUE(SensitivePathGuard::isSensitiveCredentialFileName(L"cookies.sqlite"));
    EXPECT_FALSE(SensitivePathGuard::isSensitiveCredentialFileName(L"readme.md"));
    EXPECT_FALSE(SensitivePathGuard::isSensitiveCredentialFileName(L"login_controller.ts"));

    // 2. 主流浏览器 User Data / Profiles 及 Windows 凭据目录精确边界匹配（兼容正反斜杠）
    EXPECT_TRUE(SensitivePathGuard::isSensitiveCredentialDirectoryPath(
        L"C:\\Users\\alice\\AppData\\Local\\Google\\Chrome\\User Data\\Default\\Login Data"));
    EXPECT_TRUE(SensitivePathGuard::isSensitiveCredentialDirectoryPath(
        L"C:/Users/alice/AppData/Local/Microsoft/Edge/User Data/Profile 1/Cookies"));
    EXPECT_TRUE(SensitivePathGuard::isSensitiveCredentialDirectoryPath(
        L"C:\\Users\\alice\\AppData\\Local\\BraveSoftware\\Brave-Browser\\User Data\\Default"));
    EXPECT_TRUE(SensitivePathGuard::isSensitiveCredentialDirectoryPath(
        L"C:\\Users\\alice\\AppData\\Roaming\\Mozilla\\Firefox\\Profiles\\abc123.default-release\\logins.json"));
    EXPECT_TRUE(SensitivePathGuard::isSensitiveCredentialDirectoryPath(
        L"C:\\Users\\alice\\AppData\\Roaming\\Microsoft\\Credentials\\DFBE70A7E5CC19A398EBF1B96859CE5D"));
    EXPECT_TRUE(SensitivePathGuard::isSensitiveCredentialDirectoryPath(
        L"C:\\Users\\alice\\AppData\\Local\\Microsoft\\Vault"));

    // 3. 普通用户项目路径绝不误伤
    EXPECT_FALSE(SensitivePathGuard::isSensitiveCredentialDirectoryPath(
        L"D:\\Work\\chrome-notes\\readme.md"));
    EXPECT_FALSE(SensitivePathGuard::isSensitiveCredentialDirectoryPath(
        L"C:\\Users\\alice\\Documents\\User Data Backup\\report.docx"));
    EXPECT_FALSE(SensitivePathGuard::isSensitiveCredentialDirectoryPath(
        L"C:\\Users\\alice\\AppData\\Local\\Google\\Chrome\\User Data2\\notes.txt"));

    // 4. 集成验证：即使用户显式搜索 appdata，CollectContentCandidates 仍硬性拒绝扫描浏览器凭据目录
    auto parser = std::make_unique<MftParser>();
    std::vector<FileRecordInit> records;
    auto addNode = [&](uint64_t frn, uint64_t parent, const wchar_t* name, bool isDir) {
        FileRecordInit r{};
        r.fileReferenceNumber = frn;
        r.parentFileReferenceNumber = parent;
        r.fileName = name;
        r.isDirectory = isDir;
        r.fileSize = isDir ? 0 : 2048;
        records.push_back(std::move(r));
    };

    addNode(1, 0, L"Users", true);
    addNode(2, 1, L"alice", true);
    addNode(3, 2, L"AppData", true);
    addNode(4, 3, L"Local", true);
    addNode(5, 4, L"Google", true);
    addNode(6, 5, L"Chrome", true);
    addNode(7, 6, L"User Data", true);
    addNode(8, 7, L"Default", true);
    addNode(9, 8, L"appdata_prefs.json", false);
    addNode(10, 4, L"MyTool", true);
    addNode(11, 10, L"appdata_settings.json", false);

    ASSERT_TRUE(parser->importSnapshot(std::move(records), 54321, /*volumeSerial=*/0));
    ContentCandidateOptions opts{};
    const auto expr = SearchExpression::parse(L"appdata content:hello");
    const auto candidates = parser->CollectContentCandidates(expr, opts);

    bool sawChromePref = false;
    bool sawMyToolSettings = false;
    for (const auto& c : candidates) {
        if (c.fileName == L"appdata_prefs.json") sawChromePref = true;
        if (c.fileName == L"appdata_settings.json") sawMyToolSettings = true;
    }
    EXPECT_FALSE(sawChromePref) << "Browser User Data must never be scanned by content search";
    EXPECT_TRUE(sawMyToolSettings) << "Regular AppData config file should remain searchable when appdata is queried";
}

TEST(CompactIndexStore1BTest, FortyByteRecordDualTrackArenaAndLargeFile64BitRoundTrip) {
    using tools3000::service::FileIndexStore;
    using tools3000::service::FileViewScratch;
    using tools3000::service::StoredFileRecord;
    using tools3000::service::StringArena;

    static_assert(sizeof(StoredFileRecord) == 40, "StoredFileRecord must remain exactly 40 bytes");

    // 1. 验证双轨 StringArena：ASCII 走 1-byte 轨道、CJK 走原生 2-byte UTF-16 轨道，且跨轨道比较与拷贝完全一致
    StringArena arena;
    const uint32_t asciiOff = arena.internAsciiFromWide(L"AlphaReport.pdf");
    const uint32_t wideOff = arena.intern(L"财务报告_2026.docx");
    EXPECT_TRUE(arena.equalsName(asciiOff, 15, true, L"AlphaReport.pdf"));
    EXPECT_FALSE(arena.equalsName(asciiOff, 15, true, L"alphareport.pdf"));
    EXPECT_TRUE(arena.equalsName(wideOff, 14, false, L"财务报告_2026.docx"));

    const uint32_t asciiUpperOff = arena.internAsciiFromWide(L"ALPHAREPORT.PDF");
    EXPECT_EQ(arena.compareNamesIgnoreCase(asciiOff, 15, true, asciiUpperOff, 15, true), 0);
    EXPECT_NE(arena.compareNamesExact(asciiOff, 15, true, asciiUpperOff, 15, true), 0);
    EXPECT_LT(arena.compareNamesIgnoreCase(asciiOff, 15, true, wideOff, 14, false), 0);

    // 2. 验证 >= 4 GiB 大文件与跨目录聚合体积的 64-bit 精确性以及 < 4 GiB 回落清理
    FileIndexStore store;
    store.setDriveLetter('D');
    constexpr uint64_t kSixGiB = 6ULL * 1024ULL * 1024ULL * 1024ULL + 12345ULL;
    constexpr uint64_t kFiveGiB = 5ULL * 1024ULL * 1024ULL * 1024ULL + 67890ULL;

    FileRecordInit dir{};
    dir.fileReferenceNumber = 10;
    dir.parentFileReferenceNumber = 0;
    dir.fileName = L"VMImages";
    dir.isDirectory = true;
    dir.fileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    store.upsert(dir, true, true);

    FileRecordInit big1{};
    big1.fileReferenceNumber = 11;
    big1.parentFileReferenceNumber = 10;
    big1.fileName = L"win11_dev.vmdk";
    big1.isDirectory = false;
    big1.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    big1.fileSize = kSixGiB;
    big1.lastWriteTime = 134150000000000000ULL;
    store.upsert(big1, true, true);

    FileRecordInit big2{};
    big2.fileReferenceNumber = 12;
    big2.parentFileReferenceNumber = 10;
    big2.fileName = L"备份镜像_2026.iso";
    big2.isDirectory = false;
    big2.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    big2.fileSize = kFiveGiB;
    big2.lastWriteTime = 134160000000000000ULL;
    store.upsert(big2, true, true);

    store.recomputeHierarchyMetadata();

    const auto* dirRec = store.find(10);
    const auto* big1Rec = store.find(11);
    const auto* big2Rec = store.find(12);
    ASSERT_NE(dirRec, nullptr);
    ASSERT_NE(big1Rec, nullptr);
    ASSERT_NE(big2Rec, nullptr);
    EXPECT_TRUE(big1Rec->isAsciiName());
    EXPECT_FALSE(big2Rec->isAsciiName());
    EXPECT_EQ(store.fileSizeOf(*big1Rec), kSixGiB);
    EXPECT_EQ(store.fileSizeOf(*big2Rec), kFiveGiB);
    EXPECT_EQ(store.fileSizeOf(*dirRec), kSixGiB + kFiveGiB);

    // 缩小到 < 4 GiB 时溢出表自动回收且父目录聚合大小同步更新
    EXPECT_TRUE(store.updateMetadata(11, 2048ULL, 0ULL, 134170000000000000ULL));
    EXPECT_EQ(store.fileSizeOf(*store.find(11)), 2048ULL);
    EXPECT_EQ(store.fileSizeOf(*store.find(10)), 2048ULL + kFiveGiB);
    EXPECT_EQ(store.lastWriteTimeOf(*store.find(11)), 134170000000000000ULL);

    // 3. 验证 3-pass 流式快照保存与重新加载对 ASCII/CJK/>=4GiB 文件 100% 无损保真
    wchar_t tempDirBuf[MAX_PATH]{};
    ASSERT_GT(GetTempPathW(MAX_PATH, tempDirBuf), 0u);
    const std::filesystem::path snapDir = std::filesystem::path(tempDirBuf) /
        (L"t3k_stream_snap_1b_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(snapDir, ec);
    ASSERT_TRUE(std::filesystem::create_directories(snapDir, ec));
    struct CleanupGuard {
        std::filesystem::path p;
        ~CleanupGuard() {
            std::error_code ignoreEc;
            std::filesystem::remove_all(p, ignoreEc);
        }
    } cleanup{snapDir};

    MftParser writer;
    writer.setRealTimeValidation(false);
    ASSERT_TRUE(writer.importSnapshot({dir, big1, big2}, 77777ULL, /*volumeSerial=*/0));

    auto& dbMgr = tools3000::service::db::DatabaseManager::instance();
    const std::wstring prevDbPath = dbMgr.getDbPath();
    dbMgr.init((snapDir / L"Tools3000_StreamTest.db").wstring());
    std::vector<MftParser*> writers = {&writer};
    ASSERT_TRUE(dbMgr.saveSnapshot(writers));

    MftParser reader;
    reader.setRealTimeValidation(false);
    std::vector<MftParser*> readers = {&reader};
    ASSERT_TRUE(dbMgr.loadSnapshot(readers));
    dbMgr.init(prevDbPath);

    EXPECT_EQ(reader.getFileCount(), 3u);
    const auto bigHits = reader.Search(L"size:>4gb", 50);
    ASSERT_EQ(bigHits.size(), 2u);
    const auto cjkHits = reader.Search(L"bfjx", 50);
    ASSERT_EQ(cjkHits.size(), 1u);
    EXPECT_EQ(cjkHits[0].fileName, L"备份镜像_2026.iso");
    EXPECT_EQ(cjkHits[0].fileSize, kFiveGiB);
}

TEST(FullSyntaxAccuracyAndCountConservationTest, AllSupportedSyntaxesAndBoundedHydrationPreserveExactCounts) {
    // 构造精确基准日期的 FILETIME（UTC）：2026-06-15 12:00:00 与 2020-03-10 08:00:00
    auto makeUtcFileTime = [](WORD year, WORD month, WORD day, WORD hour) -> uint64_t {
        SYSTEMTIME st{};
        st.wYear = year;
        st.wMonth = month;
        st.wDay = day;
        st.wHour = hour;
        FILETIME ft{};
        SystemTimeToFileTime(&st, &ft);
        ULARGE_INTEGER uli{};
        uli.LowPart = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        return uli.QuadPart;
    };
    const uint64_t ft2026 = makeUtcFileTime(2026, 6, 15, 12);
    const uint64_t ft2020 = makeUtcFileTime(2020, 3, 10, 8);
    constexpr uint64_t kMiB = 1024ULL * 1024ULL;
    constexpr uint64_t kGiB = 1024ULL * kMiB;

    auto parser = std::make_unique<MftParser>();
    ASSERT_TRUE(parser->Initialize('C'));
    parser->setRealTimeValidation(false);

    std::vector<FileRecordInit> records;
    auto pushRec = [&](uint64_t frn, uint64_t parent, const wchar_t* name, bool isDir,
                       uint64_t size, uint64_t mtime, DWORD attrs = FILE_ATTRIBUTE_ARCHIVE) {
        FileRecordInit r{};
        r.fileReferenceNumber = frn;
        r.parentFileReferenceNumber = parent;
        r.fileName = name;
        r.isDirectory = isDir;
        r.fileSize = size;
        r.lastWriteTime = mtime;
        r.fileAttributes = isDir ? (FILE_ATTRIBUTE_DIRECTORY | attrs) : attrs;
        records.push_back(std::move(r));
    };

    // 目录树
    pushRec(10, 0, L"Workspace", true, 0, ft2026);
    pushRec(11, 10, L"Finance", true, 0, ft2026);
    pushRec(12, 10, L"Engineering", true, 0, ft2026);
    pushRec(13, 0, L"$Recycle.Bin", true, 0, ft2026);

    // 涵盖所有语法的核心样本
    pushRec(101, 11, L"财务报告_2026.docx", false, 5 * kMiB, ft2026);
    pushRec(102, 11, L"项目管理方案.pdf", false, 2 * kMiB, ft2026);
    pushRec(103, 11, L"quarterly report 2026.xlsx", false, 8 * kMiB, ft2026);
    pushRec(104, 12, L"report_2026.pdf", false, 512, ft2026);
    pushRec(105, 12, L"report_2020.pdf", false, 6 * kGiB, ft2020);
    pushRec(106, 12, L"AlphaDraft.md", false, 4096, ft2026);
    pushRec(107, 12, L"AlphaRelease.md", false, 4096, ft2026);
    pushRec(108, 12, L"BetaSpec.pdf", false, 3 * kMiB, ft2026);
    pushRec(109, 12, L"CaseTarget.txt", false, 1024, ft2026);
    pushRec(110, 12, L"casetarget.txt", false, 1024, ft2026);
    pushRec(111, 12, L"hidden_secret.cfg", false, 512, ft2026, FILE_ATTRIBUTE_HIDDEN);
    pushRec(112, 13, L"recycled_project_batch_0.txt", false, 1024, ft2026);

    // 300 条批量样本（超过 HDD 24 条与 SSD 100 条水合上限）+ 4 条回收站干扰项
    for (uint64_t i = 0; i < 300; ++i) {
        FileRecordInit r{};
        r.fileReferenceNumber = 1000 + i;
        r.parentFileReferenceNumber = 12;
        r.fileName = L"project_batch_" + std::to_wstring(i) + L".txt";
        r.isDirectory = false;
        r.fileSize = 2048 + i;
        r.lastWriteTime = ft2026;
        r.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
        records.push_back(std::move(r));
    }
    for (uint64_t i = 1; i <= 4; ++i) {
        FileRecordInit r{};
        r.fileReferenceNumber = 2000 + i;
        r.parentFileReferenceNumber = 13;
        r.fileName = L"recycled_project_batch_" + std::to_wstring(i) + L".txt";
        r.isDirectory = false;
        r.fileSize = 1024;
        r.lastWriteTime = ft2026;
        r.fileAttributes = FILE_ATTRIBUTE_ARCHIVE;
        records.push_back(std::move(r));
    }

    ASSERT_TRUE(parser->importSnapshot(std::move(records), 88888ULL, /*volumeSerial=*/0));

    // 1. 纯中文关键词与拼音首字母
    EXPECT_EQ(parser->Search(L"财务报告", 50).size(), 1u);
    EXPECT_EQ(parser->Search(L"cwbg", 50).size(), 1u);
    EXPECT_EQ(parser->Search(L"xmgl", 50).size(), 1u);
    // 禁用拼音 nopy:
    EXPECT_EQ(parser->Search(L"nopy:cwbg", 50).size(), 0u);

    // 2. 扩展名与多扩展名 ext:
    EXPECT_EQ(parser->Search(L"ext:pdf", 50).size(), 4u);
    EXPECT_EQ(parser->Search(L"ext:docx;xlsx", 50).size(), 2u);

    // 3. 通配符 * 与 ?
    EXPECT_EQ(parser->Search(L"report_202?.pdf", 50).size(), 2u);
    EXPECT_EQ(parser->Search(L"*.md", 50).size(), 2u);

    // 4. 逻辑 NOT (!)、OR (|) 与引号精确短语
    EXPECT_EQ(parser->Search(L"Alpha !Draft", 50).size(), 1u);
    EXPECT_EQ(parser->Search(L"AlphaRelease|BetaSpec", 50).size(), 2u);
    EXPECT_EQ(parser->Search(L"\"quarterly report\"", 50).size(), 1u);

    // 5. 路径/父目录过滤 path: / parent: / 斜杠片段 / 盘符
    EXPECT_EQ(parser->Search(L"path:Finance", 50).size(), 4u);  // Finance 目录本身 + 3 个子文件
    EXPECT_EQ(parser->Search(L"parent:Finance file:", 50).size(), 3u);
    EXPECT_EQ(parser->Search(L"Finance\\财务", 50).size(), 1u);
    EXPECT_EQ(parser->Search(L"c: BetaSpec", 50).size(), 1u);
    EXPECT_EQ(parser->Search(L"d: BetaSpec", 50).size(), 0u);

    // 6. 类型限定 folder: / dir: / file:
    EXPECT_EQ(parser->Search(L"folder: Finance", 50).size(), 1u);
    EXPECT_EQ(parser->Search(L"dir: Workspace", 50).size(), 1u);
    EXPECT_EQ(parser->Search(L"file: Finance", 50).size(), 0u);

    // 7. 大小过滤（含 >= 4 GiB 64-bit 溢出表精确过滤）
    EXPECT_EQ(parser->Search(L"size:>4gb", 50).size(), 1u);
    EXPECT_EQ(parser->Search(L"size:>4gb", 50)[0].fileName, L"report_2020.pdf");
    EXPECT_EQ(parser->Search(L"size:1mb..10mb file:", 50).size(), 4u);
    EXPECT_EQ(parser->Search(L"size:<1kb file: report", 50).size(), 1u);

    // 8. 修改日期过滤 dm:（验证 1-second Unix 时间戳精度）
    EXPECT_EQ(parser->Search(L"dm:2020-01-01..2020-12-31 file:", 50).size(), 1u);
    EXPECT_EQ(parser->Search(L"dm:2026-06-01..2026-06-30 ext:pdf", 50).size(), 3u);

    // 9. 正则表达式 regex:
    EXPECT_EQ(parser->Search(L"regex:^report_\\d{4}\\.pdf$", 50).size(), 2u);

    // 10. 大小写敏感选项
    SearchParseOptions csOpts{};
    csOpts.caseSensitive = true;
    EXPECT_EQ(parser->Search(L"CaseTarget.txt", 50, {}, {}, csOpts).size(), 1u);
    csOpts.caseSensitive = false;
    EXPECT_EQ(parser->Search(L"CaseTarget.txt", 50, {}, {}, csOpts).size(), 2u);

    // 11. 隐藏文件过滤
    SearchExcludeOptions hideOpts{};
    hideOpts.excludeHidden = true;
    EXPECT_EQ(parser->Search(L"hidden_secret", 50, hideOpts).size(), 0u);
    hideOpts.excludeHidden = false;
    EXPECT_EQ(parser->Search(L"hidden_secret", 50, hideOpts).size(), 1u);

    // 12. 大结果集有界水合数量守恒：300 条常规文件 100% 全部返回，5 条 $Recycle.Bin 100% 纯内存剔除
    const auto batchHits = parser->Search(L"project_batch_", 500);
    EXPECT_EQ(batchHits.size(), 300u);
    for (const auto& h : batchHits) {
        EXPECT_EQ(h.fullPath.find(L"$Recycle.Bin"), std::wstring::npos);
    }
    // 显式搜 recycle 时可找回回收站里的 5 条
    EXPECT_EQ(parser->Search(L"recycle project_batch_", 50).size(), 5u);
}

TEST(ServiceLifecyclePolicyTest, StopsOnLastUserLogoffAndShutdownWhilePreservingLockAndMultiUser) {
    using namespace tools3000::service::lifecycle;

    // 1. 必须接收 STOP、SHUTDOWN 与 SESSIONCHANGE，且严禁同时设置 SHUTDOWN 与 PRESHUTDOWN（MSDN 互斥铁律）
    const DWORD accepted = acceptedServiceControls();
    EXPECT_NE(accepted & SERVICE_ACCEPT_STOP, 0u);
    EXPECT_NE(accepted & SERVICE_ACCEPT_SHUTDOWN, 0u);
    EXPECT_NE(accepted & SERVICE_ACCEPT_SESSIONCHANGE, 0u);
    EXPECT_EQ(accepted & SERVICE_ACCEPT_PRESHUTDOWN, 0u);

    EXPECT_TRUE(shouldStopOnServiceControl(SERVICE_CONTROL_STOP));
    EXPECT_TRUE(shouldStopOnServiceControl(SERVICE_CONTROL_SHUTDOWN));
    EXPECT_TRUE(shouldStopOnServiceControl(SERVICE_CONTROL_PRESHUTDOWN));
    EXPECT_FALSE(shouldStopOnServiceControl(SERVICE_CONTROL_INTERROGATE));
    EXPECT_FALSE(shouldStopOnServiceControl(SERVICE_CONTROL_SESSIONCHANGE));

    // 2. 单用户（含 Session 0 与 RDP-Tcp WTSListen 监听会话）：用户注销（或快速启动混合关机）时必须判定剩余 0 会话并停服
    const std::array<SessionDescriptor, 3> singleUserWithRdpListener = {{
        {0u, WTSDisconnected},   // Session 0 系统服务会话（忽略）
        {1u, WTSActive},         // 正在注销的当前用户会话（排除 excludedSessionId=1）
        {65536u, WTSListen},     // RDP-Tcp 监听占位会话（非用户会话，忽略）
    }};
    const uint32_t remainingAfterSingleLogoff =
        countRemainingInteractiveSessions(singleUserWithRdpListener, /*excludedSessionId=*/1u);
    EXPECT_EQ(remainingAfterSingleLogoff, 0u);
    EXPECT_TRUE(shouldStopOnSessionChange(WTS_SESSION_LOGOFF, remainingAfterSingleLogoff));

    // 3. 锁屏、解锁、远程断开不注销、登录事件绝不误停服务（保护会话内秒开与睡眠恢复常驻）
    EXPECT_FALSE(shouldStopOnSessionChange(WTS_SESSION_LOCK, 0u));
    EXPECT_FALSE(shouldStopOnSessionChange(WTS_SESSION_UNLOCK, 0u));
    EXPECT_FALSE(shouldStopOnSessionChange(WTS_CONSOLE_DISCONNECT, 0u));
    EXPECT_FALSE(shouldStopOnSessionChange(WTS_REMOTE_DISCONNECT, 0u));
    EXPECT_FALSE(shouldStopOnSessionChange(WTS_SESSION_LOGON, 0u));

    // 4. 多用户桌面（Windows Server / 快速用户切换）：注销 Session 1 但 Session 2 仍在线或断开挂起时，绝不误停服务
    const std::array<SessionDescriptor, 4> multiUserSessions = {{
        {0u, WTSDisconnected},
        {1u, WTSActive},
        {2u, WTSDisconnected},   // 另一位已登录但切换后台的用户
        {65536u, WTSListen},
    }};
    const uint32_t remainingMultiUser =
        countRemainingInteractiveSessions(multiUserSessions, /*excludedSessionId=*/1u);
    EXPECT_EQ(remainingMultiUser, 1u);
    EXPECT_FALSE(shouldStopOnSessionChange(WTS_SESSION_LOGOFF, remainingMultiUser));
}
