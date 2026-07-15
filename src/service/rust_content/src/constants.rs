// Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors.
// All rights reserved. | MIT License
//
// Immutable business rules and performance thresholds for the Rust content search kernel.

/// Maximum UTF-16 code units retained per snippet line before context windowing.
pub const MAX_SNIPPET_LINE_CHARS: usize = 220;

/// UTF-16 code units of context preserved around a match when truncating long lines.
pub const SNIPPET_CONTEXT_PADDING_CHARS: usize = 48;

/// Maximum ratio of replacement/invalid characters allowed in a snippet line.
pub const MAX_GARBLED_CHAR_RATIO: f64 = 0.20;

/// Maximum prefix bytes inspected during automatic character encoding detection.
pub const SAMPLE_INSPECTION_BYTES: usize = 8192;

/// Maximum ratio of illegal control bytes tolerated in 8-bit text encodings.
pub const MAX_RELAXED_CONTROL_BYTE_RATIO: f64 = 0.02;

/// Maximum ratio of illegal control units tolerated in UTF-16 text encodings.
pub const MAX_UTF16_CONTROL_UNIT_RATIO: f64 = 0.03;

/// Threshold above which a single line (e.g., minified JS/JSON) uses windowed decoding.
pub const HUGE_LINE_BYTE_THRESHOLD: usize = 16384;

/// Byte radius decoded around a byte-level hit inside a huge single line.
pub const HUGE_LINE_WINDOW_RADIUS: usize = 384;

/// Reusable fold buffer initial capacity in UTF-16 units.
pub const INITIAL_FOLD_BUFFER_CAPACITY: usize = 512;

/// Byte interval between cancellation checks during memory-mapped byte scanning.
pub const CANCEL_CHECK_BYTE_INTERVAL: usize = 0x40000;

/// Line interval mask for periodic cancellation checks in UTF-8/GBK/UTF-16 loops.
pub const CANCEL_CHECK_LINE_MASK: u32 = 0x3FF;

/// Line interval mask for periodic cancellation checks in in-memory UTF-16 document matching.
pub const CANCEL_CHECK_DOC_LINE_MASK: u32 = 0xFF;

/// UTF-16 unit interval mask for cancellation checks inside giant newline-less UTF-16 files.
pub const CANCEL_CHECK_UTF16_UNIT_MASK: usize = 0x3FFFF;
