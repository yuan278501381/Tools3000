// Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors.
// All rights reserved. | MIT License
//
// Cross-architecture SIMD byte and substring search primitives (SSE2 on x86_64, scalar/NEON fallback).

#[inline(always)]
pub fn ascii_lower_byte(b: u8) -> u8 {
    if b.is_ascii_uppercase() {
        b + 32
    } else {
        b
    }
}

#[inline]
pub fn find_byte_simd(haystack: &[u8], needle: u8) -> Option<usize> {
    #[cfg(target_arch = "x86_64")]
    unsafe {
        use core::arch::x86_64::*;
        let len = haystack.len();
        let ptr = haystack.as_ptr();
        let mut i = 0usize;
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
    #[cfg(not(target_arch = "x86_64"))]
    {
        haystack.iter().position(|&b| b == needle)
    }
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

    let first = needle[0];
    let last = needle[nlen - 1];
    let end = hlen - nlen + 1;

    #[cfg(target_arch = "x86_64")]
    unsafe {
        use core::arch::x86_64::*;
        let ptr = haystack.as_ptr();
        let v_first = _mm_set1_epi8(first as i8);
        let v_last = _mm_set1_epi8(last as i8);
        let mut i = 0usize;

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

    #[cfg(not(target_arch = "x86_64"))]
    {
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

    let verify_tail = |pos: usize| -> bool {
        for j in 1..nlen {
            let bj = haystack[pos + j];
            let pj = needle[j];
            if bj != pj && ascii_lower_byte(bj) != ascii_lower_byte(pj) {
                return false;
            }
        }
        true
    };

    #[cfg(target_arch = "x86_64")]
    unsafe {
        use core::arch::x86_64::*;
        let ptr = haystack.as_ptr();
        let v_first_lo = _mm_set1_epi8(first_lo as i8);
        let v_first_hi = _mm_set1_epi8(first_hi as i8);
        let v_last_lo = _mm_set1_epi8(last_lo as i8);
        let v_last_hi = _mm_set1_epi8(last_hi as i8);
        let mut i = 0usize;

        while i + 16 <= end {
            let c_first = _mm_loadu_si128(ptr.add(i) as *const __m128i);
            let m_first = _mm_or_si128(
                _mm_cmpeq_epi8(c_first, v_first_lo),
                _mm_cmpeq_epi8(c_first, v_first_hi),
            );
            let mask_combined = if nlen == 1 {
                _mm_movemask_epi8(m_first) as u32
            } else {
                let c_last = _mm_loadu_si128(ptr.add(i + nlen - 1) as *const __m128i);
                let m_last = _mm_or_si128(
                    _mm_cmpeq_epi8(c_last, v_last_lo),
                    _mm_cmpeq_epi8(c_last, v_last_hi),
                );
                _mm_movemask_epi8(_mm_and_si128(m_first, m_last)) as u32
            };
            let mut mask = mask_combined;
            while mask != 0 {
                let bit = mask.trailing_zeros() as usize;
                let pos = i + bit;
                if verify_tail(pos) {
                    return Some(pos);
                }
                mask &= mask - 1;
            }
            i += 16;
        }

        while i < end {
            let b0 = haystack[i];
            if (b0 == first_lo || b0 == first_hi) && verify_tail(i) {
                return Some(i);
            }
            i += 1;
        }
        None
    }

    #[cfg(not(target_arch = "x86_64"))]
    {
        for i in 0..end {
            let b0 = haystack[i];
            if (b0 == first_lo || b0 == first_hi) && verify_tail(i) {
                return Some(i);
            }
        }
        None
    }
}
