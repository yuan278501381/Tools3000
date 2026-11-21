// ─────────────────────────────────────────────────────────────────────────────
// folder.rs — 同目录图像发现、Windows 资源管理器自然排序 (StrCmpLogicalW)
// ─────────────────────────────────────────────────────────────────────────────

use crate::constants::SUPPORTED_EXTENSIONS;
use std::cmp::Ordering;
use std::path::{Path, PathBuf};

/// 判断文件路径扩展名是否属于看图器支持的图像格式
pub fn is_supported_image_path(path: &Path) -> bool {
    let Some(ext) = path.extension().and_then(|e| e.to_str()) else {
        return false;
    };
    let lower = ext.to_ascii_lowercase();
    SUPPORTED_EXTENSIONS.iter().any(|&item| item == lower)
}

/// 按 Windows 资源管理器逻辑自然排序比较两个文件名（img2.png < img10.png）
pub fn compare_natural_names(a: &str, b: &str) -> Ordering {
    #[cfg(windows)]
    {
        use std::ffi::OsStr;
        use std::os::windows::ffi::OsStrExt;
        use windows::core::PCWSTR;
        use windows::Win32::UI::Shell::StrCmpLogicalW;

        let wa: Vec<u16> = OsStr::new(a).encode_wide().chain(Some(0)).collect();
        let wb: Vec<u16> = OsStr::new(b).encode_wide().chain(Some(0)).collect();
        let cmp = unsafe { StrCmpLogicalW(PCWSTR(wa.as_ptr()), PCWSTR(wb.as_ptr())) };
        cmp.cmp(&0)
    }
    #[cfg(not(windows))]
    {
        a.to_lowercase().cmp(&b.to_lowercase())
    }
}

/// 扫描目标图片所在目录内的全部同级图片，并定位当前图片索引
pub fn scan_sibling_images(target_file: &Path) -> (Vec<PathBuf>, usize) {
    let canonical_target = target_file.to_path_buf();
    let Some(parent) = canonical_target.parent() else {
        return (vec![canonical_target], 0);
    };

    let mut siblings: Vec<PathBuf> = Vec::new();
    if let Ok(entries) = std::fs::read_dir(parent) {
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_file() && is_supported_image_path(&path) {
                siblings.push(path);
            }
        }
    }

    // 如果传入文件自身扩展名不在白名单（例如用户强制右键“打开方式”打开无扩展名图），仍保底纳入
    let target_name_lower = canonical_target
        .file_name()
        .map(|s| s.to_string_lossy().to_lowercase())
        .unwrap_or_default();
    if !siblings.iter().any(|p| {
        p.file_name()
            .map(|s| s.to_string_lossy().to_lowercase())
            .unwrap_or_default()
            == target_name_lower
    }) && canonical_target.exists()
    {
        siblings.push(canonical_target.clone());
    }

    siblings.sort_by(|a, b| {
        let na = a
            .file_name()
            .map(|s| s.to_string_lossy())
            .unwrap_or_default();
        let nb = b
            .file_name()
            .map(|s| s.to_string_lossy())
            .unwrap_or_default();
        compare_natural_names(&na, &nb)
    });

    let current_idx = siblings
        .iter()
        .position(|p| {
            p.file_name()
                .map(|s| s.to_string_lossy().to_lowercase())
                .unwrap_or_default()
                == target_name_lower
        })
        .unwrap_or(0);

    (siblings, current_idx)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn natural_sort_matches_explorer_order() {
        let mut names = vec!["photo_10.webp", "photo_2.webp", "photo_1.webp", "Photo_3.gif"];
        names.sort_by(|a, b| compare_natural_names(a, b));
        assert_eq!(
            names,
            vec!["photo_1.webp", "photo_2.webp", "Photo_3.gif", "photo_10.webp"]
        );
    }

    #[test]
    fn supports_modern_and_classic_extensions() {
        assert!(is_supported_image_path(Path::new("demo.webp")));
        assert!(is_supported_image_path(Path::new("DEMO.GIF")));
        assert!(is_supported_image_path(Path::new("photo.avif")));
        assert!(is_supported_image_path(Path::new("vector.svg")));
        assert!(!is_supported_image_path(Path::new("notes.txt")));
    }
}
