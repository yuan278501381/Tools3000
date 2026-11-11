// Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors.
// All rights reserved. | MIT License
//
// Zero-line-allocation multi-term AND document matcher and linear-time O(N) Thompson NFA regex engine.

use crate::constants::INITIAL_FOLD_BUFFER_CAPACITY;
use crate::ffi::T3kEmitSnippetFn;
use crate::snippet::{build_aligned_snippet, BuiltSnippet};

#[inline(always)]
pub fn fold_u16_case(ch: u16) -> u16 {
    if ch < 128 {
        if (b'A' as u16..=b'Z' as u16).contains(&ch) {
            return ch + 32;
        }
        return ch;
    }
    // Fast path for CJK Unified Ideographs and symbols (0x3000..=0x9FFF): caseless
    if (0x3000..=0x9FFF).contains(&ch) {
        return ch;
    }
    // Fullwidth Latin capital letters (FF21..=FF3A -> FF41..=FF5A)
    if (0xFF21..=0xFF3A).contains(&ch) {
        return ch + 0x20;
    }
    if let Some(c) = char::from_u32(ch as u32) {
        let mut lower = c.to_lowercase();
        if let Some(first) = lower.next() {
            if lower.next().is_none() && (first as u32) <= 0xFFFF {
                return first as u16;
            }
        }
    }
    ch
}

#[inline]
fn find_u16_subslice(haystack: &[u16], needle: &[u16]) -> Option<usize> {
    let nlen = needle.len();
    let hlen = haystack.len();
    if nlen == 0 || hlen < nlen {
        return None;
    }
    let first = needle[0];
    let last = needle[nlen - 1];
    let end = hlen - nlen + 1;
    for i in 0..end {
        if haystack[i] == first
            && haystack[i + nlen - 1] == last
            && (nlen <= 2 || haystack.get(i + 1..i + nlen - 1) == needle.get(1..nlen - 1))
        {
            return Some(i);
        }
    }
    None
}

pub struct RustMultiTermMatcher {
    case_sensitive: bool,
    max_snippets: usize,
    terms: Vec<Vec<u16>>,
    term_matched: Vec<bool>,
    matched_term_count: usize,
    primary_snippets: Vec<BuiltSnippet>,
    extra_snippets: Vec<BuiltSnippet>,
    reusable_fold_buf: Vec<u16>,
}

impl RustMultiTermMatcher {
    pub fn new(raw_terms: &[&[u16]], case_sensitive: bool, max_snippets_per_file: usize) -> Self {
        let mut terms = Vec::with_capacity(raw_terms.len());
        for &t in raw_terms {
            if t.is_empty() {
                continue;
            }
            if case_sensitive {
                terms.push(t.to_vec());
            } else {
                terms.push(t.iter().map(|&c| fold_u16_case(c)).collect());
            }
        }
        let term_count = terms.len();
        Self {
            case_sensitive,
            max_snippets: max_snippets_per_file.max(1),
            terms,
            term_matched: vec![false; term_count],
            matched_term_count: 0,
            primary_snippets: Vec::with_capacity(max_snippets_per_file.max(1)),
            extra_snippets: Vec::with_capacity(max_snippets_per_file.max(1)),
            reusable_fold_buf: Vec::with_capacity(INITIAL_FOLD_BUFFER_CAPACITY),
        }
    }

    #[inline(always)]
    pub fn has_valid_terms(&self) -> bool {
        !self.terms.is_empty()
    }

    #[inline(always)]
    pub fn all_terms_matched(&self) -> bool {
        !self.terms.is_empty() && self.matched_term_count == self.terms.len()
    }

    #[inline(always)]
    pub fn can_stop_early(&self) -> bool {
        self.all_terms_matched()
            && (self.primary_snippets.len() + self.extra_snippets.len()) >= self.max_snippets
    }

    pub fn process_line(&mut self, line_number: u32, raw_line: &[u16]) {
        if self.terms.is_empty() || raw_line.is_empty() {
            return;
        }

        let mut trimmed_len = raw_line.len();
        while trimmed_len > 0 {
            let last = raw_line[trimmed_len - 1];
            if last == b'\r' as u16 || last == b'\n' as u16 {
                trimmed_len -= 1;
            } else {
                break;
            }
        }
        if trimmed_len == 0 {
            return;
        }
        let clean_view = &raw_line[..trimmed_len];

        let haystack: &[u16] = if self.case_sensitive {
            clean_view
        } else {
            self.reusable_fold_buf.clear();
            self.reusable_fold_buf
                .extend(clean_view.iter().map(|&c| fold_u16_case(c)));
            &self.reusable_fold_buf
        };

        let mut first_hit_pos: Option<usize> = None;
        let mut first_hit_len: usize = 0;
        let mut line_matched_unseen = false;

        for (idx, needle) in self.terms.iter().enumerate() {
            if let Some(pos) = find_u16_subslice(haystack, needle) {
                if !self.term_matched[idx] {
                    self.term_matched[idx] = true;
                    self.matched_term_count += 1;
                    if !line_matched_unseen {
                        line_matched_unseen = true;
                        first_hit_pos = Some(pos);
                        first_hit_len = needle.len();
                    }
                } else if first_hit_pos.is_none() {
                    first_hit_pos = Some(pos);
                    first_hit_len = needle.len();
                }
            }
        }

        let Some(hit_pos) = first_hit_pos else {
            return;
        };

        if line_matched_unseen && self.primary_snippets.len() < self.max_snippets {
            if let Some(snip) =
                build_aligned_snippet(line_number, clean_view, hit_pos, first_hit_len)
            {
                self.primary_snippets.push(snip);
            }
        } else if (self.primary_snippets.len() + self.extra_snippets.len()) < self.max_snippets {
            if let Some(snip) =
                build_aligned_snippet(line_number, clean_view, hit_pos, first_hit_len)
            {
                self.extra_snippets.push(snip);
            }
        }
    }

    pub fn finalize(
        &mut self,
        emit_cb: T3kEmitSnippetFn,
        emit_ctx: *mut std::ffi::c_void,
    ) -> bool {
        if !self.all_terms_matched() {
            return false;
        }
        let mut out = Vec::with_capacity(
            self.max_snippets
                .min(self.primary_snippets.len() + self.extra_snippets.len()),
        );
        for snip in self.primary_snippets.drain(..) {
            if out.len() >= self.max_snippets {
                break;
            }
            out.push(snip);
        }
        for snip in self.extra_snippets.drain(..) {
            if out.len() >= self.max_snippets {
                break;
            }
            out.push(snip);
        }
        if out.is_empty() {
            return false;
        }
        out.sort_by_key(|s| s.line_number);
        if let Some(cb) = emit_cb {
            for s in &out {
                cb(
                    emit_ctx,
                    s.line_number,
                    s.line_content.as_ptr(),
                    s.line_content.len(),
                    s.match_offset,
                    s.match_length,
                );
            }
        }
        true
    }
}

// =====================================================================================
// Linear-Time O(N) Thompson NFA Regular Expression Engine for UTF-16 Search Expressions
// Zero backtracking, zero runtime heap allocation, immune to catastrophic ReDoS.
// =====================================================================================

const NFA_WORDS: usize = 32; // 32 * 64 = 2048 states max, fits entirely on stack
const MAX_NFA_STATES: usize = NFA_WORDS * 64;
const MAX_PARSE_DEPTH: usize = 64;
const MAX_BOUNDED_REPEAT: usize = 128;

const BUILTIN_DIGIT: u8 = 1 << 0;
const BUILTIN_NOT_DIGIT: u8 = 1 << 1;
const BUILTIN_WORD: u8 = 1 << 2;
const BUILTIN_NOT_WORD: u8 = 1 << 3;
const BUILTIN_SPACE: u8 = 1 << 4;
const BUILTIN_NOT_SPACE: u8 = 1 << 5;

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum RegexCompileStatus {
    Compiled = 1,
    UnsupportedFeature = 2,
    InvalidSyntax = 0,
}

#[inline(always)]
fn is_digit_u16(ch: u16) -> bool {
    (b'0' as u16..=b'9' as u16).contains(&ch)
}

#[inline(always)]
fn is_space_u16(ch: u16) -> bool {
    matches!(
        ch,
        0x0009 | 0x000A | 0x000B | 0x000C | 0x000D | 0x0020 | 0x00A0 | 0x3000
    )
}

#[inline(always)]
fn is_word_u16(ch: u16) -> bool {
    if ch < 128 {
        return (b'0' as u16..=b'9' as u16).contains(&ch)
            || (b'A' as u16..=b'Z' as u16).contains(&ch)
            || (b'a' as u16..=b'z' as u16).contains(&ch)
            || ch == (b'_' as u16);
    }
    if let Some(c) = char::from_u32(ch as u32) {
        return c.is_alphanumeric() || ch == (b'_' as u16);
    }
    false
}

#[inline(always)]
fn matches_builtin_mask(mask: u8, ch: u16) -> bool {
    if mask == 0 {
        return false;
    }
    if (mask & BUILTIN_DIGIT) != 0 && is_digit_u16(ch) {
        return true;
    }
    if (mask & BUILTIN_NOT_DIGIT) != 0 && !is_digit_u16(ch) {
        return true;
    }
    if (mask & BUILTIN_WORD) != 0 && is_word_u16(ch) {
        return true;
    }
    if (mask & BUILTIN_NOT_WORD) != 0 && !is_word_u16(ch) {
        return true;
    }
    if (mask & BUILTIN_SPACE) != 0 && is_space_u16(ch) {
        return true;
    }
    if (mask & BUILTIN_NOT_SPACE) != 0 && !is_space_u16(ch) {
        return true;
    }
    false
}

#[derive(Clone, Debug)]
enum RegexAst {
    Empty,
    AnchorStart,
    AnchorEnd,
    WordBoundary { negated: bool },
    AnyChar,
    Literal(u16),
    Builtin(u8),
    CharClass {
        ranges: Vec<(u16, u16)>,
        builtins: u8,
        negated: bool,
    },
    Concat(Vec<RegexAst>),
    Alt(Vec<RegexAst>),
    Repeat {
        node: Box<RegexAst>,
        min: usize,
        max: Option<usize>, // None = unbounded
    },
}

struct RegexParser<'a> {
    pat: &'a [u16],
    pos: usize,
    case_sensitive: bool,
}

impl<'a> RegexParser<'a> {
    fn new(pat: &'a [u16], case_sensitive: bool) -> Self {
        Self {
            pat,
            pos: 0,
            case_sensitive,
        }
    }

    fn peek(&self) -> Option<u16> {
        self.pat.get(self.pos).copied()
    }

    fn next(&mut self) -> Option<u16> {
        let ch = self.pat.get(self.pos).copied()?;
        self.pos += 1;
        Some(ch)
    }

    fn normalize_char(&self, ch: u16) -> u16 {
        if self.case_sensitive {
            ch
        } else {
            fold_u16_case(ch)
        }
    }

    fn parse_hex_digits(&mut self, count: usize) -> Result<u16, RegexCompileStatus> {
        let mut val: u32 = 0;
        for _ in 0..count {
            let Some(c) = self.next() else {
                return Err(RegexCompileStatus::InvalidSyntax);
            };
            let nib = if (b'0' as u16..=b'9' as u16).contains(&c) {
                (c - b'0' as u16) as u32
            } else if (b'a' as u16..=b'f' as u16).contains(&c) {
                (c - b'a' as u16 + 10) as u32
            } else if (b'A' as u16..=b'F' as u16).contains(&c) {
                (c - b'A' as u16 + 10) as u32
            } else {
                return Err(RegexCompileStatus::InvalidSyntax);
            };
            val = (val << 4) | nib;
        }
        Ok(val as u16)
    }

    fn parse_escape_in_class(&mut self) -> Result<Result<u16, u8>, RegexCompileStatus> {
        let Some(esc) = self.next() else {
            return Err(RegexCompileStatus::InvalidSyntax);
        };
        match esc as u32 {
            x if x == b'd' as u32 => Ok(Err(BUILTIN_DIGIT)),
            x if x == b'D' as u32 => Ok(Err(BUILTIN_NOT_DIGIT)),
            x if x == b'w' as u32 => Ok(Err(BUILTIN_WORD)),
            x if x == b'W' as u32 => Ok(Err(BUILTIN_NOT_WORD)),
            x if x == b's' as u32 => Ok(Err(BUILTIN_SPACE)),
            x if x == b'S' as u32 => Ok(Err(BUILTIN_NOT_SPACE)),
            x if x == b't' as u32 => Ok(Ok(b'\t' as u16)),
            x if x == b'n' as u32 => Ok(Ok(b'\n' as u16)),
            x if x == b'r' as u32 => Ok(Ok(b'\r' as u16)),
            x if x == b'f' as u32 => Ok(Ok(0x000C)),
            x if x == b'v' as u32 => Ok(Ok(0x000B)),
            x if x == b'0' as u32 => Ok(Ok(0x0000)),
            x if x == b'x' as u32 => Ok(Ok(self.parse_hex_digits(2)?)),
            x if x == b'u' as u32 => Ok(Ok(self.parse_hex_digits(4)?)),
            _ => Ok(Ok(esc)),
        }
    }

    fn parse_char_class(&mut self) -> Result<RegexAst, RegexCompileStatus> {
        let mut negated = false;
        if self.peek() == Some(b'^' as u16) {
            negated = true;
            self.pos += 1;
        }

        let mut ranges: Vec<(u16, u16)> = Vec::new();
        let mut builtins: u8 = 0;
        let mut first = true;

        loop {
            let Some(ch) = self.next() else {
                return Err(RegexCompileStatus::InvalidSyntax);
            };
            if ch == (b']' as u16) && !first {
                break;
            }
            first = false;

            // Reject POSIX collating/character class syntax [[:alpha:]] to fallback if ever used
            if ch == (b'[' as u16) && self.peek() == Some(b':' as u16) {
                return Err(RegexCompileStatus::UnsupportedFeature);
            }

            let start_char_opt: Option<u16> = if ch == (b'\\' as u16) {
                match self.parse_escape_in_class()? {
                    Ok(lit) => Some(lit),
                    Err(mask) => {
                        builtins |= mask;
                        None
                    }
                }
            } else {
                Some(ch)
            };

            if let Some(start_raw) = start_char_opt {
                if self.peek() == Some(b'-' as u16) {
                    let after_dash = self.pat.get(self.pos + 1).copied();
                    if let Some(next_c) = after_dash {
                        if next_c != (b']' as u16) {
                            self.pos += 1; // consume '-'
                            let end_raw = if self.next() == Some(b'\\' as u16) {
                                match self.parse_escape_in_class()? {
                                    Ok(lit) => lit,
                                    Err(_) => return Err(RegexCompileStatus::InvalidSyntax),
                                }
                            } else {
                                next_c
                            };
                            if start_raw > end_raw {
                                return Err(RegexCompileStatus::InvalidSyntax);
                            }
                            if self.case_sensitive {
                                ranges.push((start_raw, end_raw));
                            } else if end_raw.saturating_sub(start_raw) <= 256 {
                                for code in start_raw..=end_raw {
                                    let fc = fold_u16_case(code);
                                    ranges.push((fc, fc));
                                }
                            } else {
                                ranges.push((start_raw, end_raw));
                            }
                            continue;
                        }
                    }
                }
                let norm = self.normalize_char(start_raw);
                ranges.push((norm, norm));
            }
        }

        if !ranges.is_empty() {
            ranges.sort_unstable_by_key(|r| r.0);
            let mut merged: Vec<(u16, u16)> = Vec::with_capacity(ranges.len());
            for (lo, hi) in ranges {
                if let Some(last) = merged.last_mut() {
                    if lo <= last.1.saturating_add(1) {
                        last.1 = last.1.max(hi);
                        continue;
                    }
                }
                merged.push((lo, hi));
            }
            ranges = merged;
        }

        Ok(RegexAst::CharClass {
            ranges,
            builtins,
            negated,
        })
    }

    fn parse_escape_atom(&mut self) -> Result<RegexAst, RegexCompileStatus> {
        let Some(esc) = self.next() else {
            return Err(RegexCompileStatus::InvalidSyntax);
        };
        if (b'1' as u16..=b'9' as u16).contains(&esc) {
            // Backreferences require non-regular backtracking
            return Err(RegexCompileStatus::UnsupportedFeature);
        }
        match esc as u32 {
            x if x == b'b' as u32 => Ok(RegexAst::WordBoundary { negated: false }),
            x if x == b'B' as u32 => Ok(RegexAst::WordBoundary { negated: true }),
            x if x == b'd' as u32 => Ok(RegexAst::Builtin(BUILTIN_DIGIT)),
            x if x == b'D' as u32 => Ok(RegexAst::Builtin(BUILTIN_NOT_DIGIT)),
            x if x == b'w' as u32 => Ok(RegexAst::Builtin(BUILTIN_WORD)),
            x if x == b'W' as u32 => Ok(RegexAst::Builtin(BUILTIN_NOT_WORD)),
            x if x == b's' as u32 => Ok(RegexAst::Builtin(BUILTIN_SPACE)),
            x if x == b'S' as u32 => Ok(RegexAst::Builtin(BUILTIN_NOT_SPACE)),
            x if x == b't' as u32 => Ok(RegexAst::Literal(b'\t' as u16)),
            x if x == b'n' as u32 => Ok(RegexAst::Literal(b'\n' as u16)),
            x if x == b'r' as u32 => Ok(RegexAst::Literal(b'\r' as u16)),
            x if x == b'f' as u32 => Ok(RegexAst::Literal(0x000C)),
            x if x == b'v' as u32 => Ok(RegexAst::Literal(0x000B)),
            x if x == b'0' as u32 => Ok(RegexAst::Literal(0x0000)),
            x if x == b'x' as u32 => {
                let c = self.parse_hex_digits(2)?;
                Ok(RegexAst::Literal(self.normalize_char(c)))
            }
            x if x == b'u' as u32 => {
                let c = self.parse_hex_digits(4)?;
                Ok(RegexAst::Literal(self.normalize_char(c)))
            }
            x if x == b'p' as u32 || x == b'P' as u32 => {
                Err(RegexCompileStatus::UnsupportedFeature)
            }
            _ => Ok(RegexAst::Literal(self.normalize_char(esc))),
        }
    }

    fn try_parse_braced_quantifier(&mut self) -> Result<Option<(usize, Option<usize>)>, RegexCompileStatus> {
        let saved = self.pos;
        let mut min_val: usize = 0;
        let mut has_min = false;

        while let Some(c) = self.peek() {
            if (b'0' as u16..=b'9' as u16).contains(&c) {
                has_min = true;
                min_val = min_val
                    .checked_mul(10)
                    .and_then(|v| v.checked_add((c - b'0' as u16) as usize))
                    .ok_or(RegexCompileStatus::UnsupportedFeature)?;
                self.pos += 1;
            } else {
                break;
            }
        }

        if !has_min {
            self.pos = saved;
            return Ok(None);
        }

        match self.peek() {
            Some(c) if c == (b'}' as u16) => {
                self.pos += 1;
                if min_val > MAX_BOUNDED_REPEAT {
                    return Err(RegexCompileStatus::UnsupportedFeature);
                }
                Ok(Some((min_val, Some(min_val))))
            }
            Some(c) if c == (b',' as u16) => {
                self.pos += 1;
                if self.peek() == Some(b'}' as u16) {
                    self.pos += 1;
                    if min_val > MAX_BOUNDED_REPEAT {
                        return Err(RegexCompileStatus::UnsupportedFeature);
                    }
                    return Ok(Some((min_val, None)));
                }
                let mut max_val: usize = 0;
                let mut has_max = false;
                while let Some(d) = self.peek() {
                    if (b'0' as u16..=b'9' as u16).contains(&d) {
                        has_max = true;
                        max_val = max_val
                            .checked_mul(10)
                            .and_then(|v| v.checked_add((d - b'0' as u16) as usize))
                            .ok_or(RegexCompileStatus::UnsupportedFeature)?;
                        self.pos += 1;
                    } else {
                        break;
                    }
                }
                if has_max && self.peek() == Some(b'}' as u16) {
                    self.pos += 1;
                    if max_val < min_val {
                        return Err(RegexCompileStatus::InvalidSyntax);
                    }
                    if max_val > MAX_BOUNDED_REPEAT {
                        return Err(RegexCompileStatus::UnsupportedFeature);
                    }
                    return Ok(Some((min_val, Some(max_val))));
                }
                self.pos = saved;
                Ok(None)
            }
            _ => {
                self.pos = saved;
                Ok(None)
            }
        }
    }

    fn parse_alternation(&mut self, depth: usize) -> Result<RegexAst, RegexCompileStatus> {
        if depth > MAX_PARSE_DEPTH {
            return Err(RegexCompileStatus::UnsupportedFeature);
        }
        let mut branches = Vec::new();
        branches.push(self.parse_concat(depth)?);
        while self.peek() == Some(b'|' as u16) {
            self.pos += 1;
            branches.push(self.parse_concat(depth)?);
        }
        if branches.len() == 1 {
            Ok(branches.pop().unwrap_or(RegexAst::Empty))
        } else {
            Ok(RegexAst::Alt(branches))
        }
    }

    fn parse_concat(&mut self, depth: usize) -> Result<RegexAst, RegexCompileStatus> {
        let mut seq = Vec::new();
        while let Some(ch) = self.peek() {
            if ch == (b'|' as u16) || ch == (b')' as u16) {
                break;
            }
            let atom = match ch as u32 {
                x if x == b'^' as u32 => {
                    self.pos += 1;
                    RegexAst::AnchorStart
                }
                x if x == b'$' as u32 => {
                    self.pos += 1;
                    RegexAst::AnchorEnd
                }
                x if x == b'.' as u32 => {
                    self.pos += 1;
                    RegexAst::AnyChar
                }
                x if x == b'\\' as u32 => {
                    self.pos += 1;
                    self.parse_escape_atom()?
                }
                x if x == b'[' as u32 => {
                    self.pos += 1;
                    self.parse_char_class()?
                }
                x if x == b'(' as u32 => {
                    self.pos += 1;
                    if self.peek() == Some(b'?' as u16) {
                        self.pos += 1;
                        if self.peek() == Some(b':' as u16) {
                            self.pos += 1;
                        } else {
                            // Lookaround (?=...), (?!...), (?<=...), inline flags, etc.
                            return Err(RegexCompileStatus::UnsupportedFeature);
                        }
                    }
                    let sub = self.parse_alternation(depth + 1)?;
                    if self.next() != Some(b')' as u16) {
                        return Err(RegexCompileStatus::InvalidSyntax);
                    }
                    sub
                }
                x if x == b'*' as u32 || x == b'+' as u32 || x == b'?' as u32 => {
                    return Err(RegexCompileStatus::InvalidSyntax);
                }
                _ => {
                    self.pos += 1;
                    RegexAst::Literal(self.normalize_char(ch))
                }
            };

            let quantified = match self.peek() {
                Some(q) if q == (b'*' as u16) => {
                    self.pos += 1;
                    if self.peek() == Some(b'?' as u16) {
                        self.pos += 1;
                    }
                    RegexAst::Repeat {
                        node: Box::new(atom),
                        min: 0,
                        max: None,
                    }
                }
                Some(q) if q == (b'+' as u16) => {
                    self.pos += 1;
                    if self.peek() == Some(b'?' as u16) {
                        self.pos += 1;
                    }
                    RegexAst::Repeat {
                        node: Box::new(atom),
                        min: 1,
                        max: None,
                    }
                }
                Some(q) if q == (b'?' as u16) => {
                    self.pos += 1;
                    if self.peek() == Some(b'?' as u16) {
                        self.pos += 1;
                    }
                    RegexAst::Repeat {
                        node: Box::new(atom),
                        min: 0,
                        max: Some(1),
                    }
                }
                Some(q) if q == (b'{' as u16) => {
                    self.pos += 1;
                    if let Some((min_r, max_r)) = self.try_parse_braced_quantifier()? {
                        if self.peek() == Some(b'?' as u16) {
                            self.pos += 1;
                        }
                        RegexAst::Repeat {
                            node: Box::new(atom),
                            min: min_r,
                            max: max_r,
                        }
                    } else {
                        // Treat unclosed/literal '{' as literal char in ECMAScript relaxed grammar
                        seq.push(atom);
                        RegexAst::Literal(b'{' as u16)
                    }
                }
                _ => atom,
            };

            // Reject invalid double quantifiers like a** or a+*
            if matches!(quantified, RegexAst::Repeat { .. }) {
                if let Some(next_q) = self.peek() {
                    if next_q == (b'*' as u16) || next_q == (b'+' as u16) || next_q == (b'?' as u16) {
                        return Err(RegexCompileStatus::InvalidSyntax);
                    }
                }
            }

            seq.push(quantified);
        }

        if seq.is_empty() {
            Ok(RegexAst::Empty)
        } else if seq.len() == 1 {
            Ok(seq.pop().unwrap_or(RegexAst::Empty))
        } else {
            Ok(RegexAst::Concat(seq))
        }
    }
}

#[derive(Clone, Debug)]
enum NfaState {
    Accept,
    Jump(usize),
    Split(usize, usize),
    AnchorStart(usize),
    AnchorEnd(usize),
    WordBoundary {
        negated: bool,
        next: usize,
    },
    AnyChar(usize),
    Literal {
        ch: u16,
        next: usize,
    },
    Builtin {
        mask: u8,
        next: usize,
    },
    CharClass {
        ranges: Vec<(u16, u16)>,
        builtins: u8,
        negated: bool,
        next: usize,
    },
}

pub struct LinearUtf16Regex {
    states: Vec<NfaState>,
    start: usize,
    accept: usize,
    case_sensitive: bool,
}

impl LinearUtf16Regex {
    pub fn compile(pattern: &[u16], case_sensitive: bool) -> Result<Self, RegexCompileStatus> {
        let mut parser = RegexParser::new(pattern, case_sensitive);
        let ast = parser.parse_alternation(0)?;
        if parser.pos != pattern.len() {
            return Err(RegexCompileStatus::InvalidSyntax);
        }

        let mut builder = NfaBuilder { states: Vec::new() };
        let accept = builder.push_state(NfaState::Accept)?;
        let start = builder.emit(&ast, accept)?;

        Ok(Self {
            states: builder.states,
            start,
            accept,
            case_sensitive,
        })
    }

    #[inline]
    fn add_epsilon_closure(
        &self,
        entry_state: usize,
        pos: usize,
        text: &[u16],
        set: &mut [u64; NFA_WORDS],
    ) {
        let mut stack = [0usize; 128];
        let mut sp = 0usize;
        stack[0] = entry_state;
        sp += 1;

        while sp > 0 {
            sp -= 1;
            let sid = stack[sp];
            let word_idx = sid >> 6;
            let bit_mask = 1u64 << (sid & 63);
            if word_idx >= NFA_WORDS || (set[word_idx] & bit_mask) != 0 {
                continue;
            }
            set[word_idx] |= bit_mask;

            let Some(st) = self.states.get(sid) else {
                continue;
            };
            match *st {
                NfaState::Jump(target) => {
                    if sp < stack.len() {
                        stack[sp] = target;
                        sp += 1;
                    } else {
                        self.add_epsilon_closure_slow(target, pos, text, set);
                    }
                }
                NfaState::Split(a, b) => {
                    if sp + 2 <= stack.len() {
                        stack[sp] = b;
                        stack[sp + 1] = a;
                        sp += 2;
                    } else {
                        self.add_epsilon_closure_slow(a, pos, text, set);
                        self.add_epsilon_closure_slow(b, pos, text, set);
                    }
                }
                NfaState::AnchorStart(target) => {
                    if pos == 0 {
                        if sp < stack.len() {
                            stack[sp] = target;
                            sp += 1;
                        } else {
                            self.add_epsilon_closure_slow(target, pos, text, set);
                        }
                    }
                }
                NfaState::AnchorEnd(target) => {
                    if pos == text.len() {
                        if sp < stack.len() {
                            stack[sp] = target;
                            sp += 1;
                        } else {
                            self.add_epsilon_closure_slow(target, pos, text, set);
                        }
                    }
                }
                NfaState::WordBoundary { negated, next } => {
                    let prev_word = pos > 0 && is_word_u16(text[pos - 1]);
                    let curr_word = pos < text.len() && is_word_u16(text[pos]);
                    let is_boundary = prev_word != curr_word;
                    if is_boundary != negated {
                        if sp < stack.len() {
                            stack[sp] = next;
                            sp += 1;
                        } else {
                            self.add_epsilon_closure_slow(next, pos, text, set);
                        }
                    }
                }
                _ => {}
            }
        }
    }

    fn add_epsilon_closure_slow(
        &self,
        entry_state: usize,
        pos: usize,
        text: &[u16],
        set: &mut [u64; NFA_WORDS],
    ) {
        let mut stack = Vec::with_capacity(256);
        stack.push(entry_state);
        while let Some(sid) = stack.pop() {
            let word_idx = sid >> 6;
            let bit_mask = 1u64 << (sid & 63);
            if word_idx >= NFA_WORDS || (set[word_idx] & bit_mask) != 0 {
                continue;
            }
            set[word_idx] |= bit_mask;
            let Some(st) = self.states.get(sid) else {
                continue;
            };
            match *st {
                NfaState::Jump(target) => stack.push(target),
                NfaState::Split(a, b) => {
                    stack.push(b);
                    stack.push(a);
                }
                NfaState::AnchorStart(target) if pos == 0 => stack.push(target),
                NfaState::AnchorEnd(target) if pos == text.len() => stack.push(target),
                NfaState::WordBoundary { negated, next } => {
                    let prev_word = pos > 0 && is_word_u16(text[pos - 1]);
                    let curr_word = pos < text.len() && is_word_u16(text[pos]);
                    if (prev_word != curr_word) != negated {
                        stack.push(next);
                    }
                }
                _ => {}
            }
        }
    }

    #[inline]
    fn char_in_ranges(ranges: &[(u16, u16)], probe: u16) -> bool {
        let mut lo = 0usize;
        let mut hi = ranges.len();
        while lo < hi {
            let mid = lo + ((hi - lo) >> 1);
            let (start, end) = ranges[mid];
            if probe < start {
                hi = mid;
            } else if probe > end {
                lo = mid + 1;
            } else {
                return true;
            }
        }
        false
    }

    pub fn is_match(&self, text: &[u16]) -> bool {
        let accept_word = self.accept >> 6;
        let accept_mask = 1u64 << (self.accept & 63);
        let active_words = (self.states.len() + 63) >> 6;

        let mut curr = [0u64; NFA_WORDS];
        let mut next = [0u64; NFA_WORDS];

        for pos in 0..=text.len() {
            self.add_epsilon_closure(self.start, pos, text, &mut curr);
            if (curr[accept_word] & accept_mask) != 0 {
                return true;
            }
            if pos == text.len() {
                break;
            }

            let raw_ch = text[pos];
            let cmp_ch = if self.case_sensitive {
                raw_ch
            } else {
                fold_u16_case(raw_ch)
            };

            for w in 0..active_words {
                let mut bits = curr[w];
                while bits != 0 {
                    let tz = bits.trailing_zeros() as usize;
                    bits &= bits - 1;
                    let sid = (w << 6) | tz;
                    let Some(st) = self.states.get(sid) else {
                        continue;
                    };
                    let target_opt = match st {
                        NfaState::AnyChar(nx) => {
                            if raw_ch != (b'\n' as u16) && raw_ch != (b'\r' as u16) {
                                Some(*nx)
                            } else {
                                None
                            }
                        }
                        NfaState::Literal { ch, next: nx } => {
                            if cmp_ch == *ch {
                                Some(*nx)
                            } else {
                                None
                            }
                        }
                        NfaState::Builtin { mask, next: nx } => {
                            if matches_builtin_mask(*mask, raw_ch) {
                                Some(*nx)
                            } else {
                                None
                            }
                        }
                        NfaState::CharClass {
                            ranges,
                            builtins,
                            negated,
                            next: nx,
                        } => {
                            let hit = Self::char_in_ranges(ranges, cmp_ch)
                                || matches_builtin_mask(*builtins, raw_ch);
                            if hit != *negated {
                                Some(*nx)
                            } else {
                                None
                            }
                        }
                        _ => None,
                    };

                    if let Some(target) = target_opt {
                        self.add_epsilon_closure(target, pos + 1, text, &mut next);
                    }
                }
                curr[w] = 0;
            }

            core::mem::swap(&mut curr, &mut next);
        }

        false
    }
}

struct NfaBuilder {
    states: Vec<NfaState>,
}

impl NfaBuilder {
    fn push_state(&mut self, state: NfaState) -> Result<usize, RegexCompileStatus> {
        if self.states.len() >= MAX_NFA_STATES {
            return Err(RegexCompileStatus::UnsupportedFeature);
        }
        let id = self.states.len();
        self.states.push(state);
        Ok(id)
    }

    fn emit(&mut self, ast: &RegexAst, out: usize) -> Result<usize, RegexCompileStatus> {
        match ast {
            RegexAst::Empty => Ok(out),
            RegexAst::AnchorStart => self.push_state(NfaState::AnchorStart(out)),
            RegexAst::AnchorEnd => self.push_state(NfaState::AnchorEnd(out)),
            RegexAst::WordBoundary { negated } => self.push_state(NfaState::WordBoundary {
                negated: *negated,
                next: out,
            }),
            RegexAst::AnyChar => self.push_state(NfaState::AnyChar(out)),
            RegexAst::Literal(ch) => self.push_state(NfaState::Literal { ch: *ch, next: out }),
            RegexAst::Builtin(mask) => self.push_state(NfaState::Builtin {
                mask: *mask,
                next: out,
            }),
            RegexAst::CharClass {
                ranges,
                builtins,
                negated,
            } => self.push_state(NfaState::CharClass {
                ranges: ranges.clone(),
                builtins: *builtins,
                negated: *negated,
                next: out,
            }),
            RegexAst::Concat(items) => {
                let mut next_state = out;
                for item in items.iter().rev() {
                    next_state = self.emit(item, next_state)?;
                }
                Ok(next_state)
            }
            RegexAst::Alt(branches) => {
                let mut iter = branches.iter().rev();
                let Some(last_branch) = iter.next() else {
                    return Ok(out);
                };
                let mut acc = self.emit(last_branch, out)?;
                for branch in iter {
                    let left = self.emit(branch, out)?;
                    acc = self.push_state(NfaState::Split(left, acc))?;
                }
                Ok(acc)
            }
            RegexAst::Repeat { node, min, max } => match max {
                None => {
                    // min copies followed by a Kleene star loop
                    let split_id = self.push_state(NfaState::Jump(out))?;
                    let body_entry = self.emit(node, split_id)?;
                    // Guard against empty-matching infinite loop: Split(body_entry, out) is visited-set guarded!
                    self.states[split_id] = NfaState::Split(body_entry, out);
                    let mut entry = split_id;
                    for _ in 0..*min {
                        entry = self.emit(node, entry)?;
                    }
                    Ok(entry)
                }
                Some(max_count) => {
                    let mut entry = out;
                    let opt_copies = max_count.saturating_sub(*min);
                    for _ in 0..opt_copies {
                        let sub = self.emit(node, entry)?;
                        entry = self.push_state(NfaState::Split(sub, entry))?;
                    }
                    for _ in 0..*min {
                        entry = self.emit(node, entry)?;
                    }
                    Ok(entry)
                }
            },
        }
    }
}
