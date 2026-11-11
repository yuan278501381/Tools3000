// Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors.
// All rights reserved. | MIT License
//
// C ABI data contracts and panic-free FFI entrypoints for Tools3000_Service and Tools3000Tests.

use std::borrow::Cow;
use std::mem::{align_of, size_of};
use std::ptr;
use std::slice;

use crate::constants::{
    CANCEL_CHECK_BYTE_INTERVAL, CANCEL_CHECK_DOC_LINE_MASK, CANCEL_CHECK_LINE_MASK,
    CANCEL_CHECK_UTF16_UNIT_MASK, HUGE_LINE_BYTE_THRESHOLD, HUGE_LINE_WINDOW_RADIUS,
    INITIAL_FOLD_BUFFER_CAPACITY,
};
use crate::encoding::{detect_encoding_slice, read_utf16_unit, T3kDetectedEncoding};
use crate::matcher::{LinearUtf16Regex, RegexCompileStatus, RustMultiTermMatcher};
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

pub struct T3kRegexHandle {
    inner: LinearUtf16Regex,
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

#[inline]
unsafe fn safe_u8_slice<'a>(ptr: *const u8, len: usize) -> Option<&'a [u8]> {
    if ptr.is_null() || len > (isize::MAX as usize) {
        return None;
    }
    if len == 0 {
        return Some(&[]);
    }
    Some(slice::from_raw_parts(ptr, len))
}

#[inline]
unsafe fn safe_copy_slice<'a, T: Copy>(ptr: *const T, len: usize) -> Option<Cow<'a, [T]>> {
    if ptr.is_null() {
        return None;
    }
    if len == 0 {
        return Some(Cow::Borrowed(&[]));
    }
    let byte_len = len.checked_mul(size_of::<T>())?;
    if byte_len > (isize::MAX as usize) {
        return None;
    }
    if (ptr as usize) % align_of::<T>() == 0 {
        Some(Cow::Borrowed(slice::from_raw_parts(ptr, len)))
    } else {
        let mut owned = Vec::with_capacity(len);
        for idx in 0..len {
            owned.push(ptr::read_unaligned(ptr.add(idx)));
        }
        Some(Cow::Owned(owned))
    }
}

#[no_mangle]
pub unsafe extern "C" fn t3k_rs_detect_encoding(
    data_ptr: *const u8,
    len: usize,
    out_bom_offset: *mut usize,
) -> u32 {
    if !out_bom_offset.is_null() && (out_bom_offset as usize) % align_of::<usize>() == 0 {
        *out_bom_offset = 0;
    } else if !out_bom_offset.is_null() {
        ptr::write_unaligned(out_bom_offset, 0);
    }
    let Some(slice) = safe_u8_slice(data_ptr, len) else {
        return T3kDetectedEncoding::UnknownBinary as u32;
    };
    if slice.is_empty() {
        return T3kDetectedEncoding::UnknownBinary as u32;
    }
    let (enc, bom) = detect_encoding_slice(slice);
    if !out_bom_offset.is_null() {
        if (out_bom_offset as usize) % align_of::<usize>() == 0 {
            *out_bom_offset = bom;
        } else {
            ptr::write_unaligned(out_bom_offset, bom);
        }
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
    if data_len == 0 || pat_len == 0 || data_len < pat_len {
        return usize::MAX;
    }
    let (Some(hay), Some(needle)) = (
        safe_u8_slice(data_ptr, data_len),
        safe_u8_slice(pat_ptr, pat_len),
    ) else {
        return usize::MAX;
    };
    find_ascii_bytes_simd(hay, needle, case_sensitive != 0).unwrap_or(usize::MAX)
}

#[no_mangle]
pub unsafe extern "C" fn t3k_rs_find_exact_bytes(
    data_ptr: *const u8,
    data_len: usize,
    pat_ptr: *const u8,
    pat_len: usize,
) -> usize {
    if data_len == 0 || pat_len == 0 || data_len < pat_len {
        return usize::MAX;
    }
    let (Some(hay), Some(needle)) = (
        safe_u8_slice(data_ptr, data_len),
        safe_u8_slice(pat_ptr, pat_len),
    ) else {
        return usize::MAX;
    };
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
    if text_len == 0 || terms_count == 0 {
        return 0;
    }
    let (Some(full_text_cow), Some(raw_term_structs)) = (
        safe_copy_slice(text_ptr, text_len),
        safe_copy_slice(terms_ptr, terms_count),
    ) else {
        return 0;
    };
    let full_text: &[u16] = &full_text_cow;

    let mut owned_terms: Vec<Cow<'_, [u16]>> = Vec::with_capacity(terms_count);
    for ts in raw_term_structs.iter() {
        if ts.len > 0 {
            if let Some(t_cow) = safe_copy_slice(ts.ptr, ts.len) {
                owned_terms.push(t_cow);
            }
        }
    }
    if owned_terms.is_empty() {
        return 0;
    }
    let term_slices: Vec<&[u16]> = owned_terms.iter().map(|c| c.as_ref()).collect();

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
            if let Some(line_view) = full_text.get(line_start..i) {
                if !line_view.is_empty() {
                    matcher.process_line(line_number, line_view);
                    if matcher.can_stop_early() {
                        break;
                    }
                }
            }
            line_start = i.saturating_add(1);
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
        if term_byte_info.ascii_len == 0 {
            return None;
        }
        let ascii_pat = safe_u8_slice(term_byte_info.ascii_ptr, term_byte_info.ascii_len)?;
        return find_ascii_bytes_simd(line_bytes, ascii_pat, case_sensitive);
    }

    if term_byte_info.variants_count == 0 {
        return None;
    }
    let variants = safe_copy_slice(term_byte_info.variants_ptr, term_byte_info.variants_count)?;
    let mut best: Option<usize> = None;
    for v in variants.iter() {
        if v.len == 0 {
            continue;
        }
        let Some(pat) = safe_u8_slice(v.ptr, v.len) else {
            continue;
        };
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
        let Some(cap) = bytes.len().checked_add(4) else {
            return;
        };
        out_buf.resize(cap, 0);
        let written = cb(
            fallback_ctx,
            bytes.as_ptr(),
            bytes.len(),
            encoding,
            out_buf.as_mut_ptr(),
            out_buf.len(),
        );
        let clamped = written.min(out_buf.len());
        out_buf.truncate(clamped);
    }
}

/// Scans a byte buffer (UTF-8 / GBK / MixedText / UTF-16LE / UTF-16BE)
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
    if body_len == 0 || terms_count == 0 {
        return 0;
    }

    let (Some(body), Some(raw_u16_terms)) = (
        safe_u8_slice(body_ptr, body_len),
        safe_copy_slice(u16_terms_ptr, terms_count),
    ) else {
        return 0;
    };

    let mut owned_terms: Vec<Cow<'_, [u16]>> = Vec::with_capacity(terms_count);
    for ts in raw_u16_terms.iter() {
        if ts.len > 0 {
            if let Some(t_cow) = safe_copy_slice(ts.ptr, ts.len) {
                owned_terms.push(t_cow);
            }
        }
    }
    if owned_terms.is_empty() {
        return 0;
    }
    let term_slices: Vec<&[u16]> = owned_terms.iter().map(|c| c.as_ref()).collect();

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
                line_start = i.saturating_add(1);
                line_number = line_number.saturating_add(1);
            }
        }
        return u8::from(matcher.finalize(emit_fn, emit_ctx));
    }

    // Branch B: UTF-8 / GBK / MixedText with SIMD newline scan + byte-level candidate line filter!
    let byte_terms_cow = if !byte_terms_ptr.is_null() {
        safe_copy_slice(byte_terms_ptr, terms_count)
    } else {
        None
    };
    let byte_terms = byte_terms_cow.as_deref();

    let mut line_number: u32 = 1;
    let mut line_start: usize = 0;
    let mut last_cancel_check_offset: usize = 0;

    while line_start < body_len {
        if line_start.saturating_sub(last_cancel_check_offset) >= CANCEL_CHECK_BYTE_INTERVAL {
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

        let Some(tail_slice) = body.get(line_start..) else {
            break;
        };
        let nl_rel = find_byte_simd(tail_slice, b'\n');
        let (line_end, next_start) = match nl_rel {
            Some(rel) => {
                let end = line_start.saturating_add(rel).min(body_len);
                (end, end.saturating_add(1).min(body_len))
            }
            None => (body_len, body_len),
        };

        let line_len = line_end.saturating_sub(line_start);
        if line_len > 0 {
            if let Some(line_bytes) = body.get(line_start..line_end) {
                if line_len > HUGE_LINE_BYTE_THRESHOLD {
                    if let Some(b_terms) = byte_terms {
                        for (t_idx, bt) in b_terms.iter().enumerate() {
                            if let Some(hit_off) = line_find_term_byte_offset(line_bytes, bt, cs) {
                                let term_u16_len =
                                    raw_u16_terms.get(t_idx).map_or(0, |s| s.len);
                                let mut win_start =
                                    hit_off.saturating_sub(HUGE_LINE_WINDOW_RADIUS);
                                let mut win_end = hit_off
                                    .saturating_add(term_u16_len.saturating_mul(4))
                                    .saturating_add(HUGE_LINE_WINDOW_RADIUS)
                                    .min(line_len);

                                if encoding == (T3kDetectedEncoding::Utf8 as u32)
                                    || encoding == (T3kDetectedEncoding::MixedText as u32)
                                {
                                    let mut step = 0;
                                    while step < 3
                                        && win_start < hit_off
                                        && line_bytes
                                            .get(win_start)
                                            .map_or(false, |&b| (b & 0xC0) == 0x80)
                                    {
                                        win_start += 1;
                                        step += 1;
                                    }
                                    while win_end < line_len
                                        && line_bytes
                                            .get(win_end)
                                            .map_or(false, |&b| (b & 0xC0) == 0x80)
                                    {
                                        win_end += 1;
                                    }
                                } else if encoding == (T3kDetectedEncoding::Gbk as u32)
                                    && win_start > 0
                                {
                                    let mut back = 0;
                                    while back < 16 && win_start > 0 {
                                        if line_bytes.get(win_start).map_or(true, |&b| b < 0x80) {
                                            break;
                                        }
                                        win_start -= 1;
                                        back += 1;
                                    }
                                }

                                if let Some(window_slice) = line_bytes.get(win_start..win_end) {
                                    decode_bytes_to_u16_with_fallback(
                                        window_slice,
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
                    }
                } else {
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
        }

        line_start = next_start;
        line_number = line_number.saturating_add(1);
    }

    u8::from(matcher.finalize(emit_fn, emit_ctx))
}

/// Compiles a UTF-16 regular expression into a linear-time O(N) Thompson NFA automaton.
/// Status written to `out_status`:
/// - 1: Compiled into `out_handle` (must be freed via `t3k_rs_regex_free`)
/// - 2: Valid or complex syntax requiring non-regular feature (e.g. backreference/lookaround)
/// - 0: Invalid regular expression syntax or null input
#[no_mangle]
pub unsafe extern "C" fn t3k_rs_regex_compile(
    pat_ptr: *const u16,
    pat_len: usize,
    case_sensitive: u8,
    out_status: *mut u32,
) -> *mut T3kRegexHandle {
    let write_status = |code: u32| {
        if !out_status.is_null() {
            if (out_status as usize) % align_of::<u32>() == 0 {
                *out_status = code;
            } else {
                ptr::write_unaligned(out_status, code);
            }
        }
    };

    if pat_ptr.is_null() && pat_len > 0 {
        write_status(RegexCompileStatus::InvalidSyntax as u32);
        return ptr::null_mut();
    }

    let pat_cow = if pat_len == 0 {
        Cow::Borrowed(&[][..])
    } else {
        let Some(s) = safe_copy_slice(pat_ptr, pat_len) else {
            write_status(RegexCompileStatus::InvalidSyntax as u32);
            return ptr::null_mut();
        };
        s
    };

    match LinearUtf16Regex::compile(&pat_cow, case_sensitive != 0) {
        Ok(compiled) => {
            write_status(RegexCompileStatus::Compiled as u32);
            Box::into_raw(Box::new(T3kRegexHandle { inner: compiled }))
        }
        Err(status) => {
            write_status(status as u32);
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn t3k_rs_regex_free(handle: *mut T3kRegexHandle) {
    if !handle.is_null() {
        drop(Box::from_raw(handle));
    }
}

#[no_mangle]
pub unsafe extern "C" fn t3k_rs_regex_is_match(
    handle: *const T3kRegexHandle,
    text_ptr: *const u16,
    text_len: usize,
) -> u8 {
    if handle.is_null() {
        return 0;
    }
    if text_len == 0 {
        return u8::from((&(*handle).inner).is_match(&[]));
    }
    let Some(text_cow) = safe_copy_slice(text_ptr, text_len) else {
        return 0;
    };
    u8::from((&(*handle).inner).is_match(&text_cow))
}
