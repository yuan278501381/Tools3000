// ─────────────────────────────────────────────────────────────────────────────
// core/pinyin.rs — 零依赖轻量中文拼音首字母与多词模糊子序列搜索引擎
//                  基于 GB2312 一级字库严格拼音序区间 + 高频多音字/二级字精确表
//                  支持 "wx" -> "微信"、"sz" -> "设置"、"vsc" -> "Visual Studio Code"
// ─────────────────────────────────────────────────────────────────────────────

use windows::Win32::Globalization::WideCharToMultiByte;

/// GB2312 一级汉字（3755 常用简体汉字）23 组拼音首字母严格起始编码表
const GB2312_INITIAL_BOUNDS: &[(u16, char)] = &[
    (0xB0A1, 'a'),
    (0xB0C5, 'b'),
    (0xB2C1, 'c'),
    (0xB4EE, 'd'),
    (0xB6EA, 'e'),
    (0xB7A2, 'f'),
    (0xB8C1, 'g'),
    (0xB9FE, 'h'),
    (0xBBF7, 'j'),
    (0xBFA6, 'k'),
    (0xC0AC, 'l'),
    (0xC2E8, 'm'),
    (0xC4C3, 'n'),
    (0xC5B6, 'o'),
    (0xC5BE, 'p'),
    (0xC6DA, 'q'),
    (0xC8BB, 'r'),
    (0xC8F6, 's'),
    (0xCBFA, 't'),
    (0xCDDA, 'w'),
    (0xCEF4, 'x'),
    (0xD1B9, 'y'),
    (0xD4D1, 'z'),
];

/// 高频桌面软件标题汉字、多音字与 GB2312 二级常见字精确映射表（支持双读音首字母）
const SPECIAL_HANZI_INITIALS: &[(char, &'static str)] = &[
    ('微', "w"),
    ('信', "x"),
    ('设', "s"),
    ('置', "z"),
    ('浏', "l"),
    ('览', "l"),
    ('器', "q"),
    ('终', "z"),
    ('端', "d"),
    ('文', "w"),
    ('件', "j"),
    ('管', "g"),
    ('理', "l"),
    ('资', "z"),
    ('源', "y"),
    ('网', "w"),
    ('易', "y"),
    ('云', "y"),
    ('音', "y"),
    ('乐', "yl"),
    ('钉', "d"),
    ('飞', "f"),
    ('书', "s"),
    ('腾', "t"),
    ('讯', "x"),
    ('会', "hk"),
    ('议', "y"),
    ('截', "j"),
    ('图', "t"),
    ('录', "l"),
    ('屏', "p"),
    ('记', "j"),
    ('事', "s"),
    ('本', "b"),
    ('画', "h"),
    ('计', "j"),
    ('算', "s"),
    ('机', "j"),
    ('控', "k"),
    ('制', "z"),
    ('台', "t"),
    ('命', "m"),
    ('令', "l"),
    ('提', "t"),
    ('示', "s"),
    ('符', "f"),
    ('搜', "s"),
    ('索', "s"),
    ('剪', "j"),
    ('贴', "t"),
    ('板', "b"),
    ('桌', "z"),
    ('面', "m"),
    ('窗', "c"),
    ('口', "k"),
    ('工', "g"),
    ('具', "j"),
    ('箱', "x"),
    ('编', "b"),
    ('辑', "j"),
    ('代', "d"),
    ('码', "m"),
    ('查', "c"),
    ('看', "k"),
    ('播', "b"),
    ('放', "f"),
    ('下', "x"),
    ('载', "z"),
    ('打', "d"),
    ('印', "y"),
    ('邮', "y"),
    ('日', "r"),
    ('历', "l"),
    ('天', "t"),
    ('气', "q"),
    ('商', "s"),
    ('店', "d"),
    ('相', "x"),
    ('册', "c"),
    ('视', "s"),
    ('频', "p"),
    ('片', "p"),
    ('聊', "l"),
    ('游', "y"),
    ('戏', "x"),
    ('办', "b"),
    ('公', "g"),
    ('表', "b"),
    ('格', "g"),
    ('演', "y"),
    ('档', "d"),
    ('企', "q"),
    ('业', "y"),
    ('百', "b"),
    ('度', "d"),
    ('抖', "d"),
    ('小', "x"),
    ('红', "h"),
    ('知', "z"),
    ('乎', "h"),
    ('淘', "t"),
    ('宝', "b"),
    ('京', "j"),
    ('东', "d"),
    ('哔', "b"),
    ('哩', "l"),
    ('酷', "k"),
    ('狗', "g"),
    ('炫', "x"),
    ('喵', "m"),
    ('重', "cz"),
    ('行', "xh"),
    ('长', "cz"),
    ('调', "td"),
    ('藏', "cz"),
    ('传', "cz"),
    ('解', "jx"),
    ('数', "s"),
    ('据', "j"),
    ('库', "k"),
    ('系', "xj"),
    ('统', "t"),
    ('安', "a"),
    ('全', "q"),
    ('更', "g"),
    ('新', "x"),
    ('任', "r"),
    ('务', "w"),
    ('快', "k"),
    ('捷', "j"),
    ('键', "j"),
    ('鼠', "s"),
    ('标', "b"),
    ('手', "s"),
    ('势', "s"),
    ('翻', "f"),
    ('译', "y"),
    ('词', "c"),
    ('典', "d"),
    ('笔', "b"),
    ('电', "d"),
    ('子', "z"),
    ('脑', "n"),
    ('远', "y"),
    ('程', "c"),
    ('连', "l"),
    ('接', "j"),
    ('服', "f"),
    ('器', "q"),
    ('发', "f"),
    ('布', "b"),
    ('测', "c"),
    ('试', "s"),
    ('预', "y"),
    ('览', "l"),
    ('分', "f"),
    ('区', "q"),
    ('置', "z"),
    ('顶', "d"),
    ('透', "t"),
    ('明', "m"),
    ('焦', "j"),
    ('点', "d"),
    ('边', "b"),
    ('框', "k"),
    ('切', "q"),
    ('换', "h"),
    ('作', "z"),
    ('空', "k"),
    ('间', "j"),
    ('显', "x"),
    ('器', "q"),
    ('托', "t"),
    ('盘', "p"),
];

/// 查询单个汉字的拼音首字母集合（首选 + 可选多音备选）
pub fn hanzi_initials(ch: char) -> &'static str {
    for &(k, v) in SPECIAL_HANZI_INITIALS {
        if k == ch {
            return v;
        }
    }
    if !('\u{4E00}'..='\u{9FFF}').contains(&ch) {
        return "";
    }
    if let Some(gb_code) = char_to_gb2312_code(ch) {
        if let Some(initial) = gb2312_code_to_initial(gb_code) {
            return static_letter_str(initial);
        }
    }
    ""
}

fn char_to_gb2312_code(ch: char) -> Option<u16> {
    let mut utf16 = [0u16; 2];
    let slice = ch.encode_utf16(&mut utf16);
    if slice.len() != 1 {
        return None;
    }
    let mut out = [0u8; 4];
    let written = unsafe {
        WideCharToMultiByte(936, 0, slice, Some(&mut out), None, None)
    };
    if written == 2 {
        let code = ((out[0] as u16) << 8) | (out[1] as u16);
        Some(code)
    } else {
        None
    }
}

fn gb2312_code_to_initial(code: u16) -> Option<char> {
    if !(0xB0A1..=0xD7F9).contains(&code) {
        return None;
    }
    let mut best = None;
    for &(bound, letter) in GB2312_INITIAL_BOUNDS {
        if code >= bound {
            best = Some(letter);
        } else {
            break;
        }
    }
    best
}

fn static_letter_str(c: char) -> &'static str {
    match c {
        'a' => "a",
        'b' => "b",
        'c' => "c",
        'd' => "d",
        'e' => "e",
        'f' => "f",
        'g' => "g",
        'h' => "h",
        'j' => "j",
        'k' => "k",
        'l' => "l",
        'm' => "m",
        'n' => "n",
        'o' => "o",
        'p' => "p",
        'q' => "q",
        'r' => "r",
        's' => "s",
        't' => "t",
        'w' => "w",
        'x' => "x",
        'y' => "y",
        'z' => "z",
        _ => "",
    }
}

/// 将任意标题提取为拼音首字母 + ASCII 小写字母序列（以及英文词首字母缩写）
pub fn build_pinyin_initials(text: &str) -> (String, String) {
    let mut primary = String::with_capacity(text.len());
    let mut alternate = String::with_capacity(text.len());
    for ch in text.chars() {
        if ch.is_ascii_alphanumeric() {
            let low = ch.to_ascii_lowercase();
            primary.push(low);
            alternate.push(low);
        } else {
            let initials = hanzi_initials(ch);
            if let Some(first) = initials.chars().next() {
                primary.push(first);
                let second = initials.chars().nth(1).unwrap_or(first);
                alternate.push(second);
            }
        }
    }
    (primary, alternate)
}

/// 提取英文单词首字母缩写（例如 "Visual Studio Code" -> "vsc"）
pub fn build_acronym(text: &str) -> String {
    let mut out = String::new();
    let mut prev_is_sep = true;
    let mut prev_was_lower = false;
    for ch in text.chars() {
        if ch.is_ascii_alphanumeric() {
            let is_upper = ch.is_ascii_uppercase();
            if prev_is_sep || (is_upper && prev_was_lower) {
                out.push(ch.to_ascii_lowercase());
            }
            prev_is_sep = false;
            prev_was_lower = ch.is_ascii_lowercase();
        } else {
            let initials = hanzi_initials(ch);
            if let Some(first) = initials.chars().next() {
                out.push(first);
            }
            prev_is_sep = true;
            prev_was_lower = false;
        }
    }
    out
}

/// 计算查询词对窗口标题与进程名的综合相关性得分（None 表示不匹配）
pub fn score_window_match(
    query: &str,
    title: &str,
    process_name: &str,
    enable_pinyin: bool,
) -> Option<i32> {
    let trimmed = query.trim();
    if trimmed.is_empty() {
        return Some(0);
    }

    // 支持空格分词的多条件 AND 检索
    let mut total_score = 0i32;
    for token in trimmed.split_whitespace() {
        let token_lower = token.to_ascii_lowercase();
        let title_lower = title.to_lowercase();
        let proc_lower = process_name.to_lowercase();
        let proc_stem = proc_lower.strip_suffix(".exe").unwrap_or(&proc_lower);

        let mut best_token_score: Option<i32> = None;
        let mut consider = |score: i32| {
            best_token_score = Some(match best_token_score {
                Some(prev) => prev.max(score),
                None => score,
            });
        };

        // 1. 精确/前缀/子串直接命中（标题与进程名）
        if title_lower == token_lower || proc_stem == token_lower {
            consider(1000);
        } else if title_lower.starts_with(&token_lower) {
            consider(860);
        } else if proc_stem.starts_with(&token_lower) {
            consider(820);
        } else if let Some(pos) = title_lower.find(&token_lower) {
            consider(720 - (pos.min(200) as i32));
        } else if let Some(pos) = proc_stem.find(&token_lower) {
            consider(680 - (pos.min(200) as i32));
        }

        // 2. 英文词首缩写匹配（如 "vsc" -> "Visual Studio Code"）
        let acronym = build_acronym(title);
        if !acronym.is_empty() {
            if acronym.starts_with(&token_lower) {
                consider(840);
            } else if acronym.contains(&token_lower) {
                consider(700);
            }
        }

        // 3. 中文拼音首字母匹配（如 "wx" -> "微信"、"sz" -> "设置"）
        if enable_pinyin && token_lower.is_ascii() {
            let (py_primary, py_alt) = build_pinyin_initials(title);
            for candidate in [&py_primary, &py_alt] {
                if candidate.starts_with(&token_lower) {
                    consider(850);
                } else if let Some(pos) = candidate.find(&token_lower) {
                    consider(710 - (pos.min(200) as i32));
                } else if let Some(sub_score) = fuzzy_subsequence_score(&token_lower, candidate) {
                    consider(520 + sub_score);
                }
            }
        }

        // 4. 通用字符模糊子序列匹配
        if let Some(sub_score) = fuzzy_subsequence_score(&token_lower, &title_lower) {
            consider(460 + sub_score);
        }
        if let Some(sub_score) = fuzzy_subsequence_score(&token_lower, proc_stem) {
            consider(430 + sub_score);
        }

        total_score += best_token_score?;
    }

    Some(total_score)
}

/// 计算有序模糊子序列匹配分（偏好连续命中与靠前命中）
pub fn fuzzy_subsequence_score(needle: &str, haystack: &str) -> Option<i32> {
    let n_chars: Vec<char> = needle.chars().collect();
    if n_chars.is_empty() {
        return Some(0);
    }
    let mut ni = 0usize;
    let mut score = 0i32;
    let mut consecutive = 0i32;
    for (hi, hc) in haystack.chars().enumerate() {
        if hc == n_chars[ni] {
            consecutive += 1;
            score += 12 + consecutive * 6;
            if hi == 0 {
                score += 18;
            }
            ni += 1;
            if ni == n_chars.len() {
                // 越紧凑的匹配分越高
                let penalty = ((hi + 1).saturating_sub(n_chars.len()).min(80)) as i32;
                return Some((score - penalty).max(1));
            }
        } else {
            consecutive = 0;
        }
    }
    None
}

/// 定位搜索词在窗口标题中的高亮区间（UTF-16 字符索引 `(start, len)`），
/// 同时支持原文子串与中文拼音首字母命中定位，供 DirectWrite TextLayout 精准着重渲染！
#[allow(dead_code)]
pub fn find_title_highlight_utf16_ranges(
    query: &str,
    title: &str,
    enable_pinyin: bool,
) -> Vec<(u32, u32)> {
    let trimmed = query.trim();
    if trimmed.is_empty() || title.is_empty() {
        return Vec::new();
    }

    // 预构建每个字符对应的 UTF-16 起始偏移、长度与小写/拼音首字母视图
    struct CharSlot {
        utf16_start: u32,
        utf16_len: u32,
        lower: char,
        py_primary: Option<char>,
        py_alt: Option<char>,
    }

    let mut slots = Vec::with_capacity(title.chars().count());
    let mut u16_pos = 0u32;
    for ch in title.chars() {
        let u16_len = ch.len_utf16() as u32;
        let lower = ch.to_lowercase().next().unwrap_or(ch);
        let (py1, py2) = if ch.is_ascii_alphanumeric() {
            let l = ch.to_ascii_lowercase();
            (Some(l), Some(l))
        } else {
            let init = hanzi_initials(ch);
            let mut it = init.chars();
            let first = it.next();
            let second = it.next().or(first);
            (first, second)
        };
        slots.push(CharSlot {
            utf16_start: u16_pos,
            utf16_len: u16_len,
            lower,
            py_primary: py1,
            py_alt: py2,
        });
        u16_pos += u16_len;
    }

    let mut marked = vec![false; slots.len()];

    for token in trimmed.split_whitespace() {
        let tok_chars: Vec<char> = token.to_lowercase().chars().collect();
        if tok_chars.is_empty() {
            continue;
        }
        let mut matched_token = false;

        // 1. 原文连续子串匹配
        if tok_chars.len() <= slots.len() {
            for i in 0..=(slots.len() - tok_chars.len()) {
                if (0..tok_chars.len()).all(|j| slots[i + j].lower == tok_chars[j]) {
                    for j in 0..tok_chars.len() {
                        marked[i + j] = true;
                    }
                    matched_token = true;
                    break;
                }
            }
        }

        // 2. 中文拼音首字母连续子串匹配（忽略标题中的空格与标点）
        if !matched_token && enable_pinyin && token.is_ascii() {
            let py_slots: Vec<(usize, char, char)> = slots
                .iter()
                .enumerate()
                .filter_map(|(idx, s)| Some((idx, s.py_primary?, s.py_alt?)))
                .collect();
            if tok_chars.len() <= py_slots.len() {
                for i in 0..=(py_slots.len() - tok_chars.len()) {
                    let ok = (0..tok_chars.len()).all(|j| {
                        let (_, p1, p2) = py_slots[i + j];
                        p1 == tok_chars[j] || p2 == tok_chars[j]
                    });
                    if ok {
                        for j in 0..tok_chars.len() {
                            marked[py_slots[i + j].0] = true;
                        }
                        matched_token = true;
                        break;
                    }
                }
            }
        }

        // 3. 模糊子序列回退高亮
        if !matched_token {
            let mut ti = 0usize;
            let mut indices = Vec::with_capacity(tok_chars.len());
            for (si, s) in slots.iter().enumerate() {
                let hit = s.lower == tok_chars[ti]
                    || (enable_pinyin
                        && token.is_ascii()
                        && (s.py_primary == Some(tok_chars[ti]) || s.py_alt == Some(tok_chars[ti])));
                if hit {
                    indices.push(si);
                    ti += 1;
                    if ti == tok_chars.len() {
                        break;
                    }
                }
            }
            if ti == tok_chars.len() {
                for si in indices {
                    marked[si] = true;
                }
            }
        }
    }

    // 合并相邻已标记槽位为紧凑 UTF-16 区间
    let mut ranges: Vec<(u32, u32)> = Vec::new();
    let mut idx = 0usize;
    while idx < slots.len() {
        if !marked[idx] {
            idx += 1;
            continue;
        }
        let start = slots[idx].utf16_start;
        let mut len = slots[idx].utf16_len;
        idx += 1;
        while idx < slots.len() && marked[idx] {
            len += slots[idx].utf16_len;
            idx += 1;
        }
        ranges.push((start, len));
    }
    ranges
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn matches_chinese_pinyin_initials() {
        assert!(score_window_match("wx", "微信", "WeChat.exe", true).is_some());
        assert!(score_window_match("sz", "设置", "SystemSettings.exe", true).is_some());
        assert!(score_window_match("txhy", "腾讯会议", "wemeetapp.exe", true).is_some());
        assert!(score_window_match("jsb", "无标题 - 记事本", "notepad.exe", true).is_some());
        assert!(score_window_match("wyy", "网易云音乐", "cloudmusic.exe", true).is_some());
        assert!(score_window_match("blbl", "哔哩哔哩 - 首页", "msedge.exe", true).is_some());
    }

    #[test]
    fn matches_english_acronym_and_fuzzy() {
        let s_vsc = score_window_match("vsc", "main.rs - Visual Studio Code", "Code.exe", true);
        let s_chrome = score_window_match("vsc", "Google Chrome", "chrome.exe", true);
        assert!(s_vsc.is_some());
        assert!(s_chrome.is_none());
        assert!(score_window_match("wt", "Windows Terminal", "WindowsTerminal.exe", true).is_some());
    }

    #[test]
    fn supports_multi_token_and_gb2312_coverage() {
        // 涵盖未在特例表中的常规 GB2312 一级字（如 "北京天气" -> "bjtq"）
        assert!(score_window_match("bjtq", "北京天气预报", "msedge.exe", true).is_some());
        assert!(score_window_match("code rs", "main.rs - Visual Studio Code", "Code.exe", true).is_some());
        assert!(score_window_match("code py", "main.rs - Visual Studio Code", "Code.exe", true).is_none());

        // 验证 UTF-16 高亮区间定位（支持中文拼音首字母与英文直匹配）
        assert_eq!(find_title_highlight_utf16_ranges("jsb", "无标题 - 记事本", true), vec![(6, 3)]);
        assert_eq!(find_title_highlight_utf16_ranges("code", "main.rs - Visual Studio Code", true), vec![(24, 4)]);
    }
}
