// ─────────────────────────────────────────────────────────────────────────────
// store.rs — 剪贴板内存索引、内容哈希去重、亚毫秒多词检索与自动保留策略引擎
// ─────────────────────────────────────────────────────────────────────────────

use crate::model::{
    detect_color_hex, fnv1a_128_hex, now_epoch_ms, push_json_str, sanitize_blob_filename, ClipKind,
    ClipboardEntry, MAX_TEXT_BYTES,
};
use crate::storage::{blobs_dir, gc_unreferenced_blobs, load_snapshot, save_snapshot};
use std::fs;
use std::path::PathBuf;

pub struct ClipboardStore {
    data_dir: PathBuf,
    next_id: u64,
    entries: Vec<ClipboardEntry>,
}

impl ClipboardStore {
    pub fn open(data_dir: PathBuf) -> Self {
        let _ = fs::create_dir_all(blobs_dir(&data_dir));
        let (next_id, mut entries) = load_snapshot(&data_dir);
        sort_entries(&mut entries);
        Self {
            data_dir,
            next_id,
            entries,
        }
    }

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
    ) -> u64 {
        let mut kind = ClipKind::from_u8(raw_kind);
        let mut text = if raw_text.len() > MAX_TEXT_BYTES {
            truncate_utf8_bytes(raw_text, MAX_TEXT_BYTES)
        } else {
            raw_text.to_string()
        };
        let image_rel_path = sanitize_blob_filename(raw_image_file);

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
            let mut key_bytes = Vec::with_capacity(text.len() + 8);
            key_bytes.push(kind as u8);
            key_bytes.extend_from_slice(text.as_bytes());
            fnv1a_128_hex(&key_bytes)
        };

        let now = now_epoch_ms();

        // 去重顶置：命中同类型 + 同内容哈希时更新时间戳、计数与来源，保留置顶状态，不产生重复垃圾记录
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
            search_blob_lower: String::new(),
        };
        entry.rebuild_derived_fields();
        self.entries.push(entry);
        sort_entries(&mut self.entries);

        self.apply_retention(max_items, max_days, now);
        let _ = self.persist();
        id
    }

    pub fn query_json(&self, query: &str, filter_kind: &str, limit: usize, offset: usize) -> String {
        let terms: Vec<String> = query
            .split_whitespace()
            .map(|s| s.to_lowercase())
            .filter(|s| !s.is_empty())
            .collect();
        let kind_filter = filter_kind.trim().to_ascii_lowercase();
        let cap = if limit == 0 { 100 } else { limit.min(500) };

        let mut total_matched = 0usize;
        let mut matched: Vec<&ClipboardEntry> = Vec::with_capacity(cap);

        for entry in &self.entries {
            if !matches_kind_filter(entry, &kind_filter) {
                continue;
            }
            if !terms.is_empty()
                && !terms
                    .iter()
                    .all(|term| entry.search_blob_lower.contains(term.as_str()))
            {
                continue;
            }
            if total_matched >= offset && matched.len() < cap {
                matched.push(entry);
            }
            total_matched += 1;
        }

        let mut out = String::with_capacity(matched.len().saturating_mul(256) + 96);
        out.push_str("{\"total\":");
        out.push_str(&self.entries.len().to_string());
        out.push_str(",\"matched\":");
        out.push_str(&total_matched.to_string());
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

        // 计算磁盘实际占用（history.t3k + blobs/*）
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

        let mut out = String::with_capacity(256);
        out.push_str("{\"totalCount\":");
        out.push_str(&self.entries.len().to_string());
        out.push_str(",\"pinnedCount\":");
        out.push_str(&pinned_count.to_string());
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

        // 1. 按保留天数淘汰非置顶过期条目（置顶条目永久安全保留）
        if max_days > 0 {
            let cutoff_ms = now_ms.saturating_sub((max_days as u64).saturating_mul(86_400_000));
            self.entries.retain(|e| e.pinned || e.updated_ms >= cutoff_ms);
        }

        // 2. 按最大条数上限淘汰最旧的非置顶条目
        let cap = if max_items == 0 { 500usize } else { (max_items as usize).clamp(20, 5000) };
        let mut unpinned_seen = 0usize;
        self.entries.retain(|e| {
            if e.pinned {
                true
            } else {
                unpinned_seen += 1;
                unpinned_seen <= cap
            }
        });

        let removed = before.saturating_sub(self.entries.len()) as u32;
        if removed > 0 {
            gc_unreferenced_blobs(&self.data_dir, &self.entries);
        }
        removed
    }

    fn persist(&self) -> Result<(), String> {
        save_snapshot(&self.data_dir, self.next_id, &self.entries)
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
        let p = env::temp_dir().join(format!(
            "t3k_clip_test_{}_{}",
            tag,
            now_epoch_ms()
        ));
        let _ = fs::remove_dir_all(&p);
        fs::create_dir_all(&p).unwrap();
        p
    }

    #[test]
    fn persists_across_restart_deduplicates_and_protects_pinned_items() {
        let dir = make_temp_dir("persist");

        // 1. 首次写入文本、颜色、图片与去重项
        {
            let mut store = ClipboardStore::open(dir.clone());
            let id1 = store.add_item(0, "Hello Tools3000 世界级架构", "", 0, 0, "", "Code.exe", 0, 50, 30);
            assert!(id1 > 0);
            let id2 = store.add_item(0, "#10b981", "", 0, 0, "", "Figma.exe", 0, 50, 30);
            assert_ne!(id1, id2);
            let id3 = store.add_item(1, "", "img_abc123.png", 1920, 1080, "hash_img_1", "Tools3000.exe", 42000, 50, 30);
            assert!(id3 > id2);

            // 重复复制 id1 内容 -> 复用 id1 且 copyCount == 2，不新增重复记录
            let id1_dup = store.add_item(0, "Hello Tools3000 世界级架构", "", 0, 0, "", "WindowsTerminal.exe", 0, 50, 30);
            assert_eq!(id1_dup, id1);

            // 置顶颜色项
            assert_eq!(store.toggle_pin(id2), Some(true));
        }

        // 2. 模拟关机重启：全新打开同一目录，验证 100% 恢复持久化状态
        {
            let mut reopened = ClipboardStore::open(dir.clone());
            let all = reopened.query_json("", "all", 20, 0);
            assert!(all.contains("\"total\":3"), "actual={}", all);
            assert!(all.contains("\"kind\":\"color\""), "actual={}", all);
            assert!(all.contains("\"colorHex\":\"#10B981\""), "actual={}", all);
            assert!(all.contains("\"copyCount\":2"), "actual={}", all);
            assert!(all.contains("\"imageFile\":\"img_abc123.png\""), "actual={}", all);

            // 多词模糊搜索（内容 + 来源应用名组合匹配）
            let search = reopened.query_json("世界级 terminal", "all", 20, 0);
            assert!(search.contains("\"matched\":1"), "actual={}", search);

            // 清空未置顶 -> 仅置顶的颜色条目存活
            let removed = reopened.clear_history(true);
            assert_eq!(removed, 2);
            let after = reopened.query_json("", "all", 20, 0);
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
        let pinned_id = store.add_item(0, "Permanent Pinned Snippet", "", 0, 0, "", "Code.exe", 0, 20, 30);
        assert_eq!(store.toggle_pin(pinned_id), Some(true));

        let img_blob = blobs_dir(&dir).join("keep_img.png");
        fs::write(&img_blob, b"png_data").unwrap();
        let img_id = store.add_item(1, "", "keep_img.png", 100, 100, "img_hash_keep", "Snip.exe", 8, 20, 30);
        assert_eq!(store.toggle_pin(img_id), Some(true));

        for i in 0..35 {
            store.add_item(0, &format!("Transient entry #{}", i), "", 0, 0, "", "Notepad.exe", 0, 20, 30);
        }

        // 未置顶被严格限制为 20 条 + 2 条置顶项 = 总计 22 条，孤儿图片被自动 GC，引用图片保留
        let stats = store.stats_json();
        assert!(stats.contains("\"totalCount\":22"), "stats={}", stats);
        assert!(stats.contains("\"pinnedCount\":2"), "stats={}", stats);
        assert!(!orphan_blob.exists());
        assert!(img_blob.exists());

        // 篡改快照文件校验和：重新打开时优雅降级不 panic
        let snap = dir.join("history.t3k");
        let mut raw = fs::read(&snap).unwrap();
        raw[10] ^= 0xFF;
        fs::write(&snap, &raw).unwrap();
        let recovered = ClipboardStore::open(dir.clone());
        assert!(recovered.query_json("", "all", 10, 0).contains("\"total\":0"));

        let _ = fs::remove_dir_all(&dir);
    }
}
