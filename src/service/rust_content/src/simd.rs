// Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors.
// All rights reserved. | MIT License
//
// Cross-architecture SIMD byte and substring search primitives:
// - x86_64: Runtime AVX2 dynamic dispatch (32-byte vectors) with SSE2 baseline (16-byte vectors)
// - aarch64: Native ARM64 NEON (16-byte vectors with nibble bitmask extraction)
// - Other targets: Scalar fallback

#[cfg(target_arch = "x86_64")]
use std::sync::atomic::{AtomicU8, Ordering};

#[cfg(target_arch = "x86_64")]
static X86_SIMD_LEVEL: AtomicU8 = AtomicU8::new(0); // 0 = unknown, 1 = SSE2, 2 = AVX2

#[cfg(target_arch = "x86_64")]
#[inline]
fn has_avx2() -> bool {
    let cached = X86_SIMD_LEVEL.load(Ordering::Relaxed);
    if cached != 0 {
        return cached == 2;
    }
    let detected = if std::arch::is_x86_feature_detected!("avx2") {
        2u8
    } else {
        1u8
    };
    X86_SIMD_LEVEL.store(detected, Ordering::Relaxed);
    detected == 2
}

#[inline(always)]
pub fn ascii_lower_byte(b: u8) -> u8 {
    if b.is_ascii_uppercase() {
        b + 32
    } else {
        b
    }
}

#[cfg(target_arch = "x86_64")]
#[inline]
unsafe fn find_byte_sse2(haystack: &[u8], start: usize, needle: u8) -> Option<usize> {
    use core::arch::x86_64::*;
    let len = haystack.len();
    let ptr = haystack.as_ptr();
    let mut i = start;
    let v_needle = _mm_set1_epi8(needle as i8);
    while i + 16 <= len {
        let chunk = _mm_loadu_si128(ptr.add(i) as *const __m128i);
        let eq = _mm_cmpeq_epi8(chunk, v_needle);
        let mask = _mm_movemask_epi8(eq) as u32;
        if mask != 0 {
            return Some(i + (mask.trailing_zeros() as usize));
        }
        i += 16;
    }
    while i < len {
        if *ptr.add(i) == needle {
            return Some(i);
        }
        i += 1;
    }
    None
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn find_byte_avx2(haystack: &[u8], needle: u8) -> Option<usize> {
    use core::arch::x86_64::*;
    let len = haystack.len();
    let ptr = haystack.as_ptr();
    let mut i = 0usize;
    let v_needle = _mm256_set1_epi8(needle as i8);
    while i + 32 <= len {
        let chunk = _mm256_loadu_si256(ptr.add(i) as *const __m256i);
        let eq = _mm256_cmpeq_epi8(chunk, v_needle);
        let mask = _mm256_movemask_epi8(eq) as u32;
        if mask != 0 {
            return Some(i + (mask.trailing_zeros() as usize));
        }
        i += 32;
    }
    find_byte_sse2(haystack, i, needle)
}

#[cfg(target_arch = "aarch64")]
#[inline(always)]
unsafe fn neon_movemask_nibble(cmp_mask: core::arch::aarch64::uint8x16_t) -> u64 {
    use core::arch::aarch64::*;
    let narrowed = vshrn_n_u16(vreinterpretq_u16_u8(cmp_mask), 4);
    vget_lane_u64(vreinterpret_u64_u8(narrowed), 0)
}

#[inline]
pub fn find_byte_simd(haystack: &[u8], needle: u8) -> Option<usize> {
    #[cfg(target_arch = "x86_64")]
    unsafe {
        if haystack.len() >= 32 && has_avx2() {
            return find_byte_avx2(haystack, needle);
        }
        return find_byte_sse2(haystack, 0, needle);
    }

    #[cfg(target_arch = "aarch64")]
    unsafe {
        use core::arch::aarch64::*;
        let len = haystack.len();
        let ptr = haystack.as_ptr();
        let mut i = 0usize;
        let v_needle = vdupq_n_u8(needle);
        while i + 16 <= len {
            let chunk = vld1q_u8(ptr.add(i));
            let eq = vceqq_u8(chunk, v_needle);
            if vmaxvq_u8(eq) != 0 {
                let nibble_mask = neon_movemask_nibble(eq);
                if nibble_mask != 0 {
                    return Some(i + ((nibble_mask.trailing_zeros() >> 2) as usize));
                }
            }
            i += 16;
        }
        while i < len {
            if *ptr.add(i) == needle {
                return Some(i);
            }
            i += 1;
        }
        None
    }

    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
    {
        haystack.iter().position(|&b| b == needle)
    }
}

#[cfg(target_arch = "x86_64")]
#[inline]
unsafe fn find_exact_bytes_sse2(
    haystack: &[u8],
    needle: &[u8],
    start: usize,
    end: usize,
) -> Option<usize> {
    use core::arch::x86_64::*;
    let nlen = needle.len();
    let first = needle[0];
    let last = needle[nlen - 1];
    let ptr = haystack.as_ptr();
    let v_first = _mm_set1_epi8(first as i8);
    let v_last = _mm_set1_epi8(last as i8);
    let mut i = start;

    while i + 16 <= end {
        let c_first = _mm_loadu_si128(ptr.add(i) as *const __m128i);
        let c_last = _mm_loadu_si128(ptr.add(i + nlen - 1) as *const __m128i);
        let eq = _mm_and_si128(
            _mm_cmpeq_epi8(c_first, v_first),
            _mm_cmpeq_epi8(c_last, v_last),
        );
        let mut mask = _mm_movemask_epi8(eq) as u32;
        while mask != 0 {
            let bit = mask.trailing_zeros() as usize;
            let pos = i + bit;
            if nlen <= 2 || &haystack[pos + 1..pos + nlen - 1] == &needle[1..nlen - 1] {
                return Some(pos);
            }
            mask &= mask - 1;
        }
        i += 16;
    }

    while i < end {
        if haystack[i] == first
            && haystack[i + nlen - 1] == last
            && (nlen <= 2 || &haystack[i + 1..i + nlen - 1] == &needle[1..nlen - 1])
        {
            return Some(i);
        }
        i += 1;
    }
    None
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn find_exact_bytes_avx2(haystack: &[u8], needle: &[u8], end: usize) -> Option<usize> {
    use core::arch::x86_64::*;
    let nlen = needle.len();
    let first = needle[0];
    let last = needle[nlen - 1];
    let ptr = haystack.as_ptr();
    let v_first = _mm256_set1_epi8(first as i8);
    let v_last = _mm256_set1_epi8(last as i8);
    let mut i = 0usize;

    while i + 32 <= end {
        let c_first = _mm256_loadu_si256(ptr.add(i) as *const __m256i);
        let c_last = _mm256_loadu_si256(ptr.add(i + nlen - 1) as *const __m256i);
        let eq = _mm256_and_si256(
            _mm256_cmpeq_epi8(c_first, v_first),
            _mm256_cmpeq_epi8(c_last, v_last),
        );
        let mut mask = _mm256_movemask_epi8(eq) as u32;
        while mask != 0 {
            let bit = mask.trailing_zeros() as usize;
            let pos = i + bit;
            if nlen <= 2 || &haystack[pos + 1..pos + nlen - 1] == &needle[1..nlen - 1] {
                return Some(pos);
            }
            mask &= mask - 1;
        }
        i += 32;
    }

    find_exact_bytes_sse2(haystack, needle, i, end)
}

#[inline]
pub fn find_exact_bytes_simd(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    let nlen = needle.len();
    let hlen = haystack.len();
    if nlen == 0 {
        return Some(0);
    }
    if hlen < nlen {
        return None;
    }
    if nlen == 1 {
        return find_byte_simd(haystack, needle[0]);
    }

    let end = hlen - nlen + 1;

    #[cfg(target_arch = "x86_64")]
    unsafe {
        if end >= 32 && has_avx2() {
            return find_exact_bytes_avx2(haystack, needle, end);
        }
        return find_exact_bytes_sse2(haystack, needle, 0, end);
    }

    #[cfg(target_arch = "aarch64")]
    unsafe {
        use core::arch::aarch64::*;
        let first = needle[0];
        let last = needle[nlen - 1];
        let ptr = haystack.as_ptr();
        let v_first = vdupq_n_u8(first);
        let v_last = vdupq_n_u8(last);
        let mut i = 0usize;

        while i + 16 <= end {
            let c_first = vld1q_u8(ptr.add(i));
            let c_last = vld1q_u8(ptr.add(i + nlen - 1));
            let eq = vandq_u8(vceqq_u8(c_first, v_first), vceqq_u8(c_last, v_last));
            if vmaxvq_u8(eq) != 0 {
                let mut nibble_mask = neon_movemask_nibble(eq);
                while nibble_mask != 0 {
                    let byte_idx = (nibble_mask.trailing_zeros() >> 2) as usize;
                    let pos = i + byte_idx;
                    if nlen <= 2 || &haystack[pos + 1..pos + nlen - 1] == &needle[1..nlen - 1] {
                        return Some(pos);
                    }
                    nibble_mask &= !(0xFu64 << (byte_idx * 4));
                }
            }
            i += 16;
        }

        while i < end {
            if haystack[i] == first
                && haystack[i + nlen - 1] == last
                && (nlen <= 2 || &haystack[i + 1..i + nlen - 1] == &needle[1..nlen - 1])
            {
                return Some(i);
            }
            i += 1;
        }
        None
    }

    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
    {
        let first = needle[0];
        let last = needle[nlen - 1];
        let mut pos = 0usize;
        while pos < end {
            if let Some(rel) = find_byte_simd(&haystack[pos..end], first) {
                let idx = pos + rel;
                if haystack[idx + nlen - 1] == last
                    && (nlen <= 2 || &haystack[idx + 1..idx + nlen - 1] == &needle[1..nlen - 1])
                {
                    return Some(idx);
                }
                pos = idx + 1;
            } else {
                break;
            }
        }
        None
    }
}

#[inline(always)]
fn verify_ascii_caseless_tail(haystack: &[u8], needle: &[u8], pos: usize) -> bool {
    let nlen = needle.len();
    for j in 1..nlen {
        let bj = haystack[pos + j];
        let pj = needle[j];
        if bj != pj && ascii_lower_byte(bj) != ascii_lower_byte(pj) {
            return false;
        }
    }
    true
}

#[cfg(target_arch = "x86_64")]
#[inline]
unsafe fn find_ascii_caseless_sse2(
    haystack: &[u8],
    needle: &[u8],
    start: usize,
    end: usize,
    first_lo: u8,
    first_hi: u8,
    last_lo: u8,
    last_hi: u8,
) -> Option<usize> {
    use core::arch::x86_64::*;
    let nlen = needle.len();
    let ptr = haystack.as_ptr();
    let v_first_lo = _mm_set1_epi8(first_lo as i8);
    let v_first_hi = _mm_set1_epi8(first_hi as i8);
    let v_last_lo = _mm_set1_epi8(last_lo as i8);
    let v_last_hi = _mm_set1_epi8(last_hi as i8);
    let mut i = start;

    while i + 16 <= end {
        let c_first = _mm_loadu_si128(ptr.add(i) as *const __m128i);
        let m_first = _mm_or_si128(
            _mm_cmpeq_epi8(c_first, v_first_lo),
            _mm_cmpeq_epi8(c_first, v_first_hi),
        );
        let mut mask = if nlen == 1 {
            _mm_movemask_epi8(m_first) as u32
        } else {
            let c_last = _mm_loadu_si128(ptr.add(i + nlen - 1) as *const __m128i);
            let m_last = _mm_or_si128(
                _mm_cmpeq_epi8(c_last, v_last_lo),
                _mm_cmpeq_epi8(c_last, v_last_hi),
            );
            _mm_movemask_epi8(_mm_and_si128(m_first, m_last)) as u32
        };
        while mask != 0 {
            let bit = mask.trailing_zeros() as usize;
            let pos = i + bit;
            if verify_ascii_caseless_tail(haystack, needle, pos) {
                return Some(pos);
            }
            mask &= mask - 1;
        }
        i += 16;
    }

    while i < end {
        let b0 = haystack[i];
        if (b0 == first_lo || b0 == first_hi) && verify_ascii_caseless_tail(haystack, needle, i) {
            return Some(i);
        }
        i += 1;
    }
    None
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn find_ascii_caseless_avx2(
    haystack: &[u8],
    needle: &[u8],
    end: usize,
    first_lo: u8,
    first_hi: u8,
    last_lo: u8,
    last_hi: u8,
) -> Option<usize> {
    use core::arch::x86_64::*;
    let nlen = needle.len();
    let ptr = haystack.as_ptr();
    let v_first_lo = _mm256_set1_epi8(first_lo as i8);
    let v_first_hi = _mm256_set1_epi8(first_hi as i8);
    let v_last_lo = _mm256_set1_epi8(last_lo as i8);
    let v_last_hi = _mm256_set1_epi8(last_hi as i8);
    let mut i = 0usize;

    while i + 32 <= end {
        let c_first = _mm256_loadu_si256(ptr.add(i) as *const __m256i);
        let m_first = _mm256_or_si256(
            _mm256_cmpeq_epi8(c_first, v_first_lo),
            _mm256_cmpeq_epi8(c_first, v_first_hi),
        );
        let mut mask = if nlen == 1 {
            _mm256_movemask_epi8(m_first) as u32
        } else {
            let c_last = _mm256_loadu_si256(ptr.add(i + nlen - 1) as *const __m256i);
            let m_last = _mm256_or_si256(
                _mm256_cmpeq_epi8(c_last, v_last_lo),
                _mm256_cmpeq_epi8(c_last, v_last_hi),
            );
            _mm256_movemask_epi8(_mm256_and_si256(m_first, m_last)) as u32
        };
        while mask != 0 {
            let bit = mask.trailing_zeros() as usize;
            let pos = i + bit;
            if verify_ascii_caseless_tail(haystack, needle, pos) {
                return Some(pos);
            }
            mask &= mask - 1;
        }
        i += 32;
    }

    find_ascii_caseless_sse2(
        haystack, needle, i, end, first_lo, first_hi, last_lo, last_hi,
    )
}

#[inline]
pub fn find_ascii_bytes_simd(
    haystack: &[u8],
    needle: &[u8],
    case_sensitive: bool,
) -> Option<usize> {
    if case_sensitive {
        return find_exact_bytes_simd(haystack, needle);
    }
    let nlen = needle.len();
    let hlen = haystack.len();
    if nlen == 0 {
        return Some(0);
    }
    if hlen < nlen {
        return None;
    }

    let first_lo = ascii_lower_byte(needle[0]);
    let first_hi = first_lo.to_ascii_uppercase();
    let last_lo = ascii_lower_byte(needle[nlen - 1]);
    let last_hi = last_lo.to_ascii_uppercase();
    let end = hlen - nlen + 1;

    #[cfg(target_arch = "x86_64")]
    unsafe {
        if end >= 32 && has_avx2() {
            return find_ascii_caseless_avx2(
                haystack, needle, end, first_lo, first_hi, last_lo, last_hi,
            );
        }
        return find_ascii_caseless_sse2(
            haystack, needle, 0, end, first_lo, first_hi, last_lo, last_hi,
        );
    }

    #[cfg(target_arch = "aarch64")]
    unsafe {
        use core::arch::aarch64::*;
        let ptr = haystack.as_ptr();
        let v_first_lo = vdupq_n_u8(first_lo);
        let v_first_hi = vdupq_n_u8(first_hi);
        let v_last_lo = vdupq_n_u8(last_lo);
        let v_last_hi = vdupq_n_u8(last_hi);
        let mut i = 0usize;

        while i + 16 <= end {
            let c_first = vld1q_u8(ptr.add(i));
            let m_first = vorrq_u8(vceqq_u8(c_first, v_first_lo), vceqq_u8(c_first, v_first_hi));
            let m_combined = if nlen == 1 {
                m_first
            } else {
                let c_last = vld1q_u8(ptr.add(i + nlen - 1));
                let m_last = vorrq_u8(vceqq_u8(c_last, v_last_lo), vceqq_u8(c_last, v_last_hi));
                vandq_u8(m_first, m_last)
            };
            if vmaxvq_u8(m_combined) != 0 {
                let mut nibble_mask = neon_movemask_nibble(m_combined);
                while nibble_mask != 0 {
                    let byte_idx = (nibble_mask.trailing_zeros() >> 2) as usize;
                    let pos = i + byte_idx;
                    if verify_ascii_caseless_tail(haystack, needle, pos) {
                        return Some(pos);
                    }
                    nibble_mask &= !(0xFu64 << (byte_idx * 4));
                }
            }
            i += 16;
        }

        while i < end {
            let b0 = haystack[i];
            if (b0 == first_lo || b0 == first_hi) && verify_ascii_caseless_tail(haystack, needle, i)
            {
                return Some(i);
            }
            i += 1;
        }
        None
    }

    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
    {
        for i in 0..end {
            let b0 = haystack[i];
            if (b0 == first_lo || b0 == first_hi) && verify_ascii_caseless_tail(haystack, needle, i)
            {
                return Some(i);
            }
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn simd_byte_and_subslice_across_vector_boundaries() {
        let mut buf = vec![b'.'; 197];
        buf[0] = b'X';
        buf[31] = b'Y';
        buf[32] = b'Z';
        buf[129..142].copy_from_slice(b"Tools3000Core");

        assert_eq!(find_byte_simd(&buf, b'X'), Some(0));
        assert_eq!(find_byte_simd(&buf, b'Y'), Some(31));
        assert_eq!(find_byte_simd(&buf, b'Z'), Some(32));
        assert_eq!(find_byte_simd(&buf, b'!'), None);

        assert_eq!(find_exact_bytes_simd(&buf, b"Tools3000Core"), Some(129));
        assert_eq!(find_exact_bytes_simd(&buf, b"tools3000core"), None);
        assert_eq!(
            find_ascii_bytes_simd(&buf, b"tOoLs3000cOrE", false),
            Some(129)
        );
        assert_eq!(find_ascii_bytes_simd(&buf, b"tOoLs3000cOrE", true), None);
    }
}
