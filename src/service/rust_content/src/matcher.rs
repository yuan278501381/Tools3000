// Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors.
// All rights reserved. | MIT License
//
// Zero-line-allocation multi-term AND document matcher with reusable UTF-16 case-fold buffer.

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
            && (nlen <= 2 || &haystack[i + 1..i + nlen - 1] == &needle[1..nlen - 1])
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
