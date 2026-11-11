// Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors.
// All rights reserved. | MIT License
//
// UTF-16 snippet sanitization, surrogate-safe windowing, and match offset alignment.

use crate::constants::{
    MAX_GARBLED_CHAR_RATIO, MAX_SNIPPET_LINE_CHARS, SNIPPET_CONTEXT_PADDING_CHARS,
};
use crate::encoding::{is_high_surrogate, is_low_surrogate};

#[derive(Clone, Debug)]
pub struct BuiltSnippet {
    pub line_number: u32,
    pub line_content: Vec<u16>,
    pub match_offset: u32,
    pub match_length: u32,
}

fn sanitize_and_validate_snippet_line(line: &mut [u16]) -> bool {
    if line.is_empty() {
        return false;
    }
    let len = line.len();
    let mut i = 0usize;
    while i < len {
        let ch = line[i];
        if ch == 0x0000 || (ch < 0x20 && ch != b'\t' as u16) {
            line[i] = b' ' as u16;
        } else if is_high_surrogate(ch) {
            if i + 1 < len && is_low_surrogate(line[i + 1]) {
                i += 1;
            } else {
                line[i] = 0xFFFD;
            }
        } else if is_low_surrogate(ch) {
            line[i] = 0xFFFD;
        }
        i += 1;
    }

    let mut bad_chars = 0usize;
    for &ch in line.iter() {
        if ch == 0xFFFD || ch == 0x0000 || (ch < 0x20 && ch != b'\t' as u16) {
            bad_chars += 1;
        }
    }
    ((bad_chars as f64) / (len as f64)) <= MAX_GARBLED_CHAR_RATIO
}

pub fn build_aligned_snippet(
    line_number: u32,
    raw_line: &[u16],
    match_pos: usize,
    match_len: usize,
) -> Option<BuiltSnippet> {
    let match_end = match_pos.checked_add(match_len)?;
    if raw_line.is_empty() || match_len == 0 || match_end > raw_line.len() {
        return None;
    }

    let mut line = raw_line.to_vec();
    if !sanitize_and_validate_snippet_line(&mut line) {
        return None;
    }

    let mut snippet_offset = match_pos.min(u32::MAX as usize) as u32;
    if line.len() > MAX_SNIPPET_LINE_CHARS {
        let mut start = match_pos.saturating_sub(SNIPPET_CONTEXT_PADDING_CHARS);
        if start > 0 && start < line.len() && is_low_surrogate(line[start]) {
            start -= 1;
        }

        let mut end = match_end
            .saturating_add(SNIPPET_CONTEXT_PADDING_CHARS)
            .min(line.len());
        if end < line.len() && end > 0 && is_high_surrogate(line[end - 1]) {
            end += 1;
        }
        if start > end || end > line.len() {
            return None;
        }

        let slice_part = line.get(start..end)?;
        let mut truncated = Vec::with_capacity(slice_part.len().saturating_add(8));
        let dot = b'.' as u16;
        if start > 0 {
            truncated.extend_from_slice(&[dot, dot, dot]);
        }
        snippet_offset = truncated
            .len()
            .saturating_add(match_pos.saturating_sub(start))
            .min(u32::MAX as usize) as u32;
        truncated.extend_from_slice(slice_part);
        if end < line.len() {
            truncated.extend_from_slice(&[dot, dot, dot]);
        }
        line = truncated;
    }

    Some(BuiltSnippet {
        line_number,
        line_content: line,
        match_offset: snippet_offset,
        match_length: match_len.min(u32::MAX as usize) as u32,
    })
}
