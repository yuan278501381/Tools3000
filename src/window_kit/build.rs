// ─────────────────────────────────────────────────────────────────────────────
// build.rs — 嵌入 PerMonitorV2 高分屏清单、Tools3000 共享图标与 Windows 版本资源
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
    println!("cargo:rerun-if-changed=window_kit.manifest");
    println!("cargo:rerun-if-changed=window_kit.rc");
    println!("cargo:rerun-if-changed=../../resources/app.ico");
    println!("cargo:rerun-if-changed=ui/window_kit.slint");
    println!("cargo:rerun-if-changed=ui/tokens.slint");
    println!("cargo:rerun-if-changed=ui/icons.slint");
    println!("cargo:rerun-if-changed=ui/quick_switch.slint");
    println!("cargo:rerun-if-changed=ui/layout_hub.slint");
    println!("cargo:rerun-if-changed=ui/workspace_capture.slint");

    // 0. 编译 Slint 声明式 UI（统一 Fluent 风格，编译期生成零开销 Rust 状态机）
    let slint_cfg = slint_build::CompilerConfiguration::new().with_style("fluent".into());
    slint_build::compile_with_config("ui/window_kit.slint", slint_cfg)
        .expect("编译 Slint UI (ui/window_kit.slint) 失败");

    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os != "windows" {
        return;
    }

    let manifest_dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let out_dir = PathBuf::from(std::env::var("OUT_DIR").unwrap());

    // 1. 嵌入 PerMonitorV2 高分屏 DPI 清单
    let manifest_path = manifest_dir.join("window_kit.manifest");
    if manifest_path.exists() {
        println!("cargo:rustc-link-arg=/MANIFEST:EMBED");
        println!(
            "cargo:rustc-link-arg=/MANIFESTINPUT:{}",
            manifest_path.display()
        );
    }

    // 2. 编译并嵌入应用图标（复用 ../../resources/app.ico）与版权版本信息
    let rc_path = manifest_dir.join("window_kit.rc");
    let ico_path = manifest_dir.join("..").join("..").join("resources").join("app.ico");
    assert!(
        rc_path.is_file(),
        "缺少 Windows 资源脚本 window_kit.rc: {}",
        rc_path.display()
    );
    assert!(
        ico_path.is_file(),
        "缺少共享主图标 ../../resources/app.ico: {}",
        ico_path.display()
    );

    let rc_exe = find_windows_rc_exe()
        .expect("未找到 Windows SDK 资源编译器 rc.exe，已阻断构建以防产出无图标 EXE");
    let res_path = out_dir.join("window_kit.res");
    let status = Command::new(&rc_exe)
        .current_dir(&manifest_dir)
        .arg("/nologo")
        .arg("/c65001")
        .arg("/fo")
        .arg(&res_path)
        .arg(&rc_path)
        .status()
        .unwrap_or_else(|e| {
            panic!(
                "无法启动资源编译器 rc.exe ({})，已阻断构建以防产出无图标 EXE: {e}",
                rc_exe.display()
            )
        });
    if !status.success() || !res_path.is_file() {
        panic!(
            "rc.exe ({}) 编译 window_kit.rc 失败 (状态: {})，已阻断构建以防产出无图标 EXE",
            rc_exe.display(),
            status
        );
    }
    println!("cargo:rustc-link-arg={}", res_path.display());
}
