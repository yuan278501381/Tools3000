// ─────────────────────────────────────────────────────────────────────────────
// cli.rs — Tools3000 极客命令行界面解析器与结构化输出渲染器
//
// 规范:
//   - 100% 严禁 Unicode Emoji，状态一律使用 [OK] / [INFO] / [WARN] / [ERROR]；
//   - 支持 --json 严格机器可读输出与默认整洁等宽对齐人类输出；
//   - 子命令: search | content | status | mcp | --version | --help。
// ─────────────────────────────────────────────────────────────────────────────

use crate::config::load_cli_config;
use crate::constants::{
    CLI_NAME, CLI_VERSION, DEFAULT_CONTENT_LIMIT, DEFAULT_FILE_LIMIT, DEFAULT_MAX_SNIPPET_CHARS,
    EXIT_API_DISABLED, EXIT_OK, EXIT_SERVICE_ERROR, EXIT_USAGE,
};
use crate::json::JsonValue;
use crate::mcp::run_mcp_stdio;
use crate::pipe_client::WinPipeBackend;
use crate::service_api::{
    execute_content_search, execute_file_search, execute_index_status, ApiError, ClientSource,
    ContentSearchParams, FileSearchParams,
};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Command {
    Help,
    Version,
    Mcp,
    Status {
        start_if_stopped: bool,
        json: bool,
    },
    Search {
        query: String,
        scope: Option<String>,
        extensions: Vec<String>,
        limit: usize,
        both: bool,
        case_sensitive: bool,
        match_path: bool,
        pinyin: Option<bool>,
        json: bool,
    },
    Content {
        query: String,
        scope: Option<String>,
        extensions: Vec<String>,
        limit: usize,
        max_snippet_chars: usize,
        case_sensitive: bool,
        json: bool,
    },
}

pub fn parse_args(args: &[String]) -> Result<Command, String> {
    if args.is_empty() {
        return Ok(Command::Help);
    }

    let sub = args[0].as_str();
    match sub {
        "-h" | "--help" | "help" => Ok(Command::Help),
        "-V" | "--version" | "version" => Ok(Command::Version),
        "mcp" => {
            if args.len() > 1 && matches!(args[1].as_str(), "-h" | "--help") {
                return Ok(Command::Help);
            }
            Ok(Command::Mcp)
        }
        "status" => {
            let mut start_if_stopped = false;
            let mut json = false;
            let mut i = 1;
            while i < args.len() {
                match args[i].as_str() {
                    "--start" | "--wake" => start_if_stopped = true,
                    "--no-start" => start_if_stopped = false,
                    "--json" => json = true,
                    "-h" | "--help" => return Ok(Command::Help),
                    other => return Err(format!("unknown option for status: {}", other)),
                }
                i += 1;
            }
            Ok(Command::Status {
                start_if_stopped,
                json,
            })
        }
        "search" => {
            let mut query_parts: Vec<String> = Vec::new();
            let mut scope: Option<String> = None;
            let mut extensions: Vec<String> = Vec::new();
            let mut limit = DEFAULT_FILE_LIMIT;
            let mut both = false;
            let mut case_sensitive = false;
            let mut match_path = false;
            let mut pinyin: Option<bool> = None;
            let mut json = false;

            let mut i = 1;
            while i < args.len() {
                match args[i].as_str() {
                    "--scope" | "-s" => {
                        i += 1;
                        let val = args.get(i).ok_or("missing value for --scope")?;
                        scope = Some(val.clone());
                    }
                    "--ext" | "-e" => {
                        i += 1;
                        let val = args.get(i).ok_or("missing value for --ext")?;
                        extensions.push(val.clone());
                    }
                    "--limit" | "-n" => {
                        i += 1;
                        let val = args.get(i).ok_or("missing value for --limit")?;
                        limit = val
                            .parse::<usize>()
                            .map_err(|_| format!("invalid --limit integer: {}", val))?;
                    }
                    "--both" => both = true,
                    "--case-sensitive" | "-c" => case_sensitive = true,
                    "--match-path" | "-p" => match_path = true,
                    "--pinyin" => pinyin = Some(true),
                    "--no-pinyin" => pinyin = Some(false),
                    "--json" => json = true,
                    "-h" | "--help" => return Ok(Command::Help),
                    flag if flag.starts_with('-') => {
                        return Err(format!("unknown option for search: {}", flag));
                    }
                    positional => query_parts.push(positional.to_string()),
                }
                i += 1;
            }

            Ok(Command::Search {
                query: query_parts.join(" "),
                scope,
                extensions,
                limit,
                both,
                case_sensitive,
                match_path,
                pinyin,
                json,
            })
        }
        "content" => {
            let mut query_parts: Vec<String> = Vec::new();
            let mut scope: Option<String> = None;
            let mut extensions: Vec<String> = Vec::new();
            let mut limit = DEFAULT_CONTENT_LIMIT;
            let mut max_snippet_chars = DEFAULT_MAX_SNIPPET_CHARS;
            let mut case_sensitive = false;
            let mut json = false;

            let mut i = 1;
            while i < args.len() {
                match args[i].as_str() {
                    "--scope" | "-s" => {
                        i += 1;
                        let val = args.get(i).ok_or("missing value for --scope")?;
                        scope = Some(val.clone());
                    }
                    "--ext" | "-e" => {
                        i += 1;
                        let val = args.get(i).ok_or("missing value for --ext")?;
                        extensions.push(val.clone());
                    }
                    "--limit" | "-n" => {
                        i += 1;
                        let val = args.get(i).ok_or("missing value for --limit")?;
                        limit = val
                            .parse::<usize>()
                            .map_err(|_| format!("invalid --limit integer: {}", val))?;
                    }
                    "--max-snippet-chars" => {
                        i += 1;
                        let val = args.get(i).ok_or("missing value for --max-snippet-chars")?;
                        max_snippet_chars = val
                            .parse::<usize>()
                            .map_err(|_| format!("invalid --max-snippet-chars integer: {}", val))?;
                    }
                    "--case-sensitive" | "-c" => case_sensitive = true,
                    "--json" => json = true,
                    "-h" | "--help" => return Ok(Command::Help),
                    flag if flag.starts_with('-') => {
                        return Err(format!("unknown option for content: {}", flag));
                    }
                    positional => query_parts.push(positional.to_string()),
                }
                i += 1;
            }

            Ok(Command::Content {
                query: query_parts.join(" "),
                scope,
                extensions,
                limit,
                max_snippet_chars,
                case_sensitive,
                json,
            })
        }
        other => Err(format!("unknown subcommand '{}' (run '{} --help')", other, CLI_NAME)),
    }
}

pub fn print_help() {
    println!("{} {} - Tools3000 NTFS File & Content Search CLI / MCP Server", CLI_NAME, CLI_VERSION);
    println!();
    println!("USAGE:");
    println!("  {} search <query> [OPTIONS]", CLI_NAME);
    println!("  {} content <query> [OPTIONS]", CLI_NAME);
    println!("  {} status [--start | --no-start] [--json]", CLI_NAME);
    println!("  {} mcp", CLI_NAME);
    println!();
    println!("OPTIONS (search / content):");
    println!("  -s, --scope <DIR>            Restrict search under directory prefix");
    println!("  -e, --ext <EXTS>             Filter by extensions (e.g. pdf,docx,rs)");
    println!("  -n, --limit <N>              Max results (search: 1..200, content: 1..100)");
    println!("      --max-snippet-chars <N>  Max UTF-8 chars per content snippet (40..1200)");
    println!("      --both                   Include content matches in `search`");
    println!("  -c, --case-sensitive         Case-sensitive matching");
    println!("  -p, --match-path             Match against full file path");
    println!("      --pinyin / --no-pinyin   Override Chinese pinyin matching");
    println!("      --json                   Emit compact machine-readable JSON");
    println!();
    println!("SECURITY:");
    println!("  Disabled by default. Enable 'External CLI & MCP Search API' in Tools3000 Settings > Search.");
    println!("  Sensitive credential directories (.ssh, .env, browser secrets, private keys) are always blocked.");
}

pub fn run_cli(args: &[String]) -> i32 {
    let cmd = match parse_args(args) {
        Ok(c) => c,
        Err(msg) => {
            eprintln!("[ERROR] {}", msg);
            return EXIT_USAGE;
        }
    };

    match cmd {
        Command::Help => {
            print_help();
            EXIT_OK
        }
        Command::Version => {
            println!("{} {}", CLI_NAME, CLI_VERSION);
            EXIT_OK
        }
        Command::Mcp => {
            let cfg = load_cli_config();
            let backend = WinPipeBackend::from_config(&cfg);
            if let Err(err) = run_mcp_stdio(&backend) {
                eprintln!("[ERROR] MCP stdio server exited with I/O error: {}", err);
                return EXIT_SERVICE_ERROR;
            }
            EXIT_OK
        }
        Command::Status {
            start_if_stopped,
            json,
        } => {
            let cfg = load_cli_config();
            let backend = WinPipeBackend::from_config(&cfg);
            match execute_index_status(&cfg, &backend, ClientSource::Cli, start_if_stopped) {
                Ok(res) => {
                    if json {
                        println!("{}", res.to_json_string());
                    } else {
                        render_status_human(&res);
                    }
                    EXIT_OK
                }
                Err(err) => emit_api_error(&err, json),
            }
        }
        Command::Search {
            query,
            scope,
            extensions,
            limit,
            both,
            case_sensitive,
            match_path,
            pinyin,
            json,
        } => {
            let cfg = load_cli_config();
            let backend = WinPipeBackend::from_config(&cfg);
            let params = FileSearchParams {
                query,
                scope,
                extensions,
                limit,
                include_content_matches: both,
                case_sensitive,
                match_path,
                pinyin,
            };
            match execute_file_search(&cfg, &backend, ClientSource::Cli, &params) {
                Ok(res) => {
                    if json {
                        println!("{}", res.to_json_string());
                    } else {
                        render_results_human(&res, false);
                    }
                    EXIT_OK
                }
                Err(err) => emit_api_error(&err, json),
            }
        }
        Command::Content {
            query,
            scope,
            extensions,
            limit,
            max_snippet_chars,
            case_sensitive,
            json,
        } => {
            let cfg = load_cli_config();
            let backend = WinPipeBackend::from_config(&cfg);
            let params = ContentSearchParams {
                query,
                scope,
                extensions,
                limit,
                max_snippet_chars,
                case_sensitive,
            };
            match execute_content_search(&cfg, &backend, ClientSource::Cli, &params) {
                Ok(res) => {
                    if json {
                        println!("{}", res.to_json_string());
                    } else {
                        render_results_human(&res, true);
                    }
                    EXIT_OK
                }
                Err(err) => emit_api_error(&err, json),
            }
        }
    }
}

fn emit_api_error(err: &ApiError, json: bool) -> i32 {
    if json {
        let mut obj = JsonValue::object();
        obj.insert("success", JsonValue::Bool(false));
        obj.insert("errorCode", JsonValue::String(err.code().to_string()));
        obj.insert("error", JsonValue::String(err.message().to_string()));
        println!("{}", obj.to_json_string());
    } else {
        eprintln!("[ERROR] {}: {}", err.code(), err.message());
    }
    match err {
        ApiError::ExternalApiDisabled(_) => EXIT_API_DISABLED,
        ApiError::InvalidInput(_) => EXIT_USAGE,
        ApiError::ServiceUnavailable(_) => EXIT_SERVICE_ERROR,
    }
}

fn render_status_human(res: &JsonValue) {
    let running = res.get("running").and_then(|v| v.as_bool()).unwrap_or(false);
    if !running {
        println!("[INFO] Tools3000_SearchService: STOPPED (DEMAND_START)");
        if let Some(msg) = res.get("message").and_then(|v| v.as_str()) {
            println!("[INFO] {}", msg);
        }
        return;
    }
    let total = res.get("totalRecords").and_then(|v| v.as_i64()).unwrap_or(0);
    let vols = res.get("indexedVolumes").and_then(|v| v.as_i64()).unwrap_or(0);
    let total_vols = res.get("totalVolumes").and_then(|v| v.as_i64()).unwrap_or(0);
    let phase = res.get("phase").and_then(|v| v.as_str()).unwrap_or("idle");
    let pct = res.get("progressPercent").and_then(|v| v.as_i64()).unwrap_or(100);
    println!(
        "[OK] Tools3000_SearchService: RUNNING | phase={} ({}%) | volumes={}/{} | records={}",
        phase, pct, vols, total_vols, total
    );
}

fn render_results_human(res: &JsonValue, show_snippets: bool) {
    let count = res.get("count").and_then(|v| v.as_i64()).unwrap_or(0);
    let redacted = res.get("redactedCount").and_then(|v| v.as_i64()).unwrap_or(0);
    let elapsed = res.get("elapsedMs").and_then(|v| v.as_i64()).unwrap_or(0);
    let total = res.get("totalIndexedFiles").and_then(|v| v.as_i64()).unwrap_or(0);
    println!(
        "[OK] {} matches ({} ms, indexed={} files{})",
        count,
        elapsed,
        total,
        if redacted > 0 {
            format!(", redacted={} sensitive", redacted)
        } else {
            String::new()
        }
    );

    if let Some(items) = res.get("results").and_then(|v| v.as_array()) {
        for item in items {
            let path = item.get("path").and_then(|v| v.as_str()).unwrap_or("");
            let is_dir = item.get("isDirectory").and_then(|v| v.as_bool()).unwrap_or(false);
            let size = item.get("size").and_then(|v| v.as_i64()).unwrap_or(0);
            let tag = if is_dir { "DIR " } else { "FILE" };
            println!("  [{}] {:>10}  {}", tag, format_size_compact(size, is_dir), path);

            if show_snippets || item.get("snippets").is_some() {
                if let Some(snips) = item.get("snippets").and_then(|v| v.as_array()) {
                    for snip in snips {
                        let line_no = snip.get("lineNumber").and_then(|v| v.as_i64()).unwrap_or(0);
                        let line_text = snip.get("lineContent").and_then(|v| v.as_str()).unwrap_or("");
                        println!("         L{}: {}", line_no, line_text);
                    }
                }
            }
        }
    }
}

fn format_size_compact(bytes: i64, is_dir: bool) -> String {
    if is_dir || bytes < 0 {
        return "-".to_string();
    }
    let b = bytes as f64;
    if b < 1024.0 {
        format!("{} B", bytes)
    } else if b < 1024.0 * 1024.0 {
        format!("{:.1} KB", b / 1024.0)
    } else if b < 1024.0 * 1024.0 * 1024.0 {
        format!("{:.1} MB", b / (1024.0 * 1024.0))
    } else {
        format!("{:.2} GB", b / (1024.0 * 1024.0 * 1024.0))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn vec_s(items: &[&str]) -> Vec<String> {
        items.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn parses_search_content_status_and_mcp_subcommands() {
        assert_eq!(parse_args(&vec_s(&["mcp"])).unwrap(), Command::Mcp);
        assert_eq!(
            parse_args(&vec_s(&["status", "--json"])).unwrap(),
            Command::Status {
                start_if_stopped: false,
                json: true,
            }
        );
        assert_eq!(
            parse_args(&vec_s(&[
                "search", "hello", "world", "--scope", r"C:\repo", "--ext", "rs,md", "-n", "15", "--json"
            ]))
            .unwrap(),
            Command::Search {
                query: "hello world".to_string(),
                scope: Some(r"C:\repo".to_string()),
                extensions: vec!["rs,md".to_string()],
                limit: 15,
                both: false,
                case_sensitive: false,
                match_path: false,
                pinyin: None,
                json: true,
            }
        );
        assert_eq!(
            parse_args(&vec_s(&[
                "content", "TranslationService", "--max-snippet-chars", "180", "--case-sensitive"
            ]))
            .unwrap(),
            Command::Content {
                query: "TranslationService".to_string(),
                scope: None,
                extensions: vec![],
                limit: DEFAULT_CONTENT_LIMIT,
                max_snippet_chars: 180,
                case_sensitive: true,
                json: false,
            }
        );
    }
}
