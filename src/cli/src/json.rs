// ─────────────────────────────────────────────────────────────────────────────
// json.rs — 严格 RFC 8259 零外部依赖 JSON DOM 解析器与序列化器
//
// 职责:
//   - 支撑配置解析、命名管道 IPC、CLI --json 输出与 MCP JSON-RPC 2.0 报文；
//   - 支持完整 UTF-16 代理对转义 (\uD83D\uDE00)、科学计数法与嵌套深度 DoS 防护；
//   - 保持字段插入顺序稳定，确保机器输出可预测、便于 Diff 与回归测试。
// ─────────────────────────────────────────────────────────────────────────────

use std::fmt;

const MAX_NESTING_DEPTH: usize = 64;

#[derive(Debug, Clone, PartialEq)]
pub enum JsonValue {
    Null,
    Bool(bool),
    Int(i64),
    Float(f64),
    String(String),
    Array(Vec<JsonValue>),
    Object(Vec<(String, JsonValue)>),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct JsonError {
    pub message: String,
    pub position: usize,
}

impl fmt::Display for JsonError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "JSON error at byte {}: {}", self.position, self.message)
    }
}

impl std::error::Error for JsonError {}

impl JsonValue {
    pub fn object() -> Self {
        JsonValue::Object(Vec::new())
    }

    pub fn insert(&mut self, key: impl Into<String>, value: JsonValue) {
        if let JsonValue::Object(entries) = self {
            let k = key.into();
            if let Some((_, existing)) = entries.iter_mut().find(|(ek, _)| ek == &k) {
                *existing = value;
            } else {
                entries.push((k, value));
            }
        }
    }

    pub fn get(&self, key: &str) -> Option<&JsonValue> {
        match self {
            JsonValue::Object(entries) => entries.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            JsonValue::String(s) => Some(s.as_str()),
            _ => None,
        }
    }

    pub fn as_bool(&self) -> Option<bool> {
        match self {
            JsonValue::Bool(b) => Some(*b),
            _ => None,
        }
    }

    pub fn as_i64(&self) -> Option<i64> {
        match self {
            JsonValue::Int(i) => Some(*i),
            JsonValue::Float(f) if f.fract() == 0.0 && *f >= i64::MIN as f64 && *f <= i64::MAX as f64 => {
                Some(*f as i64)
            }
            _ => None,
        }
    }

    pub fn as_u64(&self) -> Option<u64> {
        self.as_i64().and_then(|v| if v >= 0 { Some(v as u64) } else { None })
    }

    pub fn as_array(&self) -> Option<&[JsonValue]> {
        match self {
            JsonValue::Array(arr) => Some(arr.as_slice()),
            _ => None,
        }
    }

    pub fn to_json_string(&self) -> String {
        let mut out = String::with_capacity(128);
        self.write_compact(&mut out);
        out
    }

    pub fn to_json_pretty(&self) -> String {
        let mut out = String::with_capacity(256);
        self.write_pretty(&mut out, 0);
        out
    }

    fn write_compact(&self, out: &mut String) {
        match self {
            JsonValue::Null => out.push_str("null"),
            JsonValue::Bool(true) => out.push_str("true"),
            JsonValue::Bool(false) => out.push_str("false"),
            JsonValue::Int(i) => out.push_str(&i.to_string()),
            JsonValue::Float(f) => {
                if f.is_finite() {
                    out.push_str(&f.to_string());
                } else {
                    out.push_str("null");
                }
            }
            JsonValue::String(s) => write_escaped_str(s, out),
            JsonValue::Array(arr) => {
                out.push('[');
                for (idx, item) in arr.iter().enumerate() {
                    if idx > 0 {
                        out.push(',');
                    }
                    item.write_compact(out);
                }
                out.push(']');
            }
            JsonValue::Object(entries) => {
                out.push('{');
                for (idx, (k, v)) in entries.iter().enumerate() {
                    if idx > 0 {
                        out.push(',');
                    }
                    write_escaped_str(k, out);
                    out.push(':');
                    v.write_compact(out);
                }
                out.push('}');
            }
        }
    }

    fn write_pretty(&self, out: &mut String, indent: usize) {
        match self {
            JsonValue::Array(arr) if !arr.is_empty() => {
                out.push_str("[\n");
                for (idx, item) in arr.iter().enumerate() {
                    if idx > 0 {
                        out.push_str(",\n");
                    }
                    push_indent(out, indent + 2);
                    item.write_pretty(out, indent + 2);
                }
                out.push('\n');
                push_indent(out, indent);
                out.push(']');
            }
            JsonValue::Object(entries) if !entries.is_empty() => {
                out.push_str("{\n");
                for (idx, (k, v)) in entries.iter().enumerate() {
                    if idx > 0 {
                        out.push_str(",\n");
                    }
                    push_indent(out, indent + 2);
                    write_escaped_str(k, out);
                    out.push_str(": ");
                    v.write_pretty(out, indent + 2);
                }
                out.push('\n');
                push_indent(out, indent);
                out.push('}');
            }
            _ => self.write_compact(out),
        }
    }
}

fn push_indent(out: &mut String, spaces: usize) {
    for _ in 0..spaces {
        out.push(' ');
    }
}

fn write_escaped_str(s: &str, out: &mut String) {
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
                let code = c as u32;
                out.push_str(&format!("\\u{:04x}", code));
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

pub fn parse_json(input: &str) -> Result<JsonValue, JsonError> {
    let mut parser = Parser {
        bytes: input.as_bytes(),
        pos: 0,
    };
    // 允许跳过 UTF-8 BOM，兼容外部手工编辑的 config.json
    if parser.bytes.starts_with(&[0xEF, 0xBB, 0xBF]) {
        parser.pos = 3;
    }
    parser.skip_ws();
    let val = parser.parse_value(0)?;
    parser.skip_ws();
    if parser.pos != parser.bytes.len() {
        return Err(parser.err("unexpected trailing characters"));
    }
    Ok(val)
}

struct Parser<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl<'a> Parser<'a> {
    fn err(&self, msg: impl Into<String>) -> JsonError {
        JsonError {
            message: msg.into(),
            position: self.pos,
        }
    }

    fn peek(&self) -> Option<u8> {
        self.bytes.get(self.pos).copied()
    }

    fn next(&mut self) -> Option<u8> {
        let b = self.peek()?;
        self.pos += 1;
        Some(b)
    }

    fn skip_ws(&mut self) {
        while let Some(b) = self.peek() {
            if matches!(b, b' ' | b'\t' | b'\r' | b'\n') {
                self.pos += 1;
            } else {
                break;
            }
        }
    }

    fn parse_value(&mut self, depth: usize) -> Result<JsonValue, JsonError> {
        if depth > MAX_NESTING_DEPTH {
            return Err(self.err("maximum JSON nesting depth exceeded"));
        }
        self.skip_ws();
        match self.peek() {
            Some(b'n') => self.parse_literal(b"null", JsonValue::Null),
            Some(b't') => self.parse_literal(b"true", JsonValue::Bool(true)),
            Some(b'f') => self.parse_literal(b"false", JsonValue::Bool(false)),
            Some(b'"') => self.parse_string().map(JsonValue::String),
            Some(b'[') => self.parse_array(depth + 1),
            Some(b'{') => self.parse_object(depth + 1),
            Some(b'-' | b'0'..=b'9') => self.parse_number(),
            Some(_) => Err(self.err("unexpected token")),
            None => Err(self.err("unexpected end of JSON input")),
        }
    }

    fn parse_literal(&mut self, lit: &[u8], val: JsonValue) -> Result<JsonValue, JsonError> {
        if self.bytes[self.pos..].starts_with(lit) {
            self.pos += lit.len();
            Ok(val)
        } else {
            Err(self.err("invalid literal"))
        }
    }

    fn parse_string(&mut self) -> Result<String, JsonError> {
        if self.next() != Some(b'"') {
            return Err(self.err("expected '\"'"));
        }
        let mut out = String::new();
        while let Some(b) = self.next() {
            match b {
                b'"' => return Ok(out),
                b'\\' => {
                    let esc = self.next().ok_or_else(|| self.err("unterminated escape"))?;
                    match esc {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\x08'),
                        b'f' => out.push('\x0c'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let u1 = self.parse_hex4()?;
                            if (0xD800..=0xDBFF).contains(&u1) {
                                if self.next() != Some(b'\\') || self.next() != Some(b'u') {
                                    return Err(self.err("expected low surrogate after high surrogate"));
                                }
                                let u2 = self.parse_hex4()?;
                                if !(0xDC00..=0xDFFF).contains(&u2) {
                                    return Err(self.err("invalid low surrogate"));
                                }
                                let cp = 0x10000
                                    + (((u1 as u32 - 0xD800) << 10) | (u2 as u32 - 0xDC00));
                                let ch = char::from_u32(cp)
                                    .ok_or_else(|| self.err("invalid unicode scalar"))?;
                                out.push(ch);
                            } else if (0xDC00..=0xDFFF).contains(&u1) {
                                return Err(self.err("unexpected lone low surrogate"));
                            } else {
                                let ch = char::from_u32(u1 as u32)
                                    .ok_or_else(|| self.err("invalid unicode scalar"))?;
                                out.push(ch);
                            }
                        }
                        _ => return Err(self.err("invalid escape sequence")),
                    }
                }
                0x00..=0x1F => return Err(self.err("unescaped control character in string")),
                ascii if ascii < 0x80 => out.push(ascii as char),
                lead => {
                    let start = self.pos - 1;
                    let width = if lead & 0xE0 == 0xC0 {
                        2
                    } else if lead & 0xF0 == 0xE0 {
                        3
                    } else if lead & 0xF8 == 0xF0 {
                        4
                    } else {
                        return Err(self.err("invalid UTF-8 lead byte"));
                    };
                    if start + width > self.bytes.len() {
                        return Err(self.err("truncated UTF-8 sequence"));
                    }
                    let slice = std::str::from_utf8(&self.bytes[start..start + width])
                        .map_err(|_| self.err("invalid UTF-8 sequence"))?;
                    out.push_str(slice);
                    self.pos = start + width;
                }
            }
        }
        Err(self.err("unterminated string"))
    }

    fn parse_hex4(&mut self) -> Result<u16, JsonError> {
        if self.pos + 4 > self.bytes.len() {
            return Err(self.err("truncated \\uXXXX escape"));
        }
        let mut val: u16 = 0;
        for _ in 0..4 {
            let b = self.bytes[self.pos];
            self.pos += 1;
            let digit = match b {
                b'0'..=b'9' => (b - b'0') as u16,
                b'a'..=b'f' => (b - b'a' + 10) as u16,
                b'A'..=b'F' => (b - b'A' + 10) as u16,
                _ => return Err(self.err("invalid hex digit in \\u escape")),
            };
            val = (val << 4) | digit;
        }
        Ok(val)
    }

    fn parse_number(&mut self) -> Result<JsonValue, JsonError> {
        let start = self.pos;
        if self.peek() == Some(b'-') {
            self.pos += 1;
        }
        match self.peek() {
            Some(b'0') => self.pos += 1,
            Some(b'1'..=b'9') => {
                self.pos += 1;
                while matches!(self.peek(), Some(b'0'..=b'9')) {
                    self.pos += 1;
                }
            }
            _ => return Err(self.err("invalid number")),
        }

        let mut is_float = false;
        if self.peek() == Some(b'.') {
            is_float = true;
            self.pos += 1;
            if !matches!(self.peek(), Some(b'0'..=b'9')) {
                return Err(self.err("expected digit after decimal point"));
            }
            while matches!(self.peek(), Some(b'0'..=b'9')) {
                self.pos += 1;
            }
        }
        if matches!(self.peek(), Some(b'e' | b'E')) {
            is_float = true;
            self.pos += 1;
            if matches!(self.peek(), Some(b'+' | b'-')) {
                self.pos += 1;
            }
            if !matches!(self.peek(), Some(b'0'..=b'9')) {
                return Err(self.err("expected digit in exponent"));
            }
            while matches!(self.peek(), Some(b'0'..=b'9')) {
                self.pos += 1;
            }
        }

        let num_str = std::str::from_utf8(&self.bytes[start..self.pos])
            .map_err(|_| self.err("invalid number bytes"))?;
        if !is_float {
            if let Ok(i) = num_str.parse::<i64>() {
                return Ok(JsonValue::Int(i));
            }
        }
        let f = num_str.parse::<f64>().map_err(|_| self.err("number out of range"))?;
        Ok(JsonValue::Float(f))
    }

    fn parse_array(&mut self, depth: usize) -> Result<JsonValue, JsonError> {
        self.pos += 1; // '['
        self.skip_ws();
        let mut items = Vec::new();
        if self.peek() == Some(b']') {
            self.pos += 1;
            return Ok(JsonValue::Array(items));
        }
        loop {
            items.push(self.parse_value(depth)?);
            self.skip_ws();
            match self.peek() {
                Some(b',') => {
                    self.pos += 1;
                }
                Some(b']') => {
                    self.pos += 1;
                    return Ok(JsonValue::Array(items));
                }
                _ => return Err(self.err("expected ',' or ']' in array")),
            }
        }
    }

    fn parse_object(&mut self, depth: usize) -> Result<JsonValue, JsonError> {
        self.pos += 1; // '{'
        self.skip_ws();
        let mut entries = Vec::new();
        if self.peek() == Some(b'}') {
            self.pos += 1;
            return Ok(JsonValue::Object(entries));
        }
        loop {
            self.skip_ws();
            let key = self.parse_string()?;
            self.skip_ws();
            if self.next() != Some(b':') {
                return Err(self.err("expected ':' after object key"));
            }
            let val = self.parse_value(depth)?;
            if let Some((_, existing)) = entries.iter_mut().find(|(k, _)| k == &key) {
                *existing = val;
            } else {
                entries.push((key, val));
            }
            self.skip_ws();
            match self.peek() {
                Some(b',') => {
                    self.pos += 1;
                }
                Some(b'}') => {
                    self.pos += 1;
                    return Ok(JsonValue::Object(entries));
                }
                _ => return Err(self.err("expected ',' or '}' in object")),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip_complex_unicode_and_escapes() {
        let src = r#"{"msg":"你好\n\"Tools3000\"\\path","num":-42,"flt":3.5,"arr":[true,false,null,"\u4E2D\u6587"]}"#;
        let parsed = parse_json(src).expect("valid json");
        assert_eq!(parsed.get("msg").and_then(|v| v.as_str()), Some("你好\n\"Tools3000\"\\path"));
        assert_eq!(parsed.get("num").and_then(|v| v.as_i64()), Some(-42));
        let arr = parsed.get("arr").and_then(|v| v.as_array()).expect("array");
        assert_eq!(arr[3].as_str(), Some("中文"));

        let dumped = parsed.to_json_string();
        let reparsed = parse_json(&dumped).expect("reparse");
        assert_eq!(parsed, reparsed);
    }

    #[test]
    fn rejects_excessive_nesting_depth() {
        let deep = "[".repeat(80) + &"]".repeat(80);
        assert!(parse_json(&deep).is_err());
    }
}
