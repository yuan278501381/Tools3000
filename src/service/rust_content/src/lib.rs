// Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors.
// All rights reserved. | MIT License
//
// Modular Rust SIMD-accelerated, zero-line-allocation content search kernel for Tools3000_Service.
//
// Architecture Topology:
// - `constants`: Immutable business thresholds and scan interval constants (SSOT).
// - `encoding`:  BOM & statistical encoding detection (UTF-8 / GBK / UTF-16LE / UTF-16BE / MixedText).
// - `simd`:      Cross-architecture SIMD byte/substring search primitives (SSE2 on x86_64, scalar/NEON on ARM64).
// - `snippet`:   UTF-16 control-char sanitization, surrogate-safe windowing, and highlight offset alignment.
// - `matcher`:   Zero-line-allocation multi-term AND matcher with single reusable case-fold buffer.
// - `ffi`:       C ABI contracts and `extern "C"` entrypoints linked into Tools3000_Service & Tools3000Tests.

pub mod constants;
pub mod encoding;
pub mod ffi;
pub mod matcher;
pub mod simd;
pub mod snippet;

pub use encoding::{detect_encoding_slice, T3kDetectedEncoding};
pub use ffi::{
    t3k_rs_detect_encoding, t3k_rs_find_ascii_pattern, t3k_rs_find_exact_bytes,
    t3k_rs_match_utf16_document, t3k_rs_scan_mapped_text, T3kByteSlice, T3kByteTermVariants,
    T3kCancelCheckFn, T3kEmitSnippetFn, T3kFallbackDecodeLineFn, T3kUtf16Slice,
};
pub use matcher::RustMultiTermMatcher;
pub use simd::{find_ascii_bytes_simd, find_byte_simd, find_exact_bytes_simd};
pub use snippet::{build_aligned_snippet, BuiltSnippet};
