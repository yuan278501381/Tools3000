// Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors.
// All rights reserved. | MIT License
//
// C ABI data contracts and FFI entrypoints for Tools3000_Service and Tools3000Tests.

use std::slice;

use crate::constants::{
    CANCEL_CHECK_BYTE_INTERVAL, CANCEL_CHECK_DOC_LINE_MASK, CANCEL_CHECK_LINE_MASK,
    CANCEL_CHECK_UTF16_UNIT_MASK, HUGE_LINE_BYTE_THRESHOLD, HUGE_LINE_WINDOW_RADIUS,
    INITIAL_FOLD_BUFFER_CAPACITY,
};
use crate::encoding::{detect_encoding_slice, read_utf16_unit, T3kDetectedEncoding};
use crate::matcher::RustMultiTermMatcher;
use crate::simd::{find_ascii_bytes_simd, find_byte_simd, find_exact_bytes_simd};

#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct T3kUtf16Slice {
    pub ptr: *const u16,
    pub len: usize,
}

#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct T3kByteSlice {
    pub ptr: *const u8,
    pub len: usize,
}

#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct T3kByteTermVariants {
    pub is_ascii: u8,
    pub ascii_ptr: *const u8,
    pub ascii_len: usize,
    pub variants_ptr: *const T3kByteSlice,
    pub variants_count: usize,
}

pub type T3kCancelCheckFn = Option<extern "C" fn(*const std::ffi::c_void) -> u8>;
pub type T3kFallbackDecodeLineFn = Option<
    extern "C" fn(
        *const std::ffi::c_void,
        *const u8,
        usize,
        u32,
        *mut u16,
        usize,
    ) -> usize,
>;
pub type T3kEmitSnippetFn = Option<
    extern "C" fn(
        *mut std::ffi::c_void,
        u32,
        *const u16,
        usize,
        u32,
        u32,
    ),
>;

#[no_mangle]
pub unsafe extern "C" fn t3k_rs_detect_encoding(
    data_ptr: *const u8,
    len: usize,
    out_bom_offset: *mut usize,
) -> u32 {
    if !out_bom_offset.is_null() {
        *out_bom_offset = 0;
    }
    if data_ptr.is_null() || len == 0 {
        return T3kDetectedEncoding::UnknownBinary as u32;
    }
    let slice = slice::from_raw_parts(data_ptr, len);
    let (enc, bom) = detect_encoding_slice(slice);
    if !out_bom_offset.is_null() {
        *out_bom_offset = bom;
    }
    enc as u32
}

#[no_mangle]
pub unsafe extern "C" fn t3k_rs_find_ascii_pattern(
    data_ptr: *const u8,
    data_len: usize,
    pat_ptr: *const u8,
    pat_len: usize,
    case_sensitive: u8,
) -> usize {
    if data_ptr.is_null() || pat_ptr.is_null() || data_len == 0 || pat_len == 0 || data_len < pat_len
    {
        return usize::MAX;
    }
    let hay = slice::from_raw_parts(data_ptr, data_len);
    let needle = slice::from_raw_parts(pat_ptr, pat_len);
    find_ascii_bytes_simd(hay, needle, case_sensitive != 0).unwrap_or(usize::MAX)
}

#[no_mangle]
pub unsafe extern "C" fn t3k_rs_find_exact_bytes(
    data_ptr: *const u8,
    data_len: usize,
    pat_ptr: *const u8,
    pat_len: usize,
) -> usize {
    if data_ptr.is_null() || pat_ptr.is_null() || data_len == 0 || pat_len == 0 || data_len < pat_len
    {
        return usize::MAX;
    }
    let hay = slice::from_raw_parts(data_ptr, data_len);
    let needle = slice::from_raw_parts(pat_ptr, pat_len);
    find_exact_bytes_simd(hay, needle).unwrap_or(usize::MAX)
}

#[no_mangle]
pub unsafe extern "C" fn t3k_rs_match_utf16_document(
    text_ptr: *const u16,
    text_len: usize,
    terms_ptr: *const T3kUtf16Slice,
    terms_count: usize,
    case_sensitive: u8,
    max_snippets: usize,
    cancel_fn: T3kCancelCheckFn,
    cancel_ctx: *const std::ffi::c_void,
    emit_fn: T3kEmitSnippetFn,
    emit_ctx: *mut std::ffi::c_void,
) -> u8 {
    if text_ptr.is_null() || text_len == 0 || terms_ptr.is_null() || terms_count == 0 {
        return 0;
    }
    let full_text = slice::from_raw_parts(text_ptr, text_len);
    let raw_term_structs = slice::from_raw_parts(terms_ptr, terms_count);
    let mut term_slices: Vec<&[u16]> = Vec::with_capacity(terms_count);
    for ts in raw_term_structs {
        if !ts.ptr.is_null() && ts.len > 0 {
            term_slices.push(slice::from_raw_parts(ts.ptr, ts.len));
        }
    }

    let mut matcher = RustMultiTermMatcher::new(&term_slices, case_sensitive != 0, max_snippets);
    if !matcher.has_valid_terms() {
        return 0;
    }

    let mut line_number: u32 = 1;
    let mut line_start: usize = 0;
    let nl = b'\n' as u16;

    for i in 0..=full_text.len() {
        if i == full_text.len() || full_text[i] == nl {
            if (line_number & CANCEL_CHECK_DOC_LINE_MASK) == 0 {
                if let Some(chk) = cancel_fn {
                    if chk(cancel_ctx) != 0 {
                        return 0;
                    }
                }
            }
            let line_view = &full_text[line_start..i];
            if !line_view.is_empty() {
                matcher.process_line(line_number, line_view);
                if matcher.can_stop_early() {
                    break;
                }
            }
            line_start = i + 1;
            line_number = line_number.saturating_add(1);
        }
    }

    u8::from(matcher.finalize(emit_fn, emit_ctx))
}

#[inline]
unsafe fn line_find_term_byte_offset(
    line_bytes: &[u8],
    term_byte_info: &T3kByteTermVariants,
    case_sensitive: bool,
) -> Option<usize> {
    if term_byte_info.is_ascii != 0 {
        if term_byte_info.ascii_ptr.is_null() || term_byte_info.ascii_len == 0 {
            return None;
        }
        let ascii_pat = slice::from_raw_parts(term_byte_info.ascii_ptr, term_byte_info.ascii_len);
        return find_ascii_bytes_simd(line_bytes, ascii_pat, case_sensitive);
    }

    if term_byte_info.variants_ptr.is_null() || term_byte_info.variants_count == 0 {
        return None;
    }
    let variants =
        slice::from_raw_parts(term_byte_info.variants_ptr, term_byte_info.variants_count);
    let mut best: Option<usize> = None;
    for v in variants {
        if v.ptr.is_null() || v.len == 0 {
            continue;
        }
        let pat = slice::from_raw_parts(v.ptr, v.len);
        if let Some(off) = find_exact_bytes_simd(line_bytes, pat) {
            best = Some(best.map_or(off, |cur| cur.min(off)));
            if case_sensitive {
                break;
            }
        }
    }
    best
}

fn decode_bytes_to_u16_with_fallback(
    bytes: &[u8],
    encoding: u32,
    out_buf: &mut Vec<u16>,
    fallback_decode_fn: T3kFallbackDecodeLineFn,
    fallback_ctx: *const std::ffi::c_void,
) {
    out_buf.clear();
    if bytes.is_empty() {
        return;
    }

    // 1. Pure ASCII fast-path (zero syscall, zero intermediate allocation)
    if bytes.iter().all(|&b| b < 0x80) {
        out_buf.extend(bytes.iter().map(|&b| b as u16));
        return;
    }

    // 2. Native Rust UTF-8 fast-path for Utf8 and MixedText
    if encoding == (T3kDetectedEncoding::Utf8 as u32)
        || encoding == (T3kDetectedEncoding::MixedText as u32)
    {
        if let Ok(s) = core::str::from_utf8(bytes) {
            out_buf.extend(s.encode_utf16());
            return;
        }
    }

    // 3. Fallback to Win32 MultiByteToWideChar (CP936 / ACP) for GBK or mixed non-UTF8 lines
    if let Some(cb) = fallback_decode_fn {
        out_buf.resize(bytes.len() + 4, 0);
        let written = cb(
            fallback_ctx,
            bytes.as_ptr(),
            bytes.len(),
            encoding,
            out_buf.as_mut_ptr(),
            out_buf.len(),
        );
        out_buf.truncate(written);
    }
}

/// Scans a memory-mapped byte buffer (UTF-8 / GBK / MixedText / UTF-16LE / UTF-16BE)
/// with SIMD newline leapfrogging and zero line-by-line heap allocations for non-matching lines.
#[no_mangle]
pub unsafe extern "C" fn t3k_rs_scan_mapped_text(
    body_ptr: *const u8,
    body_len: usize,
    encoding: u32,
    u16_terms_ptr: *const T3kUtf16Slice,
    byte_terms_ptr: *const T3kByteTermVariants,
    terms_count: usize,
    case_sensitive: u8,
    max_snippets: usize,
    cancel_fn: T3kCancelCheckFn,
    cancel_ctx: *const std::ffi::c_void,
    fallback_decode_fn: T3kFallbackDecodeLineFn,
    fallback_ctx: *const std::ffi::c_void,
    emit_fn: T3kEmitSnippetFn,
    emit_ctx: *mut std::ffi::c_void,
) -> u8 {
    if body_ptr.is_null() || body_len == 0 || u16_terms_ptr.is_null() || terms_count == 0 {
        return 0;
    }

    let body = slice::from_raw_parts(body_ptr, body_len);
    let raw_u16_terms = slice::from_raw_parts(u16_terms_ptr, terms_count);
    let mut term_slices: Vec<&[u16]> = Vec::with_capacity(terms_count);
    for ts in raw_u16_terms {
        if !ts.ptr.is_null() && ts.len > 0 {
            term_slices.push(slice::from_raw_parts(ts.ptr, ts.len));
        }
    }

    let cs = case_sensitive != 0;
    let mut matcher = RustMultiTermMatcher::new(&term_slices, cs, max_snippets);
    if !matcher.has_valid_terms() {
        return 0;
    }

    let mut line_u16_buf: Vec<u16> = Vec::with_capacity(INITIAL_FOLD_BUFFER_CAPACITY);

    // Branch A: UTF-16LE / UTF-16BE
    if encoding == (T3kDetectedEncoding::Utf16Le as u32)
        || encoding == (T3kDetectedEncoding::Utf16Be as u32)
    {
        let is_be = encoding == (T3kDetectedEncoding::Utf16Be as u32);
        let w_count = body_len / 2;
        if w_count == 0 {
            return 0;
        }
        let mut line_number: u32 = 1;
        let mut line_start: usize = 0;

        for i in 0..w_count {
            if (i & CANCEL_CHECK_UTF16_UNIT_MASK) == 0 {
                if let Some(chk) = cancel_fn {
                    if chk(cancel_ctx) != 0 {
                        return 0;
                    }
                }
            }
            let ch = read_utf16_unit(body, i, is_be);
            if ch == (b'\n' as u16) || i == w_count - 1 {
                if (line_number & CANCEL_CHECK_LINE_MASK) == 0 {
                    if let Some(chk) = cancel_fn {
                        if chk(cancel_ctx) != 0 {
                            return 0;
                        }
                    }
                }
                let line_end = if ch == (b'\n' as u16) { i } else { i + 1 };
                if line_end > line_start {
                    line_u16_buf.clear();
                    line_u16_buf.reserve(line_end - line_start);
                    for k in line_start..line_end {
                        line_u16_buf.push(read_utf16_unit(body, k, is_be));
                    }
                    matcher.process_line(line_number, &line_u16_buf);
                    if matcher.can_stop_early() {
                        break;
                    }
                }
                line_start = i + 1;
                line_number = line_number.saturating_add(1);
            }
        }
        return u8::from(matcher.finalize(emit_fn, emit_ctx));
    }

    // Branch B: UTF-8 / GBK / MixedText with SIMD newline scan + byte-level candidate line filter!
    let byte_terms = if !byte_terms_ptr.is_null() {
        Some(slice::from_raw_parts(byte_terms_ptr, terms_count))
    } else {
        None
    };

    let mut line_number: u32 = 1;
    let mut line_start: usize = 0;
    let mut last_cancel_check_offset: usize = 0;

    while line_start < body_len {
        if body_len - line_start > 0
            && (line_start - last_cancel_check_offset) >= CANCEL_CHECK_BYTE_INTERVAL
        {
            last_cancel_check_offset = line_start;
            if let Some(chk) = cancel_fn {
                if chk(cancel_ctx) != 0 {
                    return 0;
                }
            }
        }
        if (line_number & CANCEL_CHECK_LINE_MASK) == 0 {
            if let Some(chk) = cancel_fn {
                if chk(cancel_ctx) != 0 {
                    return 0;
                }
            }
        }

        let nl_rel = find_byte_simd(&body[line_start..], b'\n');
        let (line_end, next_start) = match nl_rel {
            Some(rel) => (line_start + rel, line_start + rel + 1),
            None => (body_len, body_len),
        };

        let line_len = line_end - line_start;
        if line_len > 0 {
            let line_bytes = &body[line_start..line_end];

            if line_len > HUGE_LINE_BYTE_THRESHOLD {
                if let Some(b_terms) = byte_terms {
                    for (t_idx, bt) in b_terms.iter().enumerate() {
                        if let Some(hit_off) = line_find_term_byte_offset(line_bytes, bt, cs) {
                            let term_u16_len = raw_u16_terms[t_idx].len;
                            let mut win_start = hit_off.saturating_sub(HUGE_LINE_WINDOW_RADIUS);
                            let mut win_end = (hit_off + term_u16_len * 4 + HUGE_LINE_WINDOW_RADIUS)
                                .min(line_len);

                            if encoding == (T3kDetectedEncoding::Utf8 as u32)
                                || encoding == (T3kDetectedEncoding::MixedText as u32)
                            {
                                let mut step = 0;
                                while step < 3
                                    && win_start < hit_off
                                    && (line_bytes[win_start] & 0xC0) == 0x80
                                {
                                    win_start += 1;
                                    step += 1;
                                }
                                while win_end < line_len && (line_bytes[win_end] & 0xC0) == 0x80 {
                                    win_end += 1;
                                }
                            } else if encoding == (T3kDetectedEncoding::Gbk as u32) && win_start > 0
                            {
                                let mut back = 0;
                                while back < 16 && win_start > 0 {
                                    if line_bytes[win_start] < 0x80 {
                                        break;
                                    }
                                    win_start -= 1;
                                    back += 1;
                                }
                            }

                            decode_bytes_to_u16_with_fallback(
                                &line_bytes[win_start..win_end],
                                encoding,
                                &mut line_u16_buf,
                                fallback_decode_fn,
                                fallback_ctx,
                            );
                            if !line_u16_buf.is_empty() {
                                matcher.process_line(line_number, &line_u16_buf);
                                if matcher.can_stop_early() {
                                    break;
                                }
                            }
                        }
                    }
                }
            } else {
                // Fast byte-level candidate check: skip decoding lines that cannot possibly match any term!
                let mut candidate_hit = true;
                if let Some(b_terms) = byte_terms {
                    candidate_hit = false;
                    for bt in b_terms {
                        if line_find_term_byte_offset(line_bytes, bt, cs).is_some() {
                            candidate_hit = true;
                            break;
                        }
                    }
                }

                if candidate_hit {
                    decode_bytes_to_u16_with_fallback(
                        line_bytes,
                        encoding,
                        &mut line_u16_buf,
                        fallback_decode_fn,
                        fallback_ctx,
                    );
                    if !line_u16_buf.is_empty() {
                        matcher.process_line(line_number, &line_u16_buf);
                    }
                }
            }

            if matcher.can_stop_early() {
                break;
            }
        }

        line_start = next_start;
        line_number = line_number.saturating_add(1);
    }

    u8::from(matcher.finalize(emit_fn, emit_ctx))
}
