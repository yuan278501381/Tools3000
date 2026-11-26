// ─────────────────────────────────────────────────────────────────────────────
// core/json.rs — 零依赖纯 Rust RFC 8259 UTF-8 递归下降 JSON 解析器与序列化器
//                支持完整转义、UTF-16 代理对、精确索引查询与紧凑/美化双模式输出
// ─────────────────────────────────────────────────────────────────────────────

use std::fmt;

/// 通用 JSON 值抽象（保留对象字段插入顺序以便稳定输出）
#[derive(Debug, Clone, PartialEq)]
pub enum JsonValue {
    Null,
    Bool(bool),
    Number(f64),
    String(String),
    Array(Vec<JsonValue>),
    Object(Vec<(String, JsonValue)>),
}

/// JSON 解析错误详情
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct JsonError {
    pub offset: usize,
    pub message: String,
}

impl fmt::Display for JsonError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "JSON parse error at byte {}: {}", self.offset, self.message)
    }
}

impl std::error::Error for JsonError {}

impl JsonValue {
    pub fn object() -> Self {
        JsonValue::Object(Vec::new())
    }

    pub fn array() -> Self {
        JsonValue::Array(Vec::new())
    }

    pub fn from_i64(v: i64) -> Self {
        JsonValue::Number(v as f64)
    }

    pub fn from_u64(v: u64) -> Self {
        JsonValue::Number(v as f64)
    }

    pub fn from_i32(v: i32) -> Self {
        JsonValue::Number(v as f64)
    }

    pub fn from_usize(v: usize) -> Self {
        JsonValue::Number(v as f64)
    }

    /// 向 Object 写入或覆盖键值对
    pub fn insert(&mut self, key: impl Into<String>, val: JsonValue) {
        if let JsonValue::Object(entries) = self {
            let k = key.into();
            if let Some(existing) = entries.iter_mut().find(|(ek, _)| ek == &k) {
                existing.1 = val;
            } else {
                entries.push((k, val));
            }
        }
    }

    /// 向 Array 追加元素
    pub fn push(&mut self, val: JsonValue) {
        if let JsonValue::Array(items) = self {
            items.push(val);
        }
    }

    pub fn get(&self, key: &str) -> Option<&JsonValue> {
        match self {
            JsonValue::Object(entries) => entries.iter().rev().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn as_bool(&self) -> Option<bool> {
        match self {
            JsonValue::Bool(b) => Some(*b),
            _ => None,
        }
    }

    pub fn as_f64(&self) -> Option<f64> {
        match self {
            JsonValue::Number(n) => Some(*n),
            _ => None,
        }
    }

    pub fn as_i64(&self) -> Option<i64> {
        match self {
            JsonValue::Number(n) if n.is_finite() => Some(n.round() as i64),
            _ => None,
        }
    }

    pub fn as_i32(&self) -> Option<i32> {
        self.as_i64().and_then(|v| i32::try_from(v).ok())
    }

    #[cfg(test)]
    pub fn as_u32(&self) -> Option<u32> {
        self.as_i64().and_then(|v| u32::try_from(v).ok())
    }

    pub fn as_usize(&self) -> Option<usize> {
        self.as_i64().and_then(|v| usize::try_from(v).ok())
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            JsonValue::String(s) => Some(s.as_str()),
            _ => None,
        }
    }

    pub fn as_array(&self) -> Option<&[JsonValue]> {
        match self {
            JsonValue::Array(items) => Some(items.as_slice()),
            _ => None,
        }
    }

    pub fn as_object(&self) -> Option<&[(String, JsonValue)]> {
        match self {
            JsonValue::Object(entries) => Some(entries.as_slice()),
            _ => None,
        }
    }

    pub fn get_bool(&self, key: &str, default: bool) -> bool {
        self.get(key).and_then(|v| v.as_bool()).unwrap_or(default)
    }

    pub fn get_i32(&self, key: &str, default: i32) -> i32 {
        self.get(key).and_then(|v| v.as_i32()).unwrap_or(default)
    }

    pub fn get_f64(&self, key: &str, default: f64) -> f64 {
        self.get(key).and_then(|v| v.as_f64()).unwrap_or(default)
    }

    pub fn get_str<'a>(&'a self, key: &str, default: &'a str) -> &'a str {
        self.get(key).and_then(|v| v.as_str()).unwrap_or(default)
    }

    /// 紧凑序列化为合法 UTF-8 JSON 字符串
    pub fn to_string_compact(&self) -> String {
        let mut out = String::with_capacity(256);
        write_json(self, &mut out, false, 0);
        out
    }

    /// 带 2 空格缩进的美化序列化
    pub fn to_string_pretty(&self) -> String {
        let mut out = String::with_capacity(512);
        write_json(self, &mut out, true, 0);
        out
    }
}

/// 解析 UTF-8 JSON 文本（自动容忍 UTF-8 BOM）
pub fn parse_json(input: &str) -> Result<JsonValue, JsonError> {
    let stripped = input.strip_prefix('\u{FEFF}').unwrap_or(input);
    let mut parser = Parser {
        bytes: stripped.as_bytes(),
        pos: 0,
        depth: 0,
    };
    parser.skip_ws();
    let val = parser.parse_value()?;
    parser.skip_ws();
    if parser.pos < parser.bytes.len() {
        return Err(parser.err("trailing characters after root JSON value"));
    }
    Ok(val)
}

const MAX_NESTING_DEPTH: usize = 64;

struct Parser<'a> {
    bytes: &'a [u8],
    pos: usize,
    depth: usize,
}

impl<'a> Parser<'a> {
    fn err(&self, msg: impl Into<String>) -> JsonError {
        JsonError {
            offset: self.pos,
            message: msg.into(),
        }
    }

    fn peek(&self) -> Option<u8> {
        self.bytes.get(self.pos).copied()
    }

    fn next_byte(&mut self) -> Option<u8> {
        let b = self.peek()?;
        self.pos += 1;
        Some(b)
    }

    fn skip_ws(&mut self) {
        while let Some(b) = self.peek() {
            match b {
                b' ' | b'\t' | b'\r' | b'\n' => self.pos += 1,
                _ => break,
            }
        }
    }

    fn parse_value(&mut self) -> Result<JsonValue, JsonError> {
        self.skip_ws();
        let b = self.peek().ok_or_else(|| self.err("unexpected end of input"))?;
        match b {
            b'{' => self.parse_object(),
            b'[' => self.parse_array(),
            b'"' => self.parse_string().map(JsonValue::String),
            b't' => self.expect_literal(b"true", JsonValue::Bool(true)),
            b'f' => self.expect_literal(b"false", JsonValue::Bool(false)),
            b'n' => self.expect_literal(b"null", JsonValue::Null),
            b'-' | b'0'..=b'9' => self.parse_number(),
            _ => Err(self.err(format!("unexpected byte 0x{:02X}", b))),
        }
    }

    fn expect_literal(&mut self, lit: &[u8], val: JsonValue) -> Result<JsonValue, JsonError> {
        if self.bytes[self.pos..].starts_with(lit) {
            self.pos += lit.len();
            Ok(val)
        } else {
            Err(self.err("invalid literal"))
        }
    }

    fn parse_object(&mut self) -> Result<JsonValue, JsonError> {
        if self.depth >= MAX_NESTING_DEPTH {
            return Err(self.err("maximum JSON nesting depth exceeded"));
        }
        self.depth += 1;
        self.pos += 1; // '{'
        self.skip_ws();
        let mut entries = Vec::new();
        if self.peek() == Some(b'}') {
            self.pos += 1;
            self.depth -= 1;
            return Ok(JsonValue::Object(entries));
        }
        loop {
            self.skip_ws();
            if self.peek() != Some(b'"') {
                return Err(self.err("expected string key in object"));
            }
            let key = self.parse_string()?;
            self.skip_ws();
            if self.next_byte() != Some(b':') {
                return Err(self.err("expected ':' after object key"));
            }
            let val = self.parse_value()?;
            entries.push((key, val));
            self.skip_ws();
            match self.next_byte() {
                Some(b',') => continue,
                Some(b'}') => break,
                _ => return Err(self.err("expected ',' or '}' in object")),
            }
        }
        self.depth -= 1;
        Ok(JsonValue::Object(entries))
    }

    fn parse_array(&mut self) -> Result<JsonValue, JsonError> {
        if self.depth >= MAX_NESTING_DEPTH {
            return Err(self.err("maximum JSON nesting depth exceeded"));
        }
        self.depth += 1;
        self.pos += 1; // '['
        self.skip_ws();
        let mut items = Vec::new();
        if self.peek() == Some(b']') {
            self.pos += 1;
            self.depth -= 1;
            return Ok(JsonValue::Array(items));
        }
        loop {
            let val = self.parse_value()?;
            items.push(val);
            self.skip_ws();
            match self.next_byte() {
                Some(b',') => continue,
                Some(b']') => break,
                _ => return Err(self.err("expected ',' or ']' in array")),
            }
        }
        self.depth -= 1;
        Ok(JsonValue::Array(items))
    }

    fn parse_string(&mut self) -> Result<String, JsonError> {
        if self.next_byte() != Some(b'"') {
            return Err(self.err("expected opening '\"'"));
        }
        let mut out = String::new();
        let mut chunk_start = self.pos;
        while self.pos < self.bytes.len() {
            let b = self.bytes[self.pos];
            if b == b'"' {
                let slice = std::str::from_utf8(&self.bytes[chunk_start..self.pos])
                    .map_err(|_| self.err("invalid UTF-8 in string"))?;
                out.push_str(slice);
                self.pos += 1;
                return Ok(out);
            }
            if b == b'\\' {
                let slice = std::str::from_utf8(&self.bytes[chunk_start..self.pos])
                    .map_err(|_| self.err("invalid UTF-8 in string"))?;
                out.push_str(slice);
                self.pos += 1;
                let esc = self.next_byte().ok_or_else(|| self.err("unterminated escape"))?;
                match esc {
                    b'"' => out.push('"'),
                    b'\\' => out.push('\\'),
                    b'/' => out.push('/'),
                    b'b' => out.push('\u{0008}'),
                    b'f' => out.push('\u{000C}'),
                    b'n' => out.push('\n'),
                    b'r' => out.push('\r'),
                    b't' => out.push('\t'),
                    b'u' => {
                        let code = self.parse_hex4()?;
                        if (0xD800..=0xDBFF).contains(&code) {
                            // 解析 UTF-16 高低代理对
                            if self.bytes.get(self.pos..self.pos + 2) != Some(b"\\u") {
                                return Err(self.err("missing low surrogate in \\u escape"));
                            }
                            self.pos += 2;
                            let low = self.parse_hex4()?;
                            if !(0xDC00..=0xDFFF).contains(&low) {
                                return Err(self.err("invalid low surrogate in \\u escape"));
                            }
                            let scalar =
                                0x10000 + (((code as u32 - 0xD800) << 10) | (low as u32 - 0xDC00));
                            let ch = char::from_u32(scalar)
                                .ok_or_else(|| self.err("invalid Unicode scalar"))?;
                            out.push(ch);
                        } else if (0xDC00..=0xDFFF).contains(&code) {
                            return Err(self.err("unexpected lone low surrogate"));
                        } else {
                            let ch = char::from_u32(code as u32)
                                .ok_or_else(|| self.err("invalid Unicode code point"))?;
                            out.push(ch);
                        }
                    }
                    _ => return Err(self.err("invalid escape sequence")),
                }
                chunk_start = self.pos;
                continue;
            }
            if b < 0x20 {
                return Err(self.err("unescaped control character in string"));
            }
            self.pos += 1;
        }
        Err(self.err("unterminated string"))
    }

    fn parse_hex4(&mut self) -> Result<u16, JsonError> {
        if self.pos + 4 > self.bytes.len() {
            return Err(self.err("truncated \\uXXXX escape"));
        }
        let mut val: u16 = 0;
        for i in 0..4 {
            let b = self.bytes[self.pos + i];
            let digit = match b {
                b'0'..=b'9' => (b - b'0') as u16,
                b'a'..=b'f' => (b - b'a' + 10) as u16,
                b'A'..=b'F' => (b - b'A' + 10) as u16,
                _ => return Err(self.err("invalid hex digit in \\uXXXX escape")),
            };
            val = (val << 4) | digit;
        }
        self.pos += 4;
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
                while matches!(self.peek(), Some(b'0'..=b'9')) {
                    self.pos += 1;
                }
            }
            _ => return Err(self.err("invalid number")),
        }
        if self.peek() == Some(b'.') {
            self.pos += 1;
            if !matches!(self.peek(), Some(b'0'..=b'9')) {
                return Err(self.err("expected digit after decimal point"));
            }
            while matches!(self.peek(), Some(b'0'..=b'9')) {
                self.pos += 1;
            }
        }
        if matches!(self.peek(), Some(b'e' | b'E')) {
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
        let raw = std::str::from_utf8(&self.bytes[start..self.pos])
            .map_err(|_| self.err("invalid UTF-8 in number"))?;
        let num = raw.parse::<f64>().map_err(|_| self.err("number out of range"))?;
        if !num.is_finite() {
            return Err(self.err("non-finite JSON number"));
        }
        Ok(JsonValue::Number(num))
    }
}

fn write_json(val: &JsonValue, out: &mut String, pretty: bool, indent: usize) {
    match val {
        JsonValue::Null => out.push_str("null"),
        JsonValue::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        JsonValue::Number(n) => {
            if !n.is_finite() {
                out.push_str("null");
            } else if n.fract() == 0.0 && *n >= -9007199254740991.0 && *n <= 9007199254740991.0 {
                let i = *n as i64;
                out.push_str(&i.to_string());
            } else {
                out.push_str(&format!("{n}"));
            }
        }
        JsonValue::String(s) => write_escaped_str(s, out),
        JsonValue::Array(items) => {
            if items.is_empty() {
                out.push_str("[]");
                return;
            }
            out.push('[');
            for (idx, item) in items.iter().enumerate() {
                if idx > 0 {
                    out.push(',');
                }
                if pretty {
                    out.push('\n');
                    push_indent(out, indent + 2);
                }
                write_json(item, out, pretty, indent + 2);
            }
            if pretty {
                out.push('\n');
                push_indent(out, indent);
            }
            out.push(']');
        }
        JsonValue::Object(entries) => {
            if entries.is_empty() {
                out.push_str("{}");
                return;
            }
            out.push('{');
            for (idx, (k, v)) in entries.iter().enumerate() {
                if idx > 0 {
                    out.push(',');
                }
                if pretty {
                    out.push('\n');
                    push_indent(out, indent + 2);
                }
                write_escaped_str(k, out);
                out.push(':');
                if pretty {
                    out.push(' ');
                }
                write_json(v, out, pretty, indent + 2);
            }
            if pretty {
                out.push('\n');
                push_indent(out, indent);
            }
            out.push('}');
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
            '\u{0008}' => out.push_str("\\b"),
            '\u{000C}' => out.push_str("\\f"),
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip_complex_utf8_json() {
        let mut root = JsonValue::object();
        root.insert("enabled", JsonValue::Bool(true));
        root.insert("name", JsonValue::String("微信\\设置\n\"测试\"\t中文字段".to_string()));
        root.insert("count", JsonValue::from_i64(-42));
        root.insert("ratio", JsonValue::Number(0.375));
        root.insert("empty", JsonValue::Null);
        let mut arr = JsonValue::array();
        arr.push(JsonValue::from_i32(1));
        arr.push(JsonValue::String("ok".to_string()));
        root.insert("items", arr);

        let compact = root.to_string_compact();
        let parsed = parse_json(&compact).expect("compact JSON should parse");
        assert_eq!(parsed, root);

        let pretty = root.to_string_pretty();
        let parsed_pretty = parse_json(&pretty).expect("pretty JSON should parse");
        assert_eq!(parsed_pretty, root);
    }

    #[test]
    fn parses_unicode_escapes_and_bom() {
        let src = "\u{FEFF}{\"zh\":\"\\u5fae\\u4fe1\",\"neg\":-1.5e2,\"bool\":false}";
        let val = parse_json(src).expect("should parse unicode escape and BOM");
        assert_eq!(val.get_str("zh", ""), "微信");
        assert_eq!(val.get_f64("neg", 0.0), -150.0);
        assert!(!val.get_bool("bool", true));
    }

    #[test]
    fn rejects_malformed_json() {
        assert!(parse_json("{unquoted: 1}").is_err());
        assert!(parse_json("[1, 2,]").is_err());
        assert!(parse_json("\"unterminated").is_err());
        assert!(parse_json("123 trailing").is_err());
    }
}
