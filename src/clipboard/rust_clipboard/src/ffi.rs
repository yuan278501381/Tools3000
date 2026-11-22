// ─────────────────────────────────────────────────────────────────────────────
// ffi.rs — C ABI 零开销导出层（供 C++ ClipboardManager 线程安全调用）
// ─────────────────────────────────────────────────────────────────────────────

use crate::store::ClipboardStore;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::path::PathBuf;
use std::ptr;
use std::sync::{Mutex, OnceLock};

static GLOBAL_STORE: OnceLock<Mutex<Option<ClipboardStore>>> = OnceLock::new();

fn store_slot() -> &'static Mutex<Option<ClipboardStore>> {
    GLOBAL_STORE.get_or_init(|| Mutex::new(None))
}

unsafe fn cstr_to_str<'a>(ptr: *const c_char) -> &'a str {
    if ptr.is_null() {
        return "";
    }
    CStr::from_ptr(ptr).to_str().unwrap_or("")
}

fn string_to_c_ptr(s: String) -> *mut c_char {
    match CString::new(s) {
        Ok(cs) => cs.into_raw(),
        Err(_) => CString::new("{}").map(|c| c.into_raw()).unwrap_or(ptr::null_mut()),
    }
}

#[no_mangle]
pub unsafe extern "C" fn t3k_clip_init(data_dir_utf8: *const c_char) -> i32 {
    let dir_str = cstr_to_str(data_dir_utf8);
    if dir_str.is_empty() {
        return 0;
    }
    if let Ok(mut guard) = store_slot().lock() {
        *guard = Some(ClipboardStore::open(PathBuf::from(dir_str)));
        return 1;
    }
    0
}

#[no_mangle]
pub extern "C" fn t3k_clip_shutdown() {
    if let Ok(mut guard) = store_slot().lock() {
        *guard = None;
    }
}

#[no_mangle]
pub unsafe extern "C" fn t3k_clip_add_item(
    kind: u8,
    text_utf8: *const c_char,
    image_file_utf8: *const c_char,
    image_width: u32,
    image_height: u32,
    content_hash_utf8: *const c_char,
    source_app_utf8: *const c_char,
    byte_size: u64,
    max_items: u32,
    max_days: u32,
) -> u64 {
    let text = cstr_to_str(text_utf8);
    let image_file = cstr_to_str(image_file_utf8);
    let hash = cstr_to_str(content_hash_utf8);
    let app = cstr_to_str(source_app_utf8);

    if let Ok(mut guard) = store_slot().lock() {
        if let Some(store) = guard.as_mut() {
            return store.add_item(
                kind,
                text,
                image_file,
                image_width,
                image_height,
                hash,
                app,
                byte_size,
                max_items,
                max_days,
            );
        }
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn t3k_clip_query_json(
    query_utf8: *const c_char,
    filter_kind_utf8: *const c_char,
    limit: u32,
    offset: u32,
) -> *mut c_char {
    let query = cstr_to_str(query_utf8);
    let filter_kind = cstr_to_str(filter_kind_utf8);
    if let Ok(guard) = store_slot().lock() {
        if let Some(store) = guard.as_ref() {
            return string_to_c_ptr(store.query_json(query, filter_kind, limit as usize, offset as usize));
        }
    }
    string_to_c_ptr("{\"total\":0,\"matched\":0,\"items\":[]}".to_string())
}

#[no_mangle]
pub extern "C" fn t3k_clip_get_item_json(id: u64) -> *mut c_char {
    if let Ok(guard) = store_slot().lock() {
        if let Some(store) = guard.as_ref() {
            if let Some(json) = store.get_item_json(id) {
                return string_to_c_ptr(json);
            }
        }
    }
    ptr::null_mut()
}

#[no_mangle]
pub extern "C" fn t3k_clip_toggle_pin(id: u64) -> i32 {
    if let Ok(mut guard) = store_slot().lock() {
        if let Some(store) = guard.as_mut() {
            return match store.toggle_pin(id) {
                Some(true) => 1,
                Some(false) => 0,
                None => -1,
            };
        }
    }
    -1
}

#[no_mangle]
pub extern "C" fn t3k_clip_delete_item(id: u64) -> i32 {
    if let Ok(mut guard) = store_slot().lock() {
        if let Some(store) = guard.as_mut() {
            return if store.delete_item(id) { 1 } else { 0 };
        }
    }
    0
}

#[no_mangle]
pub extern "C" fn t3k_clip_clear_history(keep_pinned: i32) -> u32 {
    if let Ok(mut guard) = store_slot().lock() {
        if let Some(store) = guard.as_mut() {
            return store.clear_history(keep_pinned != 0);
        }
    }
    0
}

#[no_mangle]
pub extern "C" fn t3k_clip_prune(max_items: u32, max_days: u32) -> u32 {
    if let Ok(mut guard) = store_slot().lock() {
        if let Some(store) = guard.as_mut() {
            return store.prune(max_items, max_days);
        }
    }
    0
}

#[no_mangle]
pub extern "C" fn t3k_clip_stats_json() -> *mut c_char {
    if let Ok(guard) = store_slot().lock() {
        if let Some(store) = guard.as_ref() {
            return string_to_c_ptr(store.stats_json());
        }
    }
    string_to_c_ptr("{\"totalCount\":0,\"pinnedCount\":0,\"textCount\":0,\"imageCount\":0,\"filesCount\":0,\"colorCount\":0,\"contentBytes\":0,\"diskBytes\":0,\"dataDir\":\"\"}".to_string())
}

#[no_mangle]
pub unsafe extern "C" fn t3k_clip_free_string(ptr: *mut c_char) {
    if !ptr.is_null() {
        drop(CString::from_raw(ptr));
    }
}
