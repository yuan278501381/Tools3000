#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ClipboardRustFfi.h — tools3000_clipboard_rs.lib 纯 Rust 持久化内核 C ABI 声明
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CLIPBOARD_CLIPBOARDRUSTFFI_H
#define TOOLS3000_CLIPBOARD_CLIPBOARDRUSTFFI_H

#include <cstdint>

extern "C" {

int32_t t3k_clip_init(const char* data_dir_utf8);
void t3k_clip_shutdown();

uint64_t t3k_clip_add_item(
    uint8_t kind,
    const char* text_utf8,
    const char* image_file_utf8,
    uint32_t image_width,
    uint32_t image_height,
    const char* content_hash_utf8,
    const char* source_app_utf8,
    uint64_t byte_size,
    uint32_t max_items,
    uint32_t max_days,
    const char* html_file_utf8,
    const char* rtf_file_utf8);

char* t3k_clip_query_json(
    const char* query_utf8,
    const char* filter_kind_utf8,
    const char* group_id_utf8,
    uint32_t limit,
    uint32_t offset);

char* t3k_clip_get_item_json(uint64_t id);
int32_t t3k_clip_toggle_pin(uint64_t id);
int32_t t3k_clip_delete_item(uint64_t id);
uint32_t t3k_clip_delete_items_batch(const uint64_t* ids, uint32_t count, int32_t skip_pinned);
int32_t t3k_clip_update_item_text(uint64_t id, const char* new_text_utf8);

char* t3k_clip_list_groups_json();
char* t3k_clip_create_group(const char* name_utf8);
int32_t t3k_clip_rename_group(const char* group_id_utf8, const char* new_name_utf8);
int32_t t3k_clip_delete_group(const char* group_id_utf8);
uint32_t t3k_clip_set_items_group(const uint64_t* ids, uint32_t count, const char* group_id_utf8);

uint32_t t3k_clip_clear_history(int32_t keep_pinned);
uint32_t t3k_clip_prune(uint32_t max_items, uint32_t max_days);
char* t3k_clip_stats_json();
void t3k_clip_free_string(char* ptr);

}  // extern "C"

#endif  // TOOLS3000_CLIPBOARD_CLIPBOARDRUSTFFI_H
