// ─────────────────────────────────────────────────────────────────────────────
// config.rs — 读取 Tools3000 单一事实源配置 (config.json)
//
// 优先级与主程序 WinUtils::getAppDataDirectory() + ConfigManager 严格一致:
//   1. TOOLS3000_CONFIG_PATH（显式配置文件覆写）或 TOOLS3000_DATA_ROOT\config\config.json；
//   2. 便携模式同目录 .tools3000\config\config.json / data\config\config.json / portable_data\config\config.json（兼容同目录 config.json）；
//   3. 标准安装模式 %LOCALAPPDATA%\Tools3000\config\config.json。
// ─────────────────────────────────────────────────────────────────────────────

use crate::json::{parse_json, JsonValue};
use crate::policy::{split_config_list, SecurityPolicy};
use std::env;
use std::fs;
use std::path::PathBuf;

#[derive(Debug, Clone, Default)]
pub struct LoadedCliConfig {
    pub config_path: Option<PathBuf>,
    pub policy: SecurityPolicy,
    pub global_excludes: Vec<String>,
    pub exclude_hidden: bool,
    pub exclude_system: bool,
    pub pinyin_enabled: Option<bool>,
    pub pipe_token: String,
    pub enabled_drives: Vec<String>,
}

pub fn resolve_config_path_with_roots(
    override_cfg: Option<&str>,
    override_data_root: Option<&str>,
    exe_dir: Option<&std::path::Path>,
    local_app_data: Option<&str>,
) -> Option<PathBuf> {
    if let Some(raw) = override_cfg.map(str::trim).filter(|s| !s.is_empty()) {
        let p = PathBuf::from(raw);
        if p.exists() {
            return Some(p);
        }
    }

    if let Some(root) = override_data_root.map(str::trim).filter(|s| !s.is_empty()) {
        let p = PathBuf::from(root).join("config").join("config.json");
        if p.exists() {
            return Some(p);
        }
    }

    if let Some(dir) = exe_dir {
        for portable_root in [".tools3000", "data", "portable_data"] {
            let root_dir = dir.join(portable_root);
            if root_dir.is_dir() {
                let cfg_path = root_dir.join("config").join("config.json");
                if cfg_path.exists() {
                    return Some(cfg_path);
                }
            }
        }
        let legacy_portable_cfg = dir.join("config.json");
        if legacy_portable_cfg.exists() {
            return Some(legacy_portable_cfg);
        }
    }

    if let Some(lad) = local_app_data.map(str::trim).filter(|s| !s.is_empty()) {
        let standard_cfg = PathBuf::from(lad)
            .join("Tools3000")
            .join("config")
            .join("config.json");
        if standard_cfg.exists() {
            return Some(standard_cfg);
        }
        let legacy_cfg = PathBuf::from(lad).join("Tools3000").join("config.json");
        if legacy_cfg.exists() {
            return Some(legacy_cfg);
        }
        return Some(standard_cfg);
    }

    None
}

pub fn resolve_config_path() -> Option<PathBuf> {
    let override_cfg = env::var("TOOLS3000_CONFIG_PATH").ok();
    let override_root = env::var("TOOLS3000_DATA_ROOT").ok();
    let exe_path = env::current_exe().ok();
    let exe_dir = exe_path.as_deref().and_then(|p| p.parent());
    let local_app_data = env::var("LOCALAPPDATA").ok();
    resolve_config_path_with_roots(
        override_cfg.as_deref(),
        override_root.as_deref(),
        exe_dir,
        local_app_data.as_deref(),
    )
}

pub fn load_cli_config() -> LoadedCliConfig {
    let mut cfg = LoadedCliConfig {
        exclude_hidden: true,
        exclude_system: true,
        ..LoadedCliConfig::default()
    };

    let path = match resolve_config_path() {
        Some(p) => p,
        None => return cfg,
    };
    cfg.config_path = Some(path.clone());

    let raw = match fs::read_to_string(&path) {
        Ok(text) => text,
        Err(_) => return cfg,
    };
    let root = match parse_json(&raw) {
        Ok(val) => val,
        Err(_) => return cfg,
    };

    apply_config_json(&root, &mut cfg);
    cfg
}

pub fn apply_config_json(root: &JsonValue, cfg: &mut LoadedCliConfig) {
    let search = match root.get("search") {
        Some(v) => v,
        None => return,
    };

    // 默认安全关闭 (false)，必须由用户在设置页显式开启
    cfg.policy.external_api_enabled = search
        .get("externalApiEnabled")
        .and_then(|v| v.as_bool())
        .unwrap_or(false);

    if let Some(ex_str) = search.get("externalApiExcludePaths").and_then(|v| v.as_str()) {
        cfg.policy.extra_exclude_patterns = split_config_list(ex_str);
    } else if let Some(ex_arr) = search.get("externalApiExcludePaths").and_then(|v| v.as_array()) {
        cfg.policy.extra_exclude_patterns = ex_arr
            .iter()
            .filter_map(|v| v.as_str().map(|s| s.trim().to_string()))
            .filter(|s| !s.is_empty())
            .collect();
    }

    if let Some(roots_str) = search.get("externalApiAllowedRoots").and_then(|v| v.as_str()) {
        cfg.policy.allowed_roots = split_config_list(roots_str);
    } else if let Some(roots_arr) = search.get("externalApiAllowedRoots").and_then(|v| v.as_array()) {
        cfg.policy.allowed_roots = roots_arr
            .iter()
            .filter_map(|v| v.as_str().map(|s| s.trim().to_string()))
            .filter(|s| !s.is_empty())
            .collect();
    }

    if let Some(global_ex) = search.get("excludePatterns").and_then(|v| v.as_str()) {
        cfg.global_excludes = split_config_list(global_ex);
    }
    if let Some(hidden) = search.get("excludeHidden").and_then(|v| v.as_bool()) {
        cfg.exclude_hidden = hidden;
    }
    if let Some(sys) = search.get("excludeSystem").and_then(|v| v.as_bool()) {
        cfg.exclude_system = sys;
    }
    cfg.pinyin_enabled = search.get("pinyinEnabled").and_then(|v| v.as_bool());
    if let Some(tok) = search.get("pipeToken").and_then(|v| v.as_str()) {
        cfg.pipe_token = tok.trim().to_string();
    }
    if let Some(drives_str) = search.get("enabledDrives").and_then(|v| v.as_str()) {
        cfg.enabled_drives = drives_str
            .split(|c| c == ',' || c == ';' || c == ' ')
            .filter_map(|s| {
                let first = s.trim().chars().next()?;
                if first.is_ascii_alphabetic() {
                    Some(first.to_ascii_uppercase().to_string())
                } else {
                    None
                }
            })
            .collect();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_external_api_to_disabled_and_parses_overrides() {
        let empty = parse_json("{}").unwrap();
        let mut cfg = LoadedCliConfig::default();
        apply_config_json(&empty, &mut cfg);
        assert!(!cfg.policy.external_api_enabled);

        let enabled = parse_json(
            r#"{
                "search": {
                    "externalApiEnabled": true,
                    "externalApiExcludePaths": "D:\\Secrets\nnode_modules",
                    "externalApiAllowedRoots": "C:\\repo,D:\\Docs",
                    "enabledDrives": "C, D",
                    "pipeToken": "0123456789abcdef0123456789abcdef"
                }
            }"#,
        )
        .unwrap();
        apply_config_json(&enabled, &mut cfg);
        assert!(cfg.policy.external_api_enabled);
        assert_eq!(cfg.policy.extra_exclude_patterns, vec!["D:\\Secrets", "node_modules"]);
        assert_eq!(cfg.policy.allowed_roots, vec!["C:\\repo", "D:\\Docs"]);
        assert_eq!(cfg.enabled_drives, vec!["C", "D"]);
        assert_eq!(cfg.pipe_token.len(), 32);
    }

    #[test]
    fn resolves_standard_and_portable_config_under_config_subdir() {
        let base = env::temp_dir().join(format!("t3k_cli_cfg_test_{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        let lad = base.join("LocalAppData");
        let std_cfg = lad.join("Tools3000").join("config").join("config.json");
        fs::create_dir_all(std_cfg.parent().unwrap()).unwrap();
        fs::write(&std_cfg, r#"{"search":{"externalApiEnabled":true}}"#).unwrap();

        let resolved = resolve_config_path_with_roots(
            None,
            None,
            Some(&base.join("ProgramFiles")),
            Some(lad.to_str().unwrap()),
        )
        .unwrap();
        assert_eq!(resolved, std_cfg);

        // 便携模式 data\config\config.json 优先于标准安装模式
        let exe_dir = base.join("PortableApp");
        let port_cfg = exe_dir.join("data").join("config").join("config.json");
        fs::create_dir_all(port_cfg.parent().unwrap()).unwrap();
        fs::write(&port_cfg, r#"{"search":{"externalApiEnabled":true}}"#).unwrap();
        let resolved_port = resolve_config_path_with_roots(
            None,
            None,
            Some(&exe_dir),
            Some(lad.to_str().unwrap()),
        )
        .unwrap();
        assert_eq!(resolved_port, port_cfg);

        let _ = fs::remove_dir_all(&base);
    }
}
