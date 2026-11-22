// ─────────────────────────────────────────────────────────────────────────────
// policy.rs — 外部 CLI / MCP 零信任安全策略、查询构造与 Token 预算裁剪引擎
//
// 核心防线:
//   1. 内置高危凭证目录/文件名/扩展名黑名单，永不向外部 Agent 暴露密钥与浏览器凭据；
//   2. 支持用户追加自定义排除模式与可选允许根目录白名单 (allowedRoots)；
//   3. 按 UTF-8 字符边界安全截断内容片段，防止压缩 JS/长日志撑爆 LLM 上下文窗口。
// ─────────────────────────────────────────────────────────────────────────────

use crate::constants::{
    BUILTIN_SENSITIVE_EXTENSIONS, BUILTIN_SENSITIVE_FILENAMES, BUILTIN_SENSITIVE_SEGMENTS,
    DEFAULT_MAX_SNIPPET_CHARS, MAX_SNIPPET_CHARS, MAX_SNIPPETS_PER_FILE, MIN_SNIPPET_CHARS,
};
use crate::json::JsonValue;

#[derive(Debug, Clone, Default)]
pub struct SecurityPolicy {
    pub external_api_enabled: bool,
    pub extra_exclude_patterns: Vec<String>,
    pub allowed_roots: Vec<String>,
}

impl SecurityPolicy {
    /// 标准化路径：去前后空白、剥离 \\?\ 前缀、正斜杠转反斜杠、ASCII 小写折叠
    pub fn normalize_path(raw: &str) -> String {
        let mut s = raw.trim().replace('/', "\\");
        if let Some(stripped) = s.strip_prefix(r"\\?\") {
            s = stripped.to_string();
        }
        while s.len() > 3 && s.ends_with('\\') {
            s.pop();
        }
        s.to_lowercase()
    }

    fn normalize_root_prefix(raw: &str) -> Option<String> {
        let mut n = Self::normalize_path(raw);
        if n.is_empty() {
            return None;
        }
        if !n.ends_with('\\') {
            n.push('\\');
        }
        Some(n)
    }

    /// 判定某绝对路径是否命中内置高危敏感凭证规则
    pub fn is_builtin_sensitive(raw_path: &str) -> bool {
        let norm = Self::normalize_path(raw_path);
        if norm.is_empty() {
            return true;
        }

        // 1. 多级目录片段匹配（如 microsoft\credentials、google\chrome\user data）
        for seg in BUILTIN_SENSITIVE_SEGMENTS {
            if seg.contains('\\') {
                let needle_mid = format!("\\{}\\", seg);
                let needle_end = format!("\\{}", seg);
                if norm.contains(&needle_mid) || norm.ends_with(&needle_end) {
                    return true;
                }
            }
        }

        // 2. 逐路径段精确匹配（如 .ssh、.aws、.gnupg、.kube）
        let mut last_segment = "";
        for part in norm.split('\\') {
            if part.is_empty() {
                continue;
            }
            last_segment = part;
            for seg in BUILTIN_SENSITIVE_SEGMENTS {
                if !seg.contains('\\') && part == *seg {
                    return true;
                }
            }
        }

        // 3. 文件名黑名单与 .env.* 前缀拦截
        if !last_segment.is_empty() {
            if last_segment == ".env" || last_segment.starts_with(".env.") {
                return true;
            }
            for fname in BUILTIN_SENSITIVE_FILENAMES {
                if last_segment == *fname {
                    return true;
                }
            }
            if let Some((_, ext)) = last_segment.rsplit_once('.') {
                for sens_ext in BUILTIN_SENSITIVE_EXTENSIONS {
                    if ext == *sens_ext {
                        return true;
                    }
                }
            }
        }

        false
    }

    /// 判定某路径在当前策略下是否允许返回给外部 CLI / MCP 客户端
    pub fn is_path_allowed(&self, raw_path: &str) -> bool {
        if Self::is_builtin_sensitive(raw_path) {
            return false;
        }

        let norm = Self::normalize_path(raw_path);
        if norm.is_empty() {
            return false;
        }

        // 校验可选白名单根目录
        if !self.allowed_roots.is_empty() {
            let mut matched_root = false;
            for root in &self.allowed_roots {
                if let Some(prefix) = Self::normalize_root_prefix(root) {
                    let exact = prefix.trim_end_matches('\\');
                    if norm == exact || norm.starts_with(&prefix) {
                        matched_root = true;
                        break;
                    }
                }
            }
            if !matched_root {
                return false;
            }
        }

        // 校验用户自定义排除模式
        for pat in &self.extra_exclude_patterns {
            let p = Self::normalize_path(pat);
            if p.is_empty() {
                continue;
            }
            if p.contains('\\') || p.contains(':') {
                let prefix = if p.ends_with('\\') {
                    p.clone()
                } else {
                    format!("{}\\", p)
                };
                if norm == p || norm.starts_with(&prefix) || norm.contains(&format!("\\{}\\", p.trim_matches('\\'))) {
                    return false;
                }
            } else {
                // 目录段或文件名子串
                if norm.split('\\').any(|seg| seg == p || seg.ends_with(&p)) {
                    return false;
                }
            }
        }

        true
    }

    /// 构造传给 Tools3000_Service.exe 的合成排除列表（兼顾服务端提前剪枝性能）
    pub fn merged_service_excludes(&self, global_excludes: &[String]) -> Vec<String> {
        let mut out: Vec<String> = Vec::with_capacity(global_excludes.len() + self.extra_exclude_patterns.len() + 8);
        for item in global_excludes {
            let t = item.trim();
            if !t.is_empty() && !out.iter().any(|x| x.eq_ignore_ascii_case(t)) {
                out.push(t.to_string());
            }
        }
        for item in &self.extra_exclude_patterns {
            let t = item.trim();
            if !t.is_empty() && !out.iter().any(|x| x.eq_ignore_ascii_case(t)) {
                out.push(t.to_string());
            }
        }
        // 高频敏感目录提前交由服务端剪枝，减少无谓磁盘读取
        for seg in [".ssh", ".gnupg", ".aws", ".azure", ".kube", ".gcloud"] {
            if !out.iter().any(|x| x.eq_ignore_ascii_case(seg)) {
                out.push(seg.to_string());
            }
        }
        out
    }

    /// 过滤服务端返回的 results 数组，剔除敏感与越界条目并按 max_snippet_chars 裁剪上下文片段
    pub fn sanitize_results(
        &self,
        raw_results: &[JsonValue],
        limit: usize,
        max_snippet_chars: usize,
    ) -> (Vec<JsonValue>, usize) {
        let clamped_chars = max_snippet_chars.clamp(MIN_SNIPPET_CHARS, MAX_SNIPPET_CHARS);
        let mut clean = Vec::with_capacity(raw_results.len().min(limit));
        let mut redacted_count = 0usize;

        for item in raw_results {
            let path = item.get("path").and_then(|v| v.as_str()).unwrap_or("");
            if !self.is_path_allowed(path) {
                redacted_count += 1;
                continue;
            }
            if clean.len() >= limit {
                break;
            }

            let mut obj = JsonValue::object();
            if let Some(name) = item.get("name").and_then(|v| v.as_str()) {
                obj.insert("name", JsonValue::String(name.to_string()));
            }
            obj.insert("path", JsonValue::String(path.to_string()));
            if let Some(is_dir) = item.get("isDirectory").and_then(|v| v.as_bool()) {
                obj.insert("isDirectory", JsonValue::Bool(is_dir));
            }
            if let Some(size) = item.get("size").and_then(|v| v.as_i64()) {
                obj.insert("size", JsonValue::Int(size));
            }
            if let Some(mtime) = item.get("lastWriteTime").and_then(|v| v.as_i64()) {
                obj.insert("lastWriteTime", JsonValue::Int(mtime));
            }
            if let Some(snips) = item.get("snippets").and_then(|v| v.as_array()) {
                let mut clean_snips = Vec::with_capacity(snips.len().min(MAX_SNIPPETS_PER_FILE));
                for snip in snips.iter().take(MAX_SNIPPETS_PER_FILE) {
                    let mut s_obj = JsonValue::object();
                    if let Some(line_no) = snip.get("lineNumber").and_then(|v| v.as_i64()) {
                        s_obj.insert("lineNumber", JsonValue::Int(line_no));
                    }
                    let line_text = snip.get("lineContent").and_then(|v| v.as_str()).unwrap_or("");
                    let match_offset = snip
                        .get("matchOffset")
                        .and_then(|v| v.as_u64())
                        .unwrap_or(0) as usize;
                    let (truncated, was_truncated) =
                        truncate_snippet_utf8(line_text, match_offset, clamped_chars);
                    s_obj.insert("lineContent", JsonValue::String(truncated));
                    if was_truncated {
                        s_obj.insert("truncated", JsonValue::Bool(true));
                    }
                    clean_snips.push(s_obj);
                }
                obj.insert("snippets", JsonValue::Array(clean_snips));
            }
            clean.push(obj);
        }

        (clean, redacted_count)
    }
}

/// 构造发往 Tools3000_Service 的统一查询表达式（叠加 scope 路径前缀与 ext 扩展名过滤）
pub fn build_service_query_expression(
    raw_query: &str,
    scope: Option<&str>,
    extensions: &[String],
    content_mode: bool,
) -> String {
    let mut parts: Vec<String> = Vec::new();

    if let Some(sc) = scope.map(str::trim).filter(|s| !s.is_empty()) {
        let mut normalized = sc.replace('/', "\\");
        if !normalized.ends_with('\\') {
            normalized.push('\\');
        }
        if normalized.contains(' ') {
            parts.push(format!("path:\"{}\"", normalized.replace('"', "")));
        } else {
            parts.push(format!("path:{}", normalized));
        }
    }

    let cleaned_exts: Vec<String> = extensions
        .iter()
        .flat_map(|e| e.split(|c| c == ',' || c == ';' || c == ' '))
        .map(|s| s.trim().trim_start_matches('.').to_ascii_lowercase())
        .filter(|s| !s.is_empty() && s.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-'))
        .collect();
    if !cleaned_exts.is_empty() {
        parts.push(format!("ext:{}", cleaned_exts.join(";")));
    }

    let q = raw_query.trim();
    if !q.is_empty() {
        if content_mode && !q.to_ascii_lowercase().contains("content:") {
            if q.contains(' ') && !(q.starts_with('"') && q.ends_with('"')) {
                parts.push(format!("content:\"{}\"", q.replace('"', "")));
            } else {
                parts.push(format!("content:{}", q));
            }
        } else {
            parts.push(q.to_string());
        }
    }

    parts.join(" ")
}

/// 按 UTF-8 字符边界居中裁剪内容片段，优先保留命中关键词附近上下文，绝不切碎多字节中文
pub fn truncate_snippet_utf8(text: &str, match_char_offset: usize, max_chars: usize) -> (String, bool) {
    let limit = if max_chars == 0 {
        DEFAULT_MAX_SNIPPET_CHARS
    } else {
        max_chars
    };
    let chars: Vec<char> = text.chars().collect();
    if chars.len() <= limit {
        return (text.to_string(), false);
    }

    let half = limit / 2;
    let center = match_char_offset.min(chars.len());
    let mut start = center.saturating_sub(half);
    let end = (start + limit).min(chars.len());
    if end - start < limit {
        start = end.saturating_sub(limit);
    }

    let mut out = String::with_capacity(limit * 2 + 6);
    if start > 0 {
        out.push_str("...");
    }
    for ch in &chars[start..end] {
        out.push(*ch);
    }
    if end < chars.len() {
        out.push_str("...");
    }
    (out, true)
}

/// 将逗号/分号/换行分隔的文本配置项拆分为去重列表
pub fn split_config_list(raw: &str) -> Vec<String> {
    let mut list = Vec::new();
    for piece in raw.split(|c| c == '\n' || c == '\r' || c == ';' || c == ',') {
        let trimmed = piece.trim();
        if !trimmed.is_empty() && !list.iter().any(|x: &String| x.eq_ignore_ascii_case(trimmed)) {
            list.push(trimmed.to_string());
        }
    }
    list
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn blocks_builtin_sensitive_paths_and_credentials() {
        let policy = SecurityPolicy {
            external_api_enabled: true,
            extra_exclude_patterns: vec!["D:\\PrivateVault".to_string()],
            allowed_roots: vec![],
        };

        assert!(!policy.is_path_allowed(r"C:\Users\alice\.ssh\id_ed25519"));
        assert!(!policy.is_path_allowed(r"C:\Users\alice\.aws\credentials"));
        assert!(!policy.is_path_allowed(r"C:\repo\app\.env"));
        assert!(!policy.is_path_allowed(r"C:\repo\app\.env.production"));
        assert!(!policy.is_path_allowed(r"C:\Users\alice\AppData\Local\Google\Chrome\User Data\Default\Login Data"));
        assert!(!policy.is_path_allowed(r"C:\certs\prod.pem"));
        assert!(!policy.is_path_allowed(r"D:\PrivateVault\secret.txt"));

        assert!(policy.is_path_allowed(r"C:\repo\Tools3000\README.md"));
        assert!(policy.is_path_allowed(r"D:\Docs\architecture_2026.docx"));
    }

    #[test]
    fn enforces_optional_allowed_roots_whitelist() {
        let policy = SecurityPolicy {
            external_api_enabled: true,
            extra_exclude_patterns: vec![],
            allowed_roots: vec![r"C:\repo\Tools3000".to_string()],
        };
        assert!(policy.is_path_allowed(r"C:\repo\Tools3000\src\main.cpp"));
        assert!(!policy.is_path_allowed(r"C:\repo\Tools3000_Secret\a.txt"));
        assert!(!policy.is_path_allowed(r"D:\Docs\notes.md"));
        assert!(!policy.is_path_allowed(r"C:\repo\Tools3000\.env"));
    }

    #[test]
    fn truncates_multibyte_utf8_snippet_cleanly_around_match() {
        let long_cn = format!("{}核心命中词{}", "前导文本".repeat(30), "后续文本".repeat(30));
        let (out, truncated) = truncate_snippet_utf8(&long_cn, 122, 60);
        assert!(truncated);
        assert!(out.contains("核心命中词"));
        assert!(out.starts_with("..."));
        assert!(out.ends_with("..."));
    }

    #[test]
    fn builds_service_query_with_scope_and_extensions() {
        let q = build_service_query_expression(
            "quarterly report",
            Some(r"D:\Work Docs"),
            &[".pdf".to_string(), "docx".to_string()],
            true,
        );
        assert_eq!(q, r#"path:"D:\Work Docs\" ext:pdf;docx content:"quarterly report""#);
    }
}
