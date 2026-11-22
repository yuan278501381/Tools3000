// ─────────────────────────────────────────────────────────────────────────────
// model.rs — 剪贴板历史条目模型、颜色识别与零依赖安全 JSON 序列化
// ─────────────────────────────────────────────────────────────────────────────

use std::time::{SystemTime, UNIX_EPOCH};

pub const MAX_PREVIEW_CHARS: usize = 320;
pub const MAX_TEXT_BYTES: usize = 512 * 1024; // 单条文本持久化上限 512 KB

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum ClipKind {
    Text = 0,
    Image = 1,
    Files = 2,
    Color = 3,
}

impl ClipKind {
    pub fn from_u8(v: u8) -> Self {
        match v {
            1 => ClipKind::Image,
            2 => ClipKind::Files,
            3 => ClipKind::Color,
            _ => ClipKind::Text,
        }
    }

    pub fn as_str(&self) -> &'static str {
        match self {
            ClipKind::Text => "text",
            ClipKind::Image => "image",
            ClipKind::Files => "files",
            ClipKind::Color => "color",
        }
    }
}

#[derive(Debug, Clone)]
pub struct ClipboardEntry {
    pub id: u64,
    pub kind: ClipKind,
    pub text: String,
    pub preview: String,
    pub image_rel_path: String, // 相对 blobs/ 的安全文件名（如 "a1b2c3d4.png"）
    pub image_width: u32,
    pub image_height: u32,
    pub content_hash: String,
    pub source_app: String,
    pub color_hex: String, // 归一化 #RRGGBB 或 #RRGGBBAA（仅 Color 类型非空）
    pub byte_size: u64,
    pub created_ms: u64,
    pub updated_ms: u64,
    pub pinned: bool,
    pub copy_count: u32,
    // 预计算小写检索索引，避免每次按键搜索重复分配转换
    pub search_blob_lower: String,
}

impl ClipboardEntry {
    pub fn rebuild_derived_fields(&mut self) {
        self.preview = build_preview(&self.text, self.kind, &self.image_rel_path, self.image_width, self.image_height);
        let mut blob = String::with_capacity(self.text.len().min(4096) + self.source_app.len() + 32);
        blob.push_str(&self.text.to_lowercase());
        if !self.source_app.is_empty() {
            blob.push('\n');
            blob.push_str(&self.source_app.to_lowercase());
        }
        if !self.color_hex.is_empty() {
            blob.push('\n');
            blob.push_str(&self.color_hex.to_lowercase());
        }
        self.search_blob_lower = blob;
    }

    pub fn to_json(&self, include_full_text: bool) -> String {
        let mut out = String::with_capacity(256 + if include_full_text { self.text.len() } else { self.preview.len() });
        out.push('{');
        out.push_str("\"id\":");
        out.push_str(&self.id.to_string());
        out.push_str(",\"kind\":");
        push_json_str(self.kind.as_str(), &mut out);
        out.push_str(",\"preview\":");
        push_json_str(&self.preview, &mut out);
        if include_full_text {
            out.push_str(",\"text\":");
            push_json_str(&self.text, &mut out);
        }
        out.push_str(",\"charCount\":");
        out.push_str(&self.text.chars().count().to_string());
        out.push_str(",\"lineCount\":");
        let lines = if self.text.is_empty() { 0 } else { self.text.lines().count().max(1) };
        out.push_str(&lines.to_string());
        out.push_str(",\"imageFile\":");
        push_json_str(&self.image_rel_path, &mut out);
        out.push_str(",\"imageWidth\":");
        out.push_str(&self.image_width.to_string());
        out.push_str(",\"imageHeight\":");
        out.push_str(&self.image_height.to_string());
        out.push_str(",\"sourceApp\":");
        push_json_str(&self.source_app, &mut out);
        out.push_str(",\"colorHex\":");
        push_json_str(&self.color_hex, &mut out);
        out.push_str(",\"byteSize\":");
        out.push_str(&self.byte_size.to_string());
        out.push_str(",\"createdAt\":");
        out.push_str(&self.created_ms.to_string());
        out.push_str(",\"updatedAt\":");
        out.push_str(&self.updated_ms.to_string());
        out.push_str(",\"pinned\":");
        out.push_str(if self.pinned { "true" } else { "false" });
        out.push_str(",\"copyCount\":");
        out.push_str(&self.copy_count.to_string());
        out.push('}');
        out
    }
}

pub fn now_epoch_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

/// FNV-1a 128-bit 极速内容指纹（用于文本/文件去重，零外部依赖）
pub fn fnv1a_128_hex(data: &[u8]) -> String {
    const OFFSET_BASIS: u128 = 0x6c62272e07bb0142_62b821756295c58d;
    const PRIME: u128 = 0x0000000001000000_000000000000013b;
    let mut hash = OFFSET_BASIS;
    for &b in data {
        hash ^= b as u128;
        hash = hash.wrapping_mul(PRIME);
    }
    format!("{:032x}", hash)
}

/// 检测文本是否为标准颜色色值（支持 #RGB / #RRGGBB / #RRGGBBAA / rgb(r,g,b)），返回规范化 #RRGGBB(AA)
pub fn detect_color_hex(raw: &str) -> Option<String> {
    let trimmed = raw.trim();
    if trimmed.len() > 40 || trimmed.contains('\n') {
        return None;
    }

    if let Some(hex) = trimmed.strip_prefix('#') {
        if hex.bytes().all(|b| b.is_ascii_hexdigit()) {
            match hex.len() {
                3 => {
                    let b = hex.as_bytes();
                    return Some(format!(
                        "#{}{}{}{}{}{}",
                        (b[0] as char).to_ascii_uppercase(),
                        (b[0] as char).to_ascii_uppercase(),
                        (b[1] as char).to_ascii_uppercase(),
                        (b[1] as char).to_ascii_uppercase(),
                        (b[2] as char).to_ascii_uppercase(),
                        (b[2] as char).to_ascii_uppercase()
                    ));
                }
                6 | 8 => {
                    return Some(format!("#{}", hex.to_ascii_uppercase()));
                }
                _ => return None,
            }
        }
    }

    let low = trimmed.to_ascii_lowercase();
    if let Some(inner) = low.strip_prefix("rgb(").and_then(|s| s.strip_suffix(')')) {
        let nums: Vec<u8> = inner
            .split(',')
            .filter_map(|p| p.trim().parse::<u16>().ok())
            .filter(|&v| v <= 255)
            .map(|v| v as u8)
            .collect();
        if nums.len() == 3 {
            return Some(format!("#{:02X}{:02X}{:02X}", nums[0], nums[1], nums[2]));
        }
    }

    None
}

pub fn build_preview(text: &str, kind: ClipKind, image_rel_path: &str, w: u32, h: u32) -> String {
    if kind == ClipKind::Image {
        if w > 0 && h > 0 {
            return format!("Image {}x{}", w, h);
        }
        return image_rel_path.to_string();
    }
    let trimmed = text.trim();
    let mut out = String::new();
    let mut count = 0usize;
    let mut truncated = false;
    for ch in trimmed.chars() {
        if count >= MAX_PREVIEW_CHARS {
            truncated = true;
            break;
        }
        if ch == '\r' {
            continue;
        }
        out.push(ch);
        count += 1;
    }
    if truncated {
        out.push_str("...");
    }
    out
}

pub fn push_json_str(s: &str, out: &mut String) {
    out.push('"');
    for ch in s.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\x08' => out.push_str("\\b"),
            '\x0c' => out.push_str("\\f"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => {
                out.push_str(&format!("\\u{:04x}", c as u32));
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

pub fn sanitize_blob_filename(raw: &str) -> String {
    let base = raw
        .rsplit(|c| c == '\\' || c == '/')
        .next()
        .unwrap_or("")
        .trim();
    if base.is_empty()
        || base == "."
        || base == ".."
        || base.len() > 128
        || !base
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '_' | '-' | '.'))
    {
        return String::new();
    }
    base.to_string()
}
