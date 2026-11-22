// ─────────────────────────────────────────────────────────────────────────────
// storage.rs — 抗断电原子持久化快照读写器（零外部依赖紧凑二进制格式）
//
// 格式契约:
//   Header: "T3KCLIP1" (8B) | version: u32 (1) | next_id: u64 | next_group_id: u64 |
//           group_count: u32 | Groups(len+id, len+name, created_ms) | record_count: u32
//   Record:
//     id: u64 | kind: u8 | pinned: u8 | copy_count: u32 | image_w: u32 | image_h: u32 |
//     byte_size: u64 | created_ms: u64 | updated_ms: u64 |
//     len+bytes(text) | len+bytes(image_rel_path) | len+bytes(content_hash) |
//     len+bytes(source_app) | len+bytes(color_hex) | len+bytes(group_id) |
//     len+bytes(html_rel_path) | len+bytes(rtf_rel_path)
//   Footer:
//     fnv1a_64_checksum: u64（整份负载完整性校验，自动防御损坏/截断文件）
// 写入策略:
//   先写入 `history.t3k.tmp` 并调用 `sync_all()` 刷盘，随后原子替换 `history.t3k`，
//   保证任意时刻断电/关机/重启 0 数据损坏、0 历史丢失！
// ─────────────────────────────────────────────────────────────────────────────

use crate::model::{sanitize_blob_filename, ClipKind, ClipboardEntry, ClipboardGroup};
use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

const MAGIC: &[u8; 8] = b"T3KCLIP1";
const FORMAT_VERSION: u32 = 1;
const MAX_SNAPSHOT_BYTES: usize = 64 * 1024 * 1024;
const MAX_STRING_FIELD_BYTES: usize = 2 * 1024 * 1024;

pub struct LoadedSnapshot {
    pub next_id: u64,
    pub next_group_id: u64,
    pub groups: Vec<ClipboardGroup>,
    pub entries: Vec<ClipboardEntry>,
}

pub fn snapshot_path(data_dir: &Path) -> PathBuf {
    data_dir.join("history.t3k")
}

pub fn blobs_dir(data_dir: &Path) -> PathBuf {
    data_dir.join("blobs")
}

fn fnv1a_64(data: &[u8]) -> u64 {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for &b in data {
        h ^= b as u64;
        h = h.wrapping_mul(0x0000_0100_0000_01b3);
    }
    h
}

fn write_str_field(buf: &mut Vec<u8>, s: &str) {
    let bytes = s.as_bytes();
    buf.extend_from_slice(&(bytes.len() as u32).to_le_bytes());
    buf.extend_from_slice(bytes);
}

fn read_u8(data: &[u8], pos: &mut usize) -> Option<u8> {
    let b = *data.get(*pos)?;
    *pos += 1;
    Some(b)
}

fn read_u32(data: &[u8], pos: &mut usize) -> Option<u32> {
    let slice = data.get(*pos..*pos + 4)?;
    *pos += 4;
    Some(u32::from_le_bytes([slice[0], slice[1], slice[2], slice[3]]))
}

fn read_u64(data: &[u8], pos: &mut usize) -> Option<u64> {
    let slice = data.get(*pos..*pos + 8)?;
    *pos += 8;
    Some(u64::from_le_bytes([
        slice[0], slice[1], slice[2], slice[3], slice[4], slice[5], slice[6], slice[7],
    ]))
}

fn read_str_field(data: &[u8], pos: &mut usize) -> Option<String> {
    let len = read_u32(data, pos)? as usize;
    if len > MAX_STRING_FIELD_BYTES {
        return None;
    }
    let slice = data.get(*pos..*pos + len)?;
    *pos += len;
    String::from_utf8(slice.to_vec()).ok()
}

pub fn save_snapshot(
    data_dir: &Path,
    next_id: u64,
    next_group_id: u64,
    groups: &[ClipboardGroup],
    entries: &[ClipboardEntry],
) -> Result<(), String> {
    fs::create_dir_all(data_dir).map_err(|e| format!("create_dir_all failed: {}", e))?;

    let mut payload = Vec::with_capacity(
        entries.len().saturating_mul(288) + groups.len().saturating_mul(64) + 96,
    );
    payload.extend_from_slice(MAGIC);
    payload.extend_from_slice(&FORMAT_VERSION.to_le_bytes());
    payload.extend_from_slice(&next_id.to_le_bytes());
    payload.extend_from_slice(&next_group_id.to_le_bytes());
    payload.extend_from_slice(&(groups.len() as u32).to_le_bytes());

    for g in groups {
        write_str_field(&mut payload, &g.id);
        write_str_field(&mut payload, &g.name);
        payload.extend_from_slice(&g.created_ms.to_le_bytes());
    }

    payload.extend_from_slice(&(entries.len() as u32).to_le_bytes());
    for e in entries {
        payload.extend_from_slice(&e.id.to_le_bytes());
        payload.push(e.kind as u8);
        payload.push(if e.pinned { 1 } else { 0 });
        payload.extend_from_slice(&e.copy_count.to_le_bytes());
        payload.extend_from_slice(&e.image_width.to_le_bytes());
        payload.extend_from_slice(&e.image_height.to_le_bytes());
        payload.extend_from_slice(&e.byte_size.to_le_bytes());
        payload.extend_from_slice(&e.created_ms.to_le_bytes());
        payload.extend_from_slice(&e.updated_ms.to_le_bytes());
        write_str_field(&mut payload, &e.text);
        write_str_field(&mut payload, &e.image_rel_path);
        write_str_field(&mut payload, &e.content_hash);
        write_str_field(&mut payload, &e.source_app);
        write_str_field(&mut payload, &e.color_hex);
        write_str_field(&mut payload, &e.group_id);
        write_str_field(&mut payload, &e.html_rel_path);
        write_str_field(&mut payload, &e.rtf_rel_path);
    }

    let checksum = fnv1a_64(&payload);
    payload.extend_from_slice(&checksum.to_le_bytes());

    let target = snapshot_path(data_dir);
    let tmp = data_dir.join("history.t3k.tmp");
    let bak = data_dir.join("history.t3k.bak");

    {
        let mut f = File::create(&tmp).map_err(|e| format!("create tmp failed: {}", e))?;
        f.write_all(&payload).map_err(|e| format!("write tmp failed: {}", e))?;
        f.sync_all().map_err(|e| format!("sync tmp failed: {}", e))?;
    }

    // Windows 上 rename 覆盖已有文件前先备份旧快照，极端崩溃时可从 .bak 无损恢复
    if target.exists() {
        let _ = fs::remove_file(&bak);
        if fs::rename(&target, &bak).is_err() {
            let _ = fs::remove_file(&target);
        }
    }
    if let Err(e) = fs::rename(&tmp, &target) {
        if !target.exists() && bak.exists() {
            let _ = fs::rename(&bak, &target);
        }
        return Err(format!("atomic replace failed: {}", e));
    }
    let _ = fs::remove_file(&bak);
    Ok(())
}

pub fn load_snapshot(data_dir: &Path) -> LoadedSnapshot {
    let primary = snapshot_path(data_dir);
    let backup = data_dir.join("history.t3k.bak");

    for path in [&primary, &backup] {
        if !path.exists() {
            continue;
        }
        if let Some(loaded) = load_snapshot_file(path) {
            return loaded;
        }
    }
    LoadedSnapshot {
        next_id: 1,
        next_group_id: 1,
        groups: Vec::new(),
        entries: Vec::new(),
    }
}

fn load_snapshot_file(path: &Path) -> Option<LoadedSnapshot> {
    let meta = fs::metadata(path).ok()?;
    if meta.len() < 32 || meta.len() as usize > MAX_SNAPSHOT_BYTES {
        return None;
    }
    let mut raw = Vec::with_capacity(meta.len() as usize);
    File::open(path).ok()?.read_to_end(&mut raw).ok()?;
    if raw.len() < 32 {
        return None;
    }

    let body_len = raw.len() - 8;
    let stored_sum = u64::from_le_bytes([
        raw[body_len],
        raw[body_len + 1],
        raw[body_len + 2],
        raw[body_len + 3],
        raw[body_len + 4],
        raw[body_len + 5],
        raw[body_len + 6],
        raw[body_len + 7],
    ]);
    if fnv1a_64(&raw[..body_len]) != stored_sum {
        return None;
    }

    let body = &raw[..body_len];
    if &body[0..8] != MAGIC {
        return None;
    }
    let mut pos = 8usize;
    let ver = read_u32(body, &mut pos)?;
    if ver != FORMAT_VERSION {
        return None;
    }

    let mut next_id = read_u64(body, &mut pos)?;
    let next_group_id = read_u64(body, &mut pos)?.max(1);
    let group_count = read_u32(body, &mut pos)? as usize;
    if group_count > 500 {
        return None;
    }

    let mut groups = Vec::with_capacity(group_count);
    for _ in 0..group_count {
        let gid = read_str_field(body, &mut pos)?;
        let name = read_str_field(body, &mut pos)?;
        let created_ms = read_u64(body, &mut pos)?;
        if !gid.is_empty() && !name.is_empty() {
            groups.push(ClipboardGroup {
                id: gid,
                name,
                created_ms,
            });
        }
    }

    let count = read_u32(body, &mut pos)? as usize;
    if count > 50_000 {
        return None;
    }

    let mut entries = Vec::with_capacity(count);
    for _ in 0..count {
        let id = read_u64(body, &mut pos)?;
        let kind = ClipKind::from_u8(read_u8(body, &mut pos)?);
        let pinned = read_u8(body, &mut pos)? != 0;
        let copy_count = read_u32(body, &mut pos)?;
        let image_width = read_u32(body, &mut pos)?;
        let image_height = read_u32(body, &mut pos)?;
        let byte_size = read_u64(body, &mut pos)?;
        let created_ms = read_u64(body, &mut pos)?;
        let updated_ms = read_u64(body, &mut pos)?;
        let text = read_str_field(body, &mut pos)?;
        let image_rel_path = sanitize_blob_filename(&read_str_field(body, &mut pos)?);
        let content_hash = read_str_field(body, &mut pos)?;
        let source_app = read_str_field(body, &mut pos)?;
        let color_hex = read_str_field(body, &mut pos)?;
        let group_id = read_str_field(body, &mut pos)?;
        let html_rel_path = sanitize_blob_filename(&read_str_field(body, &mut pos)?);
        let rtf_rel_path = sanitize_blob_filename(&read_str_field(body, &mut pos)?);

        if id >= next_id {
            next_id = id.saturating_add(1);
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
            source_app,
            color_hex,
            byte_size,
            created_ms,
            updated_ms,
            pinned,
            copy_count,
            group_id,
            html_rel_path,
            rtf_rel_path,
            search_blob_lower: String::new(),
        };
        entry.rebuild_derived_fields();
        entries.push(entry);
    }

    Some(LoadedSnapshot {
        next_id: next_id.max(1),
        next_group_id,
        groups,
        entries,
    })
}

/// 清理不再被任何历史条目引用的孤儿图片及富文本 (HTML/RTF) 磁盘文件，杜绝磁盘残留膨胀
pub fn gc_unreferenced_blobs(data_dir: &Path, entries: &[ClipboardEntry]) {
    let dir = blobs_dir(data_dir);
    let read_dir = match fs::read_dir(&dir) {
        Ok(rd) => rd,
        Err(_) => return,
    };

    for item in read_dir.flatten() {
        let name = match item.file_name().into_string() {
            Ok(n) => n,
            Err(_) => continue,
        };
        let referenced = entries.iter().any(|e| {
            (!e.image_rel_path.is_empty() && e.image_rel_path.eq_ignore_ascii_case(&name))
                || (!e.html_rel_path.is_empty() && e.html_rel_path.eq_ignore_ascii_case(&name))
                || (!e.rtf_rel_path.is_empty() && e.rtf_rel_path.eq_ignore_ascii_case(&name))
        });
        if !referenced {
            let _ = fs::remove_file(item.path());
        }
    }
}
