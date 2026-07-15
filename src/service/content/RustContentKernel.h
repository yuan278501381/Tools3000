// Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors.
// All rights reserved. | MIT License

#pragma once

#include <cstddef>
#include <cstdint>

namespace tools3000::service::content {

extern "C" {

struct T3kUtf16Slice {
    const uint16_t* ptr;
    size_t len;
};

struct T3kByteSlice {
    const uint8_t* ptr;
    size_t len;
};

struct T3kByteTermVariants {
    uint8_t is_ascii;
    const uint8_t* ascii_ptr;
    size_t ascii_len;
    const T3kByteSlice* variants_ptr;
    size_t variants_count;
};

using T3kCancelCheckFn = uint8_t (*)(const void* ctx);
using T3kFallbackDecodeLineFn = size_t (*)(
    const void* ctx,
    const uint8_t* bytes_ptr,
    size_t bytes_len,
    uint32_t encoding,
    uint16_t* out_u16_ptr,
    size_t out_u16_cap
);
using T3kEmitSnippetFn = void (*)(
    void* ctx,
    uint32_t line_number,
    const uint16_t* line_ptr,
    size_t line_len,
    uint32_t match_offset,
    uint32_t match_length
);

uint32_t t3k_rs_detect_encoding(
    const uint8_t* data_ptr,
    size_t len,
    size_t* out_bom_offset
);

size_t t3k_rs_find_ascii_pattern(
    const uint8_t* data_ptr,
    size_t data_len,
    const uint8_t* pat_ptr,
    size_t pat_len,
    uint8_t case_sensitive
);

size_t t3k_rs_find_exact_bytes(
    const uint8_t* data_ptr,
    size_t data_len,
    const uint8_t* pat_ptr,
    size_t pat_len
);

uint8_t t3k_rs_match_utf16_document(
    const uint16_t* text_ptr,
    size_t text_len,
    const T3kUtf16Slice* terms_ptr,
    size_t terms_count,
    uint8_t case_sensitive,
    size_t max_snippets,
    T3kCancelCheckFn cancel_fn,
    const void* cancel_ctx,
    T3kEmitSnippetFn emit_fn,
    void* emit_ctx
);

uint8_t t3k_rs_scan_mapped_text(
    const uint8_t* body_ptr,
    size_t body_len,
    uint32_t encoding,
    const T3kUtf16Slice* u16_terms_ptr,
    const T3kByteTermVariants* byte_terms_ptr,
    size_t terms_count,
    uint8_t case_sensitive,
    size_t max_snippets,
    T3kCancelCheckFn cancel_fn,
    const void* cancel_ctx,
    T3kFallbackDecodeLineFn fallback_decode_fn,
    const void* fallback_ctx,
    T3kEmitSnippetFn emit_fn,
    void* emit_ctx
);

} // extern "C"

} // namespace tools3000::service::content
