// ─────────────────────────────────────────────────────────────────────────────
// build.rs — 嵌入 PerMonitorV2 高分屏清单、多尺寸应用图标与 Windows 版本资源
// ─────────────────────────────────────────────────────────────────────────────

use std::path::{Path, PathBuf};
use std::process::Command;

fn find_windows_rc_exe() -> Option<PathBuf> {
    if let Ok(rc) = std::env::var("RC") {
        let p = PathBuf::from(rc);
        if p.is_file() {
            return Some(p);
        }
    }

    let kits_root = Path::new(r"C:\Program Files (x86)\Windows Kits\10\bin");
    if kits_root.is_dir() {
        let mut versions: Vec<PathBuf> = std::fs::read_dir(kits_root)
            .ok()?
            .filter_map(|e| e.ok().map(|e| e.path()))
            .filter(|p| p.is_dir())
            .collect();
        versions.sort();
        versions.reverse();
        let host_arch = if cfg!(target_arch = "aarch64") {
            "arm64"
        } else {
            "x64"
        };
        for ver in &versions {
            for arch in [host_arch, "x64", "x86"] {
                let candidate = ver.join(arch).join("rc.exe");
                if candidate.is_file() {
                    return Some(candidate);
                }
            }
        }
    }

    // 兜底尝试 PATH 中的 rc.exe
    Some(PathBuf::from("rc.exe"))
}

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=viewer.manifest");
    println!("cargo:rerun-if-changed=viewer.rc");
    println!("cargo:rerun-if-changed=assets/picasa_viewer.ico");
    println!("cargo:rerun-if-changed=assets/picasa_photo_doc.ico");

    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os != "windows" {
        return;
    }

    let manifest_dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let out_dir = PathBuf::from(std::env::var("OUT_DIR").unwrap());

    // 1. 嵌入 PerMonitorV2 高分屏 DPI 清单
    let manifest_path = manifest_dir.join("viewer.manifest");
    if manifest_path.exists() {
        println!("cargo:rustc-link-arg=/MANIFEST:EMBED");
        println!(
            "cargo:rustc-link-arg=/MANIFESTINPUT:{}",
            manifest_path.display()
        );
    }

    // 2. 编译并嵌入多尺寸 ICO 图标（16/24/32/48/64/128/256px）与版权版本信息
    let rc_path = manifest_dir.join("viewer.rc");
    let ico_path = manifest_dir.join("assets").join("picasa_viewer.ico");
    if rc_path.exists() && ico_path.exists() {
        if let Some(rc_exe) = find_windows_rc_exe() {
            let res_path = out_dir.join("viewer.res");
            let status = Command::new(&rc_exe)
                .current_dir(&manifest_dir)
                .arg("/nologo")
                .arg("/c65001")
                .arg("/fo")
                .arg(&res_path)
                .arg(&rc_path)
                .status();
            match status {
                Ok(s) if s.success() && res_path.exists() => {
                    println!("cargo:rustc-link-arg={}", res_path.display());
                }
                Ok(s) => {
                    println!(
                        "cargo:warning=rc.exe 编译资源返回非零状态 ({}), 将跳过图标嵌入",
                        s
                    );
                }
                Err(e) => {
                    println!("cargo:warning=无法启动 rc.exe ({e}), 将跳过图标嵌入");
                }
            }
        }
    }
}
