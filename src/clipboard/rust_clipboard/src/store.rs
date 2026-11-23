// ─────────────────────────────────────────────────────────────────────────────
// store.rs — 剪贴板内存索引、去重、收藏分组、批量操作、内联编辑与富文本 GC 引擎
// ─────────────────────────────────────────────────────────────────────────────

use crate::model::{
    detect_color_hex, fnv1a_128_hex, now_epoch_ms, push_json_str, sanitize_blob_filename,
    sanitize_group_name, ClipKind, ClipboardEntry, ClipboardGroup, MAX_GROUPS, MAX_TEXT_BYTES,
};
use crate::storage::{blobs_dir, gc_unreferenced_blobs, load_snapshot, save_snapshot};
use std::collections::HashSet;
use std::fs;
use std::path::PathBuf;

pub struct ClipboardStore {
    data_dir: PathBuf,
    next_id: u64,
    next_group_id: u64,
    groups: Vec<ClipboardGroup>,
    entries: Vec<ClipboardEntry>,
}

impl ClipboardStore {
    pub fn open(data_dir: PathBuf) -> Self {
        let _ = fs::create_dir_all(blobs_dir(&data_dir));
        let mut loaded = load_snapshot(&data_dir);
        sort_entries(&mut loaded.entries);
        Self {
            data_dir,
            next_id: loaded.next_id,
            next_group_id: loaded.next_group_id,
            groups: loaded.groups,
            entries: loaded.entries,
        }
    }

    #[allow(clippy::too_many_arguments)]
    pub fn add_item(
        &mut self,
        raw_kind: u8,
        raw_text: &str,
        raw_image_file: &str,
        image_width: u32,
        image_height: u32,
        raw_hash: &str,
        source_app: &str,
        byte_size: u64,
        max_items: u32,
        max_days: u32,
        raw_html_file: &str,
        raw_rtf_file: &str,
    ) -> u64 {
        let mut kind = ClipKind::from_u8(raw_kind);
        let mut text = if raw_text.len() > MAX_TEXT_BYTES {
            truncate_utf8_bytes(raw_text, MAX_TEXT_BYTES)
        } else {
            raw_text.to_string()
        };
        let image_rel_path = sanitize_blob_filename(raw_image_file);
        let html_rel_path = sanitize_blob_filename(raw_html_file);
        let rtf_rel_path = sanitize_blob_filename(raw_rtf_file);

        let mut color_hex = String::new();
        if kind == ClipKind::Text {
            if let Some(hex) = detect_color_hex(&text) {
                kind = ClipKind::Color;
                color_hex = hex;
            }
        } else if kind == ClipKind::Color {
            color_hex = detect_color_hex(&text).unwrap_or_else(|| text.trim().to_string());
        }

        if kind != ClipKind::Image && text.trim().is_empty() {
            return 0;
        }
        if kind == ClipKind::Image && image_rel_path.is_empty() {
            return 0;
        }

        let content_hash = if !raw_hash.trim().is_empty() {
            raw_hash.trim().to_ascii_lowercase()
        } else {
            let mut key_bytes = Vec::with_capacity(text.len() + html_rel_path.len() + rtf_rel_path.len() + 16);
            key_bytes.push(kind as u8);
            key_bytes.extend_from_slice(text.as_bytes());
            if !html_rel_path.is_empty() {
                key_bytes.push(0x1e);
                key_bytes.extend_from_slice(html_rel_path.as_bytes());
            }
            if !rtf_rel_path.is_empty() {
                key_bytes.push(0x1f);
                key_bytes.extend_from_slice(rtf_rel_path.as_bytes());
            }
            fnv1a_128_hex(&key_bytes)
        };

        let now = now_epoch_ms();

        // 去重顶置：命中同类型 + 同内容哈希时更新时间戳、计数与来源，保留置顶与分组归属，不产生重复垃圾记录
        if let Some(existing) = self
            .entries
            .iter_mut()
            .find(|e| e.kind == kind && !e.content_hash.is_empty() && e.content_hash == content_hash)
        {
            existing.updated_ms = now;
            existing.copy_count = existing.copy_count.saturating_add(1);
            if !source_app.is_empty() {
                existing.source_app = source_app.to_string();
            }
            if !image_rel_path.is_empty() {
                existing.image_rel_path = image_rel_path;
            }
            if !html_rel_path.is_empty() {
                existing.html_rel_path = html_rel_path;
            }
            if !rtf_rel_path.is_empty() {
                existing.rtf_rel_path = rtf_rel_path;
            }
            if image_width > 0 {
                existing.image_width = image_width;
            }
            if image_height > 0 {
                existing.image_height = image_height;
            }
            existing.rebuild_derived_fields();
            let id = existing.id;
            sort_entries(&mut self.entries);
            let _ = self.persist();
            return id;
        }

        let id = self.next_id;
        self.next_id = self.next_id.saturating_add(1);

        let effective_bytes = if byte_size > 0 {
            byte_size
        } else {
            text.len() as u64
        };
        if kind == ClipKind::Image && text.is_empty() {
            text = format!("Image {}x{}", image_width, image_height);
        }

        let mut entry = ClipboardEntry {
            id,
            kind,
            text,
            preview: String::new(),
            image_rel_path,
            image_width,
            image_height,
            content_hash,
            source_app: source_app.to_string(),
            color_hex,
            byte_size: effective_bytes,
            created_ms: now,
            updated_ms: now,
            pinned: false,
            copy_count: 1,
            group_id: String::new(),
            html_rel_path,
            rtf_rel_path,
            search_blob_lower: String::new(),
        };
        entry.rebuild_derived_fields();
        self.entries.push(entry);
        sort_entries(&mut self.entries);

        self.apply_retention(max_items, max_days, now);
        let _ = self.persist();
        id
    }

    pub fn query_json(
        &self,
        query: &str,
        filter_kind: &str,
        group_id: &str,
        limit: usize,
        offset: usize,
    ) -> String {
        self.query_json_with_options(query, filter_kind, group_id, limit, offset, false)
    }

    pub fn query_json_with_options(
        &self,
        query: &str,
        filter_kind: &str,
        group_id: &str,
        limit: usize,
        offset: usize,
        exclude_pinned: bool,
    ) -> String {
        let terms: Vec<String> = query
            .split_whitespace()
            .map(|s| s.to_lowercase())
            .filter(|s| !s.is_empty())
            .collect();
        let kind_filter = filter_kind.trim().to_ascii_lowercase();
        let group_filter = group_id.trim();
        let cap = if limit == 0 { 100 } else { limit.min(500) };
        let should_exclude_pinned = exclude_pinned && kind_filter != "pinned";

        let mut total_matched = 0usize;
        let mut total_pinned = 0usize;
        let mut hidden_pinned = 0usize;
        let mut matched: Vec<&ClipboardEntry> = Vec::with_capacity(cap);

        for entry in &self.entries {
            if entry.pinned {
                total_pinned += 1;
            }
            if !matches_kind_filter(entry, &kind_filter) {
                continue;
            }
            if !group_filter.is_empty() && entry.group_id != group_filter {
                continue;
            }
            if !terms.is_empty()
                && !terms
                    .iter()
                    .all(|term| entry.search_blob_lower.contains(term.as_str()))
            {
                continue;
            }
            if should_exclude_pinned && entry.pinned {
                hidden_pinned += 1;
                continue;
            }
            if total_matched >= offset && matched.len() < cap {
                matched.push(entry);
            }
            total_matched += 1;
        }

        let mut out = String::with_capacity(matched.len().saturating_mul(288) + 128);
        out.push_str("{\"total\":");
        out.push_str(&self.entries.len().to_string());
        out.push_str(",\"matched\":");
        out.push_str(&total_matched.to_string());
        out.push_str(",\"pinnedCount\":");
        out.push_str(&total_pinned.to_string());
        out.push_str(",\"hiddenPinnedCount\":");
        out.push_str(&hidden_pinned.to_string());
        out.push_str(",\"items\":[");
        for (idx, item) in matched.iter().enumerate() {
            if idx > 0 {
                out.push(',');
            }
            out.push_str(&item.to_json(false));
        }
        out.push_str("]}");
        out
    }

    pub fn get_item_json(&self, id: u64) -> Option<String> {
        self.entries
            .iter()
            .find(|e| e.id == id)
            .map(|e| e.to_json(true))
    }

    pub fn toggle_pin(&mut self, id: u64) -> Option<bool> {
        let entry = self.entries.iter_mut().find(|e| e.id == id)?;
        entry.pinned = !entry.pinned;
        let now_pinned = entry.pinned;
        sort_entries(&mut self.entries);
        let _ = self.persist();
        Some(now_pinned)
    }

    pub fn delete_item(&mut self, id: u64) -> bool {
        let before = self.entries.len();
        self.entries.retain(|e| e.id != id);
        if self.entries.len() == before {
            return false;
        }
        let _ = self.persist();
        gc_unreferenced_blobs(&self.data_dir, &self.entries);
        true
    }

    pub fn delete_items_batch(&mut self, ids: &[u64], skip_pinned: bool) -> u32 {
        if ids.is_empty() {
            return 0;
        }
        let target: HashSet<u64> = ids.iter().copied().collect();
        let before = self.entries.len();
        self.entries.retain(|e| {
            if !target.contains(&e.id) {
                return true;
            }
            skip_pinned && e.pinned
        });
        let removed = before.saturating_sub(self.entries.len()) as u32;
        if removed > 0 {
            let _ = self.persist();
            gc_unreferenced_blobs(&self.data_dir, &self.entries);
        }
        removed
    }

    pub fn update_item_text(&mut self, id: u64, raw_text: &str) -> bool {
        let text = if raw_text.len() > MAX_TEXT_BYTES {
            truncate_utf8_bytes(raw_text, MAX_TEXT_BYTES)
        } else {
            raw_text.to_string()
        };
        if text.trim().is_empty() {
            return false;
        }

        let mut had_rich_blob = false;
        let Some(entry) = self.entries.iter_mut().find(|e| e.id == id) else {
            return false;
        };
        if entry.kind == ClipKind::Image || entry.kind == ClipKind::Files {
            return false;
        }

        if let Some(hex) = detect_color_hex(&text) {
            entry.kind = ClipKind::Color;
            entry.color_hex = hex;
        } else {
            entry.kind = ClipKind::Text;
            entry.color_hex.clear();
        }

        if !entry.html_rel_path.is_empty() || !entry.rtf_rel_path.is_empty() {
            had_rich_blob = true;
            entry.html_rel_path.clear();
            entry.rtf_rel_path.clear();
        }

        let mut key_bytes = Vec::with_capacity(text.len() + 4);
        key_bytes.push(entry.kind as u8);
        key_bytes.extend_from_slice(text.as_bytes());
        entry.content_hash = fnv1a_128_hex(&key_bytes);
        entry.byte_size = text.len() as u64;
        entry.text = text;
        entry.updated_ms = now_epoch_ms();
        entry.rebuild_derived_fields();

        sort_entries(&mut self.entries);
        let _ = self.persist();
        if had_rich_blob {
            gc_unreferenced_blobs(&self.data_dir, &self.entries);
        }
        true
    }

    pub fn list_groups_json(&self) -> String {
        let mut out = String::with_capacity(self.groups.len().saturating_mul(96) + 32);
        out.push_str("{\"groups\":[");
        for (idx, g) in self.groups.iter().enumerate() {
            if idx > 0 {
                out.push(',');
            }
            let count = self.entries.iter().filter(|e| e.group_id == g.id).count();
            out.push_str(&g.to_json(count));
        }
        out.push_str("]}");
        out
    }

    pub fn create_group(&mut self, raw_name: &str) -> Option<String> {
        if self.groups.len() >= MAX_GROUPS {
            return None;
        }
        let name = sanitize_group_name(raw_name);
        if name.is_empty() {
            return None;
        }
        if self
            .groups
            .iter()
            .any(|g| g.name.eq_ignore_ascii_case(&name))
        {
            return None;
        }
        let gid = format!("grp_{}", self.next_group_id);
        self.next_group_id = self.next_group_id.saturating_add(1);
        let group = ClipboardGroup {
            id: gid,
            name,
            created_ms: now_epoch_ms(),
        };
        let json = group.to_json(0);
        self.groups.push(group);
        let _ = self.persist();
        Some(json)
    }

    pub fn rename_group(&mut self, group_id: &str, raw_new_name: &str) -> bool {
        let new_name = sanitize_group_name(raw_new_name);
        if group_id.is_empty() || new_name.is_empty() {
            return false;
        }
        if self
            .groups
            .iter()
            .any(|g| g.id != group_id && g.name.eq_ignore_ascii_case(&new_name))
        {
            return false;
        }
        let Some(target) = self.groups.iter_mut().find(|g| g.id == group_id) else {
            return false;
        };
        target.name = new_name;
        let _ = self.persist();
        true
    }

    pub fn delete_group(&mut self, group_id: &str) -> bool {
        if group_id.is_empty() {
            return false;
        }
        let before = self.groups.len();
        self.groups.retain(|g| g.id != group_id);
        if self.groups.len() == before {
            return false;
        }
        // 安全解绑条目所属分组，绝不删除任何剪贴板历史条目
        for e in &mut self.entries {
            if e.group_id == group_id {
                e.group_id.clear();
            }
        }
        let _ = self.persist();
        true
    }

    pub fn set_items_group(&mut self, ids: &[u64], group_id: &str) -> u32 {
        if ids.is_empty() {
            return 0;
        }
        let trimmed = group_id.trim();
        if !trimmed.is_empty() && !self.groups.iter().any(|g| g.id == trimmed) {
            return 0;
        }
        let target: HashSet<u64> = ids.iter().copied().collect();
        let mut updated = 0u32;
        for e in &mut self.entries {
            if target.contains(&e.id) && e.group_id != trimmed {
                e.group_id = trimmed.to_string();
                updated += 1;
            }
        }
        if updated > 0 {
            let _ = self.persist();
        }
        updated
    }

    pub fn clear_history(&mut self, keep_pinned: bool) -> u32 {
        let before = self.entries.len();
        if keep_pinned {
            self.entries.retain(|e| e.pinned);
        } else {
            self.entries.clear();
        }
        let removed = before.saturating_sub(self.entries.len()) as u32;
        if removed > 0 {
            let _ = self.persist();
            gc_unreferenced_blobs(&self.data_dir, &self.entries);
        }
        removed
    }

    pub fn prune(&mut self, max_items: u32, max_days: u32) -> u32 {
        let removed = self.apply_retention(max_items, max_days, now_epoch_ms());
        if removed > 0 {
            let _ = self.persist();
            gc_unreferenced_blobs(&self.data_dir, &self.entries);
        }
        removed
    }

    pub fn stats_json(&self) -> String {
        let mut pinned_count = 0usize;
        let mut image_count = 0usize;
        let mut text_count = 0usize;
        let mut files_count = 0usize;
        let mut color_count = 0usize;
        let mut total_bytes = 0u64;

        for e in &self.entries {
            if e.pinned {
                pinned_count += 1;
            }
            match e.kind {
                ClipKind::Text => text_count += 1,
                ClipKind::Image => image_count += 1,
                ClipKind::Files => files_count += 1,
                ClipKind::Color => color_count += 1,
            }
            total_bytes = total_bytes.saturating_add(e.byte_size);
        }

        let mut disk_bytes = fs::metadata(self.data_dir.join("history.t3k"))
            .map(|m| m.len())
            .unwrap_or(0);
        if let Ok(rd) = fs::read_dir(blobs_dir(&self.data_dir)) {
            for ent in rd.flatten() {
                if let Ok(m) = ent.metadata() {
                    disk_bytes = disk_bytes.saturating_add(m.len());
                }
            }
        }

        let mut out = String::with_capacity(288);
        out.push_str("{\"totalCount\":");
        out.push_str(&self.entries.len().to_string());
        out.push_str(",\"pinnedCount\":");
        out.push_str(&pinned_count.to_string());
        out.push_str(",\"groupCount\":");
        out.push_str(&self.groups.len().to_string());
        out.push_str(",\"textCount\":");
        out.push_str(&text_count.to_string());
        out.push_str(",\"imageCount\":");
        out.push_str(&image_count.to_string());
        out.push_str(",\"filesCount\":");
        out.push_str(&files_count.to_string());
        out.push_str(",\"colorCount\":");
        out.push_str(&color_count.to_string());
        out.push_str(",\"contentBytes\":");
        out.push_str(&total_bytes.to_string());
        out.push_str(",\"diskBytes\":");
        out.push_str(&disk_bytes.to_string());
        out.push_str(",\"dataDir\":");
        push_json_str(&self.data_dir.to_string_lossy(), &mut out);
        out.push('}');
        out
    }

    fn apply_retention(&mut self, max_items: u32, max_days: u32, now_ms: u64) -> u32 {
        let before = self.entries.len();

        // 1. 按保留天数淘汰非置顶过期条目（置顶与已归属收藏分组的条目永久安全保留）
        if max_days > 0 {
            let cutoff_ms = now_ms.saturating_sub((max_days as u64).saturating_mul(86_400_000));
            self.entries
                .retain(|e| e.pinned || !e.group_id.is_empty() || e.updated_ms >= cutoff_ms);
        }

        // 2. 按最大条数上限淘汰最旧的普通未置顶未归组条目
        let cap = if max_items == 0 {
            500usize
        } else {
            (max_items as usize).clamp(20, 5000)
        };
        let mut transient_seen = 0usize;
        self.entries.retain(|e| {
            if e.pinned || !e.group_id.is_empty() {
                true
            } else {
                transient_seen += 1;
                transient_seen <= cap
            }
        });

        let removed = before.saturating_sub(self.entries.len()) as u32;
        if removed > 0 {
            gc_unreferenced_blobs(&self.data_dir, &self.entries);
        }
        removed
    }

    fn persist(&self) -> Result<(), String> {
        save_snapshot(
            &self.data_dir,
            self.next_id,
            self.next_group_id,
            &self.groups,
            &self.entries,
        )
    }
}

fn matches_kind_filter(entry: &ClipboardEntry, filter: &str) -> bool {
    match filter {
        "" | "all" => true,
        "pinned" => entry.pinned,
        "text" => entry.kind == ClipKind::Text || entry.kind == ClipKind::Color,
        "image" => entry.kind == ClipKind::Image,
        "files" => entry.kind == ClipKind::Files,
        "color" => entry.kind == ClipKind::Color,
        _ => true,
    }
}

/// 置顶优先，同优先级按最近更新时间戳倒序排列
fn sort_entries(entries: &mut [ClipboardEntry]) {
    entries.sort_by(|a, b| {
        b.pinned
            .cmp(&a.pinned)
            .then_with(|| b.updated_ms.cmp(&a.updated_ms))
            .then_with(|| b.id.cmp(&a.id))
    });
}

fn truncate_utf8_bytes(s: &str, max_bytes: usize) -> String {
    if s.len() <= max_bytes {
        return s.to_string();
    }
    let mut end = max_bytes;
    while end > 0 && !s.is_char_boundary(end) {
        end -= 1;
    }
    s[..end].to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::env;

    fn make_temp_dir(tag: &str) -> PathBuf {
        let p = env::temp_dir().join(format!("t3k_clip_test_{}_{}", tag, now_epoch_ms()));
        let _ = fs::remove_dir_all(&p);
        fs::create_dir_all(&p).unwrap();
        p
    }

    #[test]
    fn persists_across_restart_deduplicates_and_protects_pinned_items() {
        let dir = make_temp_dir("persist");

        {
            let mut store = ClipboardStore::open(dir.clone());
            let id1 = store.add_item(0, "Hello Tools3000 世界级架构", "", 0, 0, "", "Code.exe", 0, 50, 30, "", "");
            assert!(id1 > 0);
            let id2 = store.add_item(0, "#10b981", "", 0, 0, "", "Figma.exe", 0, 50, 30, "", "");
            assert_ne!(id1, id2);
            let id3 = store.add_item(1, "", "img_abc123.png", 1920, 1080, "hash_img_1", "Tools3000.exe", 42000, 50, 30, "", "");
            assert!(id3 > id2);

            let id1_dup = store.add_item(0, "Hello Tools3000 世界级架构", "", 0, 0, "", "WindowsTerminal.exe", 0, 50, 30, "", "");
            assert_eq!(id1_dup, id1);

            assert_eq!(store.toggle_pin(id2), Some(true));
        }

        {
            let mut reopened = ClipboardStore::open(dir.clone());
            let all = reopened.query_json("", "all", "", 20, 0);
            assert!(all.contains("\"total\":3"), "actual={}", all);
            assert!(all.contains("\"kind\":\"color\""), "actual={}", all);
            assert!(all.contains("\"colorHex\":\"#10B981\""), "actual={}", all);
            assert!(all.contains("\"copyCount\":2"), "actual={}", all);
            assert!(all.contains("\"imageFile\":\"img_abc123.png\""), "actual={}", all);

            let search = reopened.query_json("世界级 terminal", "all", "", 20, 0);
            assert!(search.contains("\"matched\":1"), "actual={}", search);

            let removed = reopened.clear_history(true);
            assert_eq!(removed, 2);
            let after = reopened.query_json("", "all", "", 20, 0);
            assert!(after.contains("\"total\":1"));
            assert!(after.contains("#10B981"));
        }

        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn enforces_max_items_retention_and_gcs_orphan_blobs() {
        let dir = make_temp_dir("retention");
        let orphan_blob = blobs_dir(&dir).join("orphan_old.png");
        fs::create_dir_all(blobs_dir(&dir)).unwrap();
        fs::write(&orphan_blob, b"fake_png_bytes").unwrap();
        assert!(orphan_blob.exists());

        let mut store = ClipboardStore::open(dir.clone());
        let pinned_id = store.add_item(0, "Permanent Pinned Snippet", "", 0, 0, "", "Code.exe", 0, 20, 30, "", "");
        assert_eq!(store.toggle_pin(pinned_id), Some(true));

        let img_blob = blobs_dir(&dir).join("keep_img.png");
        fs::write(&img_blob, b"png_data").unwrap();
        let img_id = store.add_item(1, "", "keep_img.png", 100, 100, "img_hash_keep", "Snip.exe", 8, 20, 30, "", "");
        assert_eq!(store.toggle_pin(img_id), Some(true));

        for i in 0..35 {
            store.add_item(0, &format!("Transient entry #{}", i), "", 0, 0, "", "Notepad.exe", 0, 20, 30, "", "");
        }

        let stats = store.stats_json();
        assert!(stats.contains("\"totalCount\":22"), "stats={}", stats);
        assert!(stats.contains("\"pinnedCount\":2"), "stats={}", stats);
        assert!(!orphan_blob.exists());
        assert!(img_blob.exists());

        let snap = dir.join("history.t3k");
        let mut raw = fs::read(&snap).unwrap();
        raw[10] ^= 0xFF;
        fs::write(&snap, &raw).unwrap();
        let recovered = ClipboardStore::open(dir.clone());
        assert!(recovered.query_json("", "all", "", 10, 0).contains("\"total\":0"));

        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn supports_groups_batch_delete_inline_edit_and_rich_text_blob_gc() {
        let dir = make_temp_dir("rich_group_features");
        fs::create_dir_all(blobs_dir(&dir)).unwrap();
        let html_blob = blobs_dir(&dir).join("h_rich1.html");
        fs::write(&html_blob, b"<b>Bold Title</b>").unwrap();

        let id1: u64;
        let id2: u64;
        let id3: u64;
        {
            let mut store = ClipboardStore::open(dir.clone());
            id1 = store.add_item(0, "Bold Title", "", 0, 0, "", "Chrome.exe", 10, 50, 30, "h_rich1.html", "");
            id2 = store.add_item(0, "Second snippet", "", 0, 0, "", "Code.exe", 14, 50, 30, "", "");
            id3 = store.add_item(0, "Third snippet", "", 0, 0, "", "Code.exe", 13, 50, 30, "", "");

            // 列表项仅带 hasRichText 布尔标志，不携带大富文本内容；详情项带 htmlFile
            let list = store.query_json("", "all", "", 10, 0);
            assert!(list.contains("\"hasRichText\":true"), "list={}", list);
            assert!(!list.contains("\"htmlFile\""), "list should omit htmlFile: {}", list);
            let detail = store.get_item_json(id1).unwrap();
            assert!(detail.contains("\"htmlFile\":\"h_rich1.html\""), "detail={}", detail);

            // 创建分组 -> 归组 -> 筛选 -> 重命名
            let g_json = store.create_group("常用代码").unwrap();
            assert!(g_json.contains("\"id\":\"grp_1\""), "g={}", g_json);
            assert_eq!(store.set_items_group(&[id1, id2], "grp_1"), 2);
            let filtered = store.query_json("", "all", "grp_1", 10, 0);
            assert!(filtered.contains("\"matched\":2"), "filtered={}", filtered);
            assert!(store.rename_group("grp_1", "工作常用"));
            assert!(store.list_groups_json().contains("工作常用"));
        }

        // 重启恢复验证完整持久化
        {
            let mut reopened = ClipboardStore::open(dir.clone());
            assert!(reopened.list_groups_json().contains("\"count\":2"));

            // 纯文本内联编辑：更新文本并自动清理旧 HTML 富文本孤儿 blob
            assert!(reopened.update_item_text(id1, "#FF6600"));
            let edited = reopened.get_item_json(id1).unwrap();
            assert!(edited.contains("\"kind\":\"color\""), "edited={}", edited);
            assert!(edited.contains("\"colorHex\":\"#FF6600\""), "edited={}", edited);
            assert!(edited.contains("\"hasRichText\":false"), "edited={}", edited);
            assert!(!html_blob.exists(), "stale rich html blob should be GCed on plain-text edit");

            // 删除分组不删除条目
            assert!(reopened.delete_group("grp_1"));
            assert!(reopened.query_json("", "all", "", 10, 0).contains("\"total\":3"));

            // 批量删除支持 skip_pinned=true 与 false
            assert_eq!(reopened.toggle_pin(id2), Some(true));
            assert_eq!(reopened.delete_items_batch(&[id2, id3], true), 1);
            assert!(reopened.query_json("", "all", "", 10, 0).contains("\"total\":2"));
            assert_eq!(reopened.delete_items_batch(&[id1, id2], false), 2);
            assert!(reopened.query_json("", "all", "", 10, 0).contains("\"total\":0"));
        }

        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn excludes_pinned_items_in_non_pinned_categories_while_keeping_pinned_tab_intact() {
        let dir = make_temp_dir("exclude_pinned");
        let mut store = ClipboardStore::open(dir.clone());
        let id_pinned_text = store.add_item(0, "Pinned Alpha", "", 0, 0, "", "Code.exe", 12, 50, 30, "", "");
        let id_normal_text = store.add_item(0, "Normal Beta", "", 0, 0, "", "Notepad.exe", 11, 50, 30, "", "");
        let id_pinned_color = store.add_item(0, "#2563EB", "", 0, 0, "", "Figma.exe", 7, 50, 30, "", "");
        assert_eq!(store.toggle_pin(id_pinned_text), Some(true));
        assert_eq!(store.toggle_pin(id_pinned_color), Some(true));

        // 1. exclude_pinned=false 时，all 返回全部 3 条，pinnedCount=2, hiddenPinnedCount=0
        let all_with_pinned = store.query_json_with_options("", "all", "", 10, 0, false);
        assert!(all_with_pinned.contains("\"matched\":3"), "actual={}", all_with_pinned);
        assert!(all_with_pinned.contains("\"pinnedCount\":2"), "actual={}", all_with_pinned);
        assert!(all_with_pinned.contains("\"hiddenPinnedCount\":0"), "actual={}", all_with_pinned);

        // 2. exclude_pinned=true 时，all 与 text 仅返回非置顶项，hiddenPinnedCount=2
        let all_without_pinned = store.query_json_with_options("", "all", "", 10, 0, true);
        assert!(all_without_pinned.contains("\"total\":3"), "actual={}", all_without_pinned);
        assert!(all_without_pinned.contains("\"matched\":1"), "actual={}", all_without_pinned);
        assert!(all_without_pinned.contains("\"pinnedCount\":2"), "actual={}", all_without_pinned);
        assert!(all_without_pinned.contains("\"hiddenPinnedCount\":2"), "actual={}", all_without_pinned);
        assert!(all_without_pinned.contains("Normal Beta"), "actual={}", all_without_pinned);
        assert!(!all_without_pinned.contains("Pinned Alpha"), "actual={}", all_without_pinned);
        assert!(all_without_pinned.contains(&format!("\"id\":{}", id_normal_text)));

        let text_without_pinned = store.query_json_with_options("", "text", "", 10, 0, true);
        assert!(text_without_pinned.contains("\"matched\":1"), "actual={}", text_without_pinned);
        assert!(text_without_pinned.contains("\"hiddenPinnedCount\":2"), "actual={}", text_without_pinned);

        // 3. pinned 分类下即便传 exclude_pinned=true，依然完整保留全部置顶条目
        let pinned_tab = store.query_json_with_options("", "pinned", "", 10, 0, true);
        assert!(pinned_tab.contains("\"matched\":2"), "actual={}", pinned_tab);
        assert!(pinned_tab.contains("\"hiddenPinnedCount\":0"), "actual={}", pinned_tab);
        assert!(pinned_tab.contains("Pinned Alpha"), "actual={}", pinned_tab);
        assert!(pinned_tab.contains("#2563EB"), "actual={}", pinned_tab);

        let _ = fs::remove_dir_all(&dir);
    }
}
