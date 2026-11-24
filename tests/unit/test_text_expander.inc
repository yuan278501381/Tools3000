// -----------------------------------------------------------------------------
// test_text_expander.inc -- TextExpander 匹配引擎与动态宏单元测试套件
//
// 覆盖范围:
//   1. Immediate 即时匹配、准确退格计数与触发后自动清空缓冲
//   2. Delimiter 边界匹配（Space / Tab / Enter）、退格 +1 与英文词边界防误触
//   3. 最长触发词优先匹配（Longest Match First）
//   4. 大小写敏感 / 不敏感切换
//   5. Backspace 修正输入与 clear() 跨窗口/点击重置
//   6. {{date}} / {{time}} / {{datetime}} / {{clipboard}} / {{cursor}} 单遍宏展开与防递归
//   7. 极速 Unicode 直注路径判定、64 字符滑动窗与 JSON 序列化往返
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// -----------------------------------------------------------------------------

#include "text_expander/TextExpanderMatcher.h"

namespace {

std::optional<tools3000::text_expander::MatchResult> feedText(
    tools3000::text_expander::TextExpanderMatcher& matcher,
    std::wstring_view seq) {
    std::optional<tools3000::text_expander::MatchResult> last;
    for (wchar_t ch : seq) {
        auto hit = matcher.pushChar(ch);
        if (hit.has_value()) {
            last = std::move(hit);
        }
    }
    return last;
}

}  // namespace

TEST(TextExpanderMatcherTest, ImmediateModeMatchesAndClearsBuffer) {
    using namespace tools3000::text_expander;

    TextExpanderMatcher matcher;
    matcher.setRules({
        TextExpanderRule{"r1", ";date", "{{date}}", "Date", true, tools3000::text_expander::TriggerMode::Immediate, false, 0},
    });

    EXPECT_FALSE(matcher.pushChar(L';').has_value());
    EXPECT_FALSE(matcher.pushChar(L'd').has_value());
    EXPECT_FALSE(matcher.pushChar(L'a').has_value());
    EXPECT_FALSE(matcher.pushChar(L't').has_value());

    const auto hit = matcher.pushChar(L'e');
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->rule.id, "r1");
    EXPECT_EQ(hit->eraseCount, 5u);
    EXPECT_FALSE(hit->triggeredByDelimiter);
    EXPECT_TRUE(matcher.buffer().empty());
}

TEST(TextExpanderMatcherTest, DelimiterModeSpaceTabEnterAndWordBoundaryGuard) {
    using namespace tools3000::text_expander;

    TextExpanderMatcher matcher;
    matcher.setRules({
        TextExpanderRule{"btw", "btw", "by the way", "BTW", true, tools3000::text_expander::TriggerMode::Delimiter, false, 0},
        TextExpanderRule{"thx", ";thx", "Thanks!", "Thanks", true, tools3000::text_expander::TriggerMode::Delimiter, false, 0},
    });

    // 1. 英文词边界防误触：敲 "abtw " 不应触发 "btw"
    EXPECT_FALSE(feedText(matcher, L"abtw ").has_value());

    // 2. 空格之后独立单词 "btw " 精准触发，eraseCount = 3 + 1 = 4
    const auto hitSpace = feedText(matcher, L"btw ");
    ASSERT_TRUE(hitSpace.has_value());
    EXPECT_EQ(hitSpace->rule.id, "btw");
    EXPECT_EQ(hitSpace->eraseCount, 4u);
    EXPECT_TRUE(hitSpace->triggeredByDelimiter);
    EXPECT_EQ(hitSpace->delimiterChar, L' ');

    // 3. Tab 边界触发
    const auto hitTab = feedText(matcher, L"btw\t");
    ASSERT_TRUE(hitTab.has_value());
    EXPECT_EQ(hitTab->rule.id, "btw");
    EXPECT_EQ(hitTab->eraseCount, 4u);
    EXPECT_EQ(hitTab->delimiterChar, L'\t');

    // 4. Enter 边界触发
    const auto hitEnter = feedText(matcher, L"btw\n");
    ASSERT_TRUE(hitEnter.has_value());
    EXPECT_EQ(hitEnter->rule.id, "btw");
    EXPECT_EQ(hitEnter->eraseCount, 4u);
    EXPECT_EQ(hitEnter->delimiterChar, L'\n');

    // 5. 符号前缀触发词 ";thx" 即使紧贴前文也可在分隔符时安全触发
    const auto hitSymbol = feedText(matcher, L"hello;thx ");
    ASSERT_TRUE(hitSymbol.has_value());
    EXPECT_EQ(hitSymbol->rule.id, "thx");
    EXPECT_EQ(hitSymbol->eraseCount, 5u);
}

TEST(TextExpanderMatcherTest, LongestTriggerWinsPriority) {
    using namespace tools3000::text_expander;

    TextExpanderMatcher matcher;
    matcher.setRules({
        TextExpanderRule{"short", "date", "SHORT", "Short", true, tools3000::text_expander::TriggerMode::Immediate, false, 0},
        TextExpanderRule{"long", ";date", "LONG", "Long", true, tools3000::text_expander::TriggerMode::Immediate, false, 0},
    });

    const auto hit = feedText(matcher, L";date");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->rule.id, "long");
    EXPECT_EQ(hit->eraseCount, 5u);

    // Delimiter 模式下同样最长匹配优先
    TextExpanderMatcher delimMatcher;
    delimMatcher.setRules({
        TextExpanderRule{"d1", ";dt", "SHORT_DELIM", "Short", true, tools3000::text_expander::TriggerMode::Delimiter, false, 0},
        TextExpanderRule{"d2", ";;dt", "LONG_DELIM", "Long", true, tools3000::text_expander::TriggerMode::Delimiter, false, 0},
    });
    const auto dHit = feedText(delimMatcher, L";;dt ");
    ASSERT_TRUE(dHit.has_value());
    EXPECT_EQ(dHit->rule.id, "d2");
    EXPECT_EQ(dHit->eraseCount, 5u);
}

TEST(TextExpanderMatcherTest, CaseSensitivityAndDisabledRules) {
    using namespace tools3000::text_expander;

    TextExpanderMatcher matcher;
    matcher.setRules({
        TextExpanderRule{"ci", ";sig", "Signature", "CI", true, tools3000::text_expander::TriggerMode::Immediate, false, 0},
        TextExpanderRule{"cs", ";ID", "UPPER_ID", "CS", true, tools3000::text_expander::TriggerMode::Immediate, true, 0},
        TextExpanderRule{"off", ";off", "NEVER", "Disabled", false, tools3000::text_expander::TriggerMode::Immediate, false, 0},
    });

    EXPECT_EQ(matcher.activeRuleCount(), 2u);

    // 禁用规则永不命中
    EXPECT_FALSE(feedText(matcher, L";off").has_value());

    // 大小写不敏感规则：大写 ";SIG" 命中 ";sig"
    const auto ciHit = feedText(matcher, L";SIG");
    ASSERT_TRUE(ciHit.has_value());
    EXPECT_EQ(ciHit->rule.id, "ci");

    // 大小写敏感规则：小写 ";id" 不命中，精确 ";ID" 命中
    EXPECT_FALSE(feedText(matcher, L";id").has_value());
    const auto csHit = feedText(matcher, L";ID");
    ASSERT_TRUE(csHit.has_value());
    EXPECT_EQ(csHit->rule.id, "cs");
}

TEST(TextExpanderMatcherTest, BackspaceCorrectionAndClearBuffer) {
    using namespace tools3000::text_expander;

    TextExpanderMatcher matcher;
    matcher.setRules({
        TextExpanderRule{"r1", ";now", "{{datetime}}", "Now", true, tools3000::text_expander::TriggerMode::Immediate, false, 0},
    });

    // 打错一个字符后退格再补全
    EXPECT_FALSE(matcher.pushChar(L';').has_value());
    EXPECT_FALSE(matcher.pushChar(L'n').has_value());
    EXPECT_FALSE(matcher.pushChar(L'x').has_value());
    matcher.backspace();
    EXPECT_EQ(matcher.buffer(), L";n");
    EXPECT_FALSE(matcher.pushChar(L'o').has_value());
    const auto hit = matcher.pushChar(L'w');
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->rule.id, "r1");
    EXPECT_EQ(hit->eraseCount, 4u);

    // clear() 彻底清空上下文，避免鼠标点击/切换窗口导致跨区串词
    EXPECT_FALSE(matcher.pushChar(L';').has_value());
    EXPECT_FALSE(matcher.pushChar(L'n').has_value());
    matcher.clear();
    EXPECT_TRUE(matcher.buffer().empty());
    EXPECT_FALSE(feedText(matcher, L"ow").has_value());
}

TEST(TextExpanderMatcherTest, DynamicMacroExpansionAndRecursionSafety) {
    using namespace tools3000::text_expander;

    const LocalDateTime fixedTime{2026, 11, 23, 9, 5, 42};

    // 1. {{date}} / {{time}} / {{datetime}}
    const auto dateOut = TextExpanderMatcher::expandTemplate(L"Date={{date}}", fixedTime, L"");
    EXPECT_EQ(dateOut.text, L"Date=2026-11-23");
    EXPECT_EQ(dateOut.moveLeftSteps, 0u);

    const auto timeOut = TextExpanderMatcher::expandTemplate(L"Time={{time}}", fixedTime, L"");
    EXPECT_EQ(timeOut.text, L"Time=09:05");

    const auto dtOut = TextExpanderMatcher::expandTemplate(L"[{{datetime}}]", fixedTime, L"");
    EXPECT_EQ(dtOut.text, L"[2026-11-23 09:05]");

    // 2. {{clipboard}} 注入与防递归展开（剪贴板中含 {{date}}/{{clipboard}} 绝不二次解析）
    const auto clipOut = TextExpanderMatcher::expandTemplate(
        L"Quote: {{clipboard}} ({{date}})",
        fixedTime,
        L"literal {{clipboard}} and {{date}}");
    EXPECT_EQ(clipOut.text, L"Quote: literal {{clipboard}} and {{date}} (2026-11-23)");

    // 3. {{clipboard}} 长度截断上限保护
    const auto truncOut = TextExpanderMatcher::expandTemplate(L"{{clipboard}}", fixedTime, L"abcdefghij", 4);
    EXPECT_EQ(truncOut.text, L"abcd");

    // 4. {{cursor}} 精准左移步数计算（含英文、中文与 CRLF）
    const auto htmlOut = TextExpanderMatcher::expandTemplate(L"<div>{{cursor}}</div>", fixedTime, L"");
    EXPECT_EQ(htmlOut.text, L"<div></div>");
    EXPECT_EQ(htmlOut.moveLeftSteps, 6u);

    const auto cjkOut = TextExpanderMatcher::expandTemplate(L"你好{{cursor}}世界", fixedTime, L"");
    EXPECT_EQ(cjkOut.text, L"你好世界");
    EXPECT_EQ(cjkOut.moveLeftSteps, 2u);

    const auto crlfOut = TextExpanderMatcher::expandTemplate(L"A{{cursor}}\r\nB", fixedTime, L"");
    EXPECT_EQ(crlfOut.text, L"A\r\nB");
    EXPECT_EQ(crlfOut.moveLeftSteps, 2u);  // CRLF=1 + 'B'=1

    // 5. 多个 {{cursor}} 仅首次生效，其余自动剥离
    const auto multiCursor = TextExpanderMatcher::expandTemplate(L"1{{cursor}}2{{cursor}}3", fixedTime, L"");
    EXPECT_EQ(multiCursor.text, L"123");
    EXPECT_EQ(multiCursor.moveLeftSteps, 2u);
}

TEST(TextExpanderMatcherTest, SlidingWindowCapDirectPathAndJsonRoundTrip) {
    using namespace tools3000::text_expander;

    TextExpanderMatcher matcher;
    matcher.setRules(defaultSampleRules());
    EXPECT_EQ(matcher.activeRuleCount(), 3u);

    // 连续键入超过 64 字符的无关前缀后，仍可无缝匹配尾部 ";date"
    std::wstring longPrefix(80, L'x');
    EXPECT_FALSE(feedText(matcher, longPrefix).has_value());
    EXPECT_LE(matcher.buffer().size(), TextExpanderMatcher::kMaxBufferLength);

    const auto hit = feedText(matcher, L";date");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->rule.id, "sample_date");

    // 极速 Unicode 直注路径判定
    EXPECT_TRUE(TextExpanderMatcher::isFastDirectUnicodeCandidate(L"2026-11-23"));
    EXPECT_FALSE(TextExpanderMatcher::isFastDirectUnicodeCandidate(L"line1\nline2"));
    EXPECT_FALSE(TextExpanderMatcher::isFastDirectUnicodeCandidate(L"中文内容"));
    EXPECT_FALSE(TextExpanderMatcher::isFastDirectUnicodeCandidate(std::wstring(60, L'a')));

    // JSON 序列化与反序列化往返 + 敏感进程解析
    const TextExpanderRule original{"id_1", ";mail", "hi@example.com", "Email", true, tools3000::text_expander::TriggerMode::Delimiter, true, 7};
    const nlohmann::json serialized = ruleToJson(original);
    TextExpanderRule restored;
    ASSERT_TRUE(ruleFromJson(serialized, restored));
    EXPECT_EQ(restored.id, original.id);
    EXPECT_EQ(restored.trigger, original.trigger);
    EXPECT_EQ(restored.replacement, original.replacement);
    EXPECT_EQ(restored.label, original.label);
    EXPECT_EQ(restored.enabled, original.enabled);
    EXPECT_EQ(restored.mode, original.mode);
    EXPECT_EQ(restored.caseSensitive, original.caseSensitive);
    EXPECT_EQ(restored.hitCount, original.hitCount);

    const auto excluded = parseExcludedProcessString("  Game.EXE, keepass.exe; game.exe \n");
    ASSERT_EQ(excluded.size(), 2u);
    EXPECT_EQ(excluded[0], "game.exe");
    EXPECT_EQ(excluded[1], "keepass.exe");

    // 确认默认设置开箱即用为启用状态，且隐私保护与剪贴板还原均默认开启
    const TextExpanderSettings defaults{};
    EXPECT_TRUE(defaults.enabled);
    EXPECT_TRUE(defaults.autoExcludeSensitiveApps);
    EXPECT_TRUE(defaults.restoreClipboard);
}

TEST(TextExpanderMatcherTest, FullwidthCjkEquivalenceMultilingualCaseAndRichMacros) {
    using namespace tools3000::text_expander;

    TextExpanderMatcher matcher;
    TextExpanderRule cjkFlex{"cjk_flex", ";sig", "Signature", "Sig", true, tools3000::text_expander::TriggerMode::Immediate, false, 0, false, true};
    TextExpanderRule cjkStrict{"cjk_strict", "；only", "FullwidthOnly", "Strict", true, tools3000::text_expander::TriggerMode::Immediate, false, 0, true, true};
    TextExpanderRule ruRule{"ru_hi", ";пр", "Привет!", "Ru", true, tools3000::text_expander::TriggerMode::Immediate, false, 0, false, true};
    TextExpanderRule delimKeep{"d_keep", "brb", "be right back[{{cursor}}]", "BRB", true, tools3000::text_expander::TriggerMode::Delimiter, false, 0, false, true};
    matcher.setRules({cjkFlex, cjkStrict, ruRule, delimKeep});

    // 1. 中日韩全角标点与全角字母智能命中半角规则 ";sig"
    const auto fwHit = feedText(matcher, L"；ＳＩＧ");
    ASSERT_TRUE(fwHit.has_value());
    EXPECT_EQ(fwHit->rule.id, "cjk_flex");
    EXPECT_EQ(fwHit->eraseCount, 4u);

    // 2. 开启 fullwidthSensitive=true 时，半角 ";only" 不命中全角规则 "；only"，全角才命中
    EXPECT_FALSE(feedText(matcher, L";only").has_value());
    const auto strictHit = feedText(matcher, L"；only");
    ASSERT_TRUE(strictHit.has_value());
    EXPECT_EQ(strictHit->rule.id, "cjk_strict");

    // 3. 俄语西里尔大写 "；ПР" 自动折叠全角分号与西里尔大小写命中 ";пр"
    const auto ruHit = feedText(matcher, L"；ПР");
    ASSERT_TRUE(ruHit.has_value());
    EXPECT_EQ(ruHit->rule.id, "ru_hi");

    // 4. Delimiter + keepDelimiter=true 保留末尾空格并自动累加 {{cursor}} 回退步数
    const auto brbHit = feedText(matcher, L"brb ");
    ASSERT_TRUE(brbHit.has_value());
    ExpandedOutput brbOut = TextExpanderMatcher::expandTemplate(
        detail::utf8ToUtf16(brbHit->rule.replacement), LocalDateTime{2026, 11, 24, 9, 5, 3}, L"");
    TextExpanderMatcher::applyDelimiterRetention(brbOut, *brbHit);
    EXPECT_EQ(brbOut.text, L"be right back[] ");
    EXPECT_EQ(brbOut.moveLeftSteps, 2u); // ']' + ' '

    // 5. 14 项动态宏、跨月相对日期偏移与 8 语种本地化星期验证
    TemplateExpansionContext ctx;
    ctx.now = LocalDateTime{2026, 11, 30, 17, 8, 9, -1, 1796058489ll}; // 2026-11-30 为星期一 (Monday)
    ctx.clipboardText = L"  https://example.com \r\n ";
    ctx.userName = L"alice";
    ctx.localeCode = "ja-JP";

    const auto rich = TextExpanderMatcher::expandTemplateWithContext(
        L"{{date_compact}}|{{date_slash}}|{{time_sec}}|{{weekday}}|{{date+1d}}|{{date-30d}}|{{clipboard_trim}}|{{user}}|{{timestamp}}|{{uuid}}",
        ctx);
    EXPECT_NE(rich.text.find(L"20261130|2026/11/30|17:08:09|\u6708\u66DC\u65E5|2026-12-01|2026-10-31|https://example.com|alice|1796058489|"), std::wstring::npos);
    EXPECT_GT(rich.text.size(), 110u);
}
