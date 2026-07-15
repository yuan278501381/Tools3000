// Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors.
// All rights reserved. | MIT License
//
// Character encoding detection (BOM, strict UTF-8, strict GBK, UTF-16LE/BE) and UTF-16 utilities.

use crate::constants::{
    MAX_RELAXED_CONTROL_BYTE_RATIO, MAX_UTF16_CONTROL_UNIT_RATIO, SAMPLE_INSPECTION_BYTES,
};

#[repr(u32)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum T3kDetectedEncoding {
    UnknownBinary = 0,
    Utf8 = 1,
    Utf16Le = 2,
    Utf16Be = 3,
    Gbk = 4,
    MixedText = 5,
}

#[inline(always)]
pub fn is_illegal_control_byte(b: u8) -> bool {
    if b == 0x00 {
        return true;
    }
    if b < 0x20 {
        return b != 0x09 && b != 0x0A && b != 0x0D && b != 0x0C && b != 0x08 && b != 0x1B;
    }
    b == 0x7F
}

#[inline(always)]
pub fn is_high_surrogate(ch: u16) -> bool {
    (0xD800..=0xDBFF).contains(&ch)
}

#[inline(always)]
pub fn is_low_surrogate(ch: u16) -> bool {
    (0xDC00..=0xDFFF).contains(&ch)
}

#[inline(always)]
pub fn read_utf16_unit(data: &[u8], unit_idx: usize, is_be: bool) -> u16 {
    let byte_pos = unit_idx * 2;
    if is_be {
        ((data[byte_pos] as u16) << 8) | (data[byte_pos + 1] as u16)
    } else {
        (data[byte_pos] as u16) | ((data[byte_pos + 1] as u16) << 8)
    }
}

fn check_strict_utf8(data: &[u8]) -> Option<(usize, usize)> {
    let mut i = 0;
    let len = data.len();
    let mut valid_multibyte = 0usize;
    let mut illegal_control = 0usize;

    while i < len {
        let c = data[i];
        if c <= 0x7F {
            if is_illegal_control_byte(c) {
                illegal_control += 1;
            }
            i += 1;
        } else if (0xC2..=0xDF).contains(&c) {
            if i + 1 >= len {
                break;
            }
            if (data[i + 1] & 0xC0) != 0x80 {
                return None;
            }
            valid_multibyte += 1;
            i += 2;
        } else if (0xE0..=0xEF).contains(&c) {
            if i + 2 >= len {
                break;
            }
            let c1 = data[i + 1];
            let c2 = data[i + 2];
            if (c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 {
                return None;
            }
            if c == 0xE0 && c1 < 0xA0 {
                return None;
            }
            if c == 0xED && c1 >= 0xA0 {
                return None;
            }
            valid_multibyte += 1;
            i += 3;
        } else if (0xF0..=0xF4).contains(&c) {
            if i + 3 >= len {
                break;
            }
            let c1 = data[i + 1];
            let c2 = data[i + 2];
            let c3 = data[i + 3];
            if (c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80 {
                return None;
            }
            if c == 0xF0 && c1 < 0x90 {
                return None;
            }
            if c == 0xF4 && c1 > 0x8F {
                return None;
            }
            valid_multibyte += 1;
            i += 4;
        } else {
            return None;
        }
    }
    Some((valid_multibyte, illegal_control))
}

fn check_strict_gbk(data: &[u8]) -> Option<(usize, usize)> {
    let mut i = 0;
    let len = data.len();
    let mut valid_gbk = 0usize;
    let mut illegal_control = 0usize;

    while i < len {
        let c = data[i];
        if c <= 0x7F {
            if is_illegal_control_byte(c) {
                illegal_control += 1;
            }
            i += 1;
        } else if (0x81..=0xFE).contains(&c) {
            if i + 1 >= len {
                break;
            }
            let c2 = data[i + 1];
            if (0x40..=0x7E).contains(&c2) || (0x80..=0xFE).contains(&c2) {
                valid_gbk += 1;
                i += 2;
            } else {
                return None;
            }
        } else {
            return None;
        }
    }
    Some((valid_gbk, illegal_control))
}

fn is_utf16_buffer(data: &[u8], is_be: bool) -> bool {
    let len = data.len();
    if len < 2 || (len % 2 != 0) {
        return false;
    }
    let count = len / 2;
    let mut control_count = 0usize;
    let mut valid_char_count = 0usize;
    let mut i = 0usize;

    while i < count {
        let code = read_utf16_unit(data, i, is_be);
        if code == 0x0000 {
            return false;
        }
        if is_high_surrogate(code) {
            if i + 1 == count {
                break;
            }
            let low = read_utf16_unit(data, i + 1, is_be);
            if !is_low_surrogate(low) {
                return false;
            }
            valid_char_count += 2;
            i += 2;
            continue;
        }
        if is_low_surrogate(code) {
            return false;
        }
        if code < 0x20 {
            if code != 0x09 && code != 0x0A && code != 0x0D && code != 0x0C {
                control_count += 1;
            }
        } else {
            valid_char_count += 1;
        }
        i += 1;
    }

    if count > 0 && ((control_count as f64) / (count as f64) > MAX_UTF16_CONTROL_UNIT_RATIO) {
        return false;
    }
    valid_char_count > 0
}

pub fn detect_encoding_slice(data: &[u8]) -> (T3kDetectedEncoding, usize) {
    let len = data.len();
    if len == 0 {
        return (T3kDetectedEncoding::UnknownBinary, 0);
    }

    if len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF {
        return (T3kDetectedEncoding::Utf8, 3);
    }
    if len >= 2 && data[0] == 0xFF && data[1] == 0xFE {
        return (T3kDetectedEncoding::Utf16Le, 2);
    }
    if len >= 2 && data[0] == 0xFE && data[1] == 0xFF {
        return (T3kDetectedEncoding::Utf16Be, 2);
    }

    let sample_len = len.min(SAMPLE_INSPECTION_BYTES);
    let sample = &data[..sample_len];

    let mut null_count = 0usize;
    let mut illegal_control_count = 0usize;
    for &b in sample {
        if b == 0x00 {
            null_count += 1;
        } else if is_illegal_control_byte(b) {
            illegal_control_count += 1;
        }
    }

    if null_count > 0 {
        let utf16_sample_len = if len % 2 == 0 {
            sample_len & !1usize
        } else {
            sample_len
        };
        if utf16_sample_len >= 2 {
            let u16_sample = &data[..utf16_sample_len];
            if is_utf16_buffer(u16_sample, false) {
                return (T3kDetectedEncoding::Utf16Le, 0);
            }
            if is_utf16_buffer(u16_sample, true) {
                return (T3kDetectedEncoding::Utf16Be, 0);
            }
        }
        return (T3kDetectedEncoding::UnknownBinary, 0);
    }

    let control_ratio = (illegal_control_count as f64) / (sample_len as f64);
    if control_ratio >= MAX_RELAXED_CONTROL_BYTE_RATIO {
        return (T3kDetectedEncoding::UnknownBinary, 0);
    }

    if check_strict_utf8(sample).is_some() {
        return (T3kDetectedEncoding::Utf8, 0);
    }

    if let Some((gbk_multi, _)) = check_strict_gbk(sample) {
        return if gbk_multi == 0 {
            (T3kDetectedEncoding::Utf8, 0)
        } else {
            (T3kDetectedEncoding::Gbk, 0)
        };
    }

    (T3kDetectedEncoding::MixedText, 0)
}
