// Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors.
// All rights reserved. | MIT License
//
// Tools3000 Rust Content Search Kernel
// Modular architecture:
// - constants: Immutable thresholds, buffer budgets, and cancellation masks
// - encoding:  BOM / UTF-8 / GBK / UTF-16LE / UTF-16BE detection
// - simd:      SSE2 / scalar SIMD byte & case-insensitive ASCII search primitives
// - snippet:   UTF-16 snippet sanitization and surrogate-safe context windowing
// - matcher:   Zero-line-allocation multi-term AND matcher & linear-time O(N) regex engine
// - ffi:       C ABI exports for Tools3000_Service and Tools3000Tests

#![allow(clippy::missing_safety_doc)]

pub mod constants;
pub mod encoding;
pub mod ffi;
pub mod matcher;
pub mod simd;
pub mod snippet;

pub use encoding::T3kDetectedEncoding;
pub use ffi::{
    t3k_rs_detect_encoding, t3k_rs_find_ascii_pattern, t3k_rs_find_exact_bytes,
    t3k_rs_match_utf16_document, t3k_rs_regex_compile, t3k_rs_regex_free, t3k_rs_regex_is_match,
    t3k_rs_scan_mapped_text, T3kByteSlice, T3kByteTermVariants, T3kCancelCheckFn, T3kEmitSnippetFn,
    T3kFallbackDecodeLineFn, T3kRegexHandle, T3kUtf16Slice,
};

#[cfg(test)]
mod tests {
    use super::*;
    use std::ptr;
    use std::time::Instant;

    fn u16_vec(s: &str) -> Vec<u16> {
        s.encode_utf16().collect()
    }

    #[test]
    fn linear_regex_basic_and_case_folding() {
        let pat = u16_vec("^app_[0-9]+\\.log$");
        let mut status = 0u32;
        let handle = unsafe { t3k_rs_regex_compile(pat.as_ptr(), pat.len(), 0, &mut status) };
        assert_eq!(status, 1);
        assert!(!handle.is_null());

        let hit = u16_vec("APP_2026.LOG");
        let miss = u16_vec("readme.txt");
        unsafe {
            assert_eq!(t3k_rs_regex_is_match(handle, hit.as_ptr(), hit.len()), 1);
            assert_eq!(t3k_rs_regex_is_match(handle, miss.as_ptr(), miss.len()), 0);
            t3k_rs_regex_free(handle);
        }
    }

    #[test]
    fn linear_regex_redos_immunity() {
        // Classic catastrophic backtracking pattern: (a+)+$ against 60 'a's + '!'
        let pat = u16_vec("(a+)+$");
        let mut status = 0u32;
        let handle = unsafe { t3k_rs_regex_compile(pat.as_ptr(), pat.len(), 1, &mut status) };
        assert_eq!(status, 1);
        assert!(!handle.is_null());

        let evil_input = u16_vec(&format!("{}!", "a".repeat(256)));
        let start = Instant::now();
        let matched =
            unsafe { t3k_rs_regex_is_match(handle, evil_input.as_ptr(), evil_input.len()) };
        let elapsed = start.elapsed();
        unsafe {
            t3k_rs_regex_free(handle);
        }
        assert_eq!(matched, 0);
        assert!(
            elapsed.as_millis() < 50,
            "ReDoS pattern took too long: {:?}",
            elapsed
        );
    }

    #[test]
    fn linear_regex_classes_alternation_and_quantifiers() {
        let pat = u16_vec(r"(?:report|summary)[_\-\s]+[0-9]{2,4}\.(?:pdf|docx?)$");
        let mut status = 0u32;
        let handle = unsafe { t3k_rs_regex_compile(pat.as_ptr(), pat.len(), 0, &mut status) };
        assert_eq!(status, 1);

        let ok1 = u16_vec("Quarterly_Summary-2026.docx");
        let ok2 = u16_vec("report 99.pdf");
        let bad = u16_vec("report_1.pdf");
        unsafe {
            assert_eq!(t3k_rs_regex_is_match(handle, ok1.as_ptr(), ok1.len()), 1);
            assert_eq!(t3k_rs_regex_is_match(handle, ok2.as_ptr(), ok2.len()), 1);
            assert_eq!(t3k_rs_regex_is_match(handle, bad.as_ptr(), bad.len()), 0);
            t3k_rs_regex_free(handle);
        }
    }

    #[test]
    fn linear_regex_invalid_and_unsupported_classification() {
        let broken = u16_vec("[unclosed");
        let mut status = 99u32;
        let h1 = unsafe { t3k_rs_regex_compile(broken.as_ptr(), broken.len(), 0, &mut status) };
        assert!(h1.is_null());
        assert_eq!(status, 0);

        let backreference = u16_vec(r"(foo)\1");
        let h2 = unsafe {
            t3k_rs_regex_compile(backreference.as_ptr(), backreference.len(), 0, &mut status)
        };
        assert!(h2.is_null());
        assert_eq!(status, 2);
    }

    #[test]
    fn ffi_defensive_null_and_huge_len_guards() {
        unsafe {
            let mut bom = 123usize;
            assert_eq!(t3k_rs_detect_encoding(ptr::null(), 100, &mut bom), 0);
            assert_eq!(bom, 0);
            assert_eq!(
                t3k_rs_find_ascii_pattern(ptr::null(), 10, b"a".as_ptr(), 1, 0),
                usize::MAX
            );
            assert_eq!(
                t3k_rs_find_exact_bytes(b"abc".as_ptr(), usize::MAX, b"a".as_ptr(), 1),
                usize::MAX
            );
            assert_eq!(
                t3k_rs_match_utf16_document(
                    ptr::null(),
                    10,
                    ptr::null(),
                    1,
                    0,
                    3,
                    None,
                    ptr::null(),
                    None,
                    ptr::null_mut(),
                ),
                0
            );
            assert_eq!(
                t3k_rs_scan_mapped_text(
                    ptr::null(),
                    10,
                    1,
                    ptr::null(),
                    ptr::null(),
                    1,
                    0,
                    3,
                    None,
                    ptr::null(),
                    None,
                    ptr::null(),
                    None,
                    ptr::null_mut(),
                ),
                0
            );
        }
    }

    #[test]
    fn scan_mapped_huge_line_overflow_safe() {
        let mut huge = vec![b'x'; 20000];
        huge[19990..19995].copy_from_slice(b"hello");
        let term = u16_vec("hello");
        // Deliberately pass an exaggerated u16 slice length check or normal slice on huge line
        let u16_slice = T3kUtf16Slice {
            ptr: term.as_ptr(),
            len: term.len(),
        };
        let byte_var = T3kByteTermVariants {
            is_ascii: 1,
            ascii_ptr: b"hello".as_ptr(),
            ascii_len: 5,
            variants_ptr: ptr::null(),
            variants_count: 0,
        };
        let matched = unsafe {
            t3k_rs_scan_mapped_text(
                huge.as_ptr(),
                huge.len(),
                T3kDetectedEncoding::Utf8 as u32,
                &u16_slice,
                &byte_var,
                1,
                0,
                3,
                None,
                ptr::null(),
                None,
                ptr::null(),
                None,
                ptr::null_mut(),
            )
        };
        assert_eq!(matched, 1);
    }
}
