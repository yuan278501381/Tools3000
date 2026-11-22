// ─────────────────────────────────────────────────────────────────────────────
// mcp.rs — Model Context Protocol (MCP) stdio JSON-RPC 2.0 只读搜索服务器
//
// 核心特性:
//   1. 双传输帧自动探测：同时兼容 NDJSON（每行一帧）与 Content-Length 帧头，
//      无缝对接 Cursor / Claude Desktop / Claude Code / Antigravity / Windsurf；
//   2. 严格 DEMAND_START：initialize / notifications/initialized / ping / tools/list
//      100% 本地静态响应，绝不启动或探测 Tools3000_SearchService；
//   3. 仅暴露 3 个严格只读工具 (search_files, search_content, get_index_status)，
//      强制校验外部接口开关、内置敏感凭证黑名单与 Snippet Token 预算裁剪。
// ─────────────────────────────────────────────────────────────────────────────

use crate::config::{load_cli_config, LoadedCliConfig};
use crate::constants::{
    CLI_VERSION, DEFAULT_CONTENT_LIMIT, DEFAULT_FILE_LIMIT, DEFAULT_MAX_SNIPPET_CHARS,
    MCP_PROTOCOL_VERSION, MCP_SERVER_NAME,
};
use crate::json::{parse_json, JsonValue};
use crate::pipe_client::SearchBackend;
use crate::service_api::{
    execute_content_search, execute_file_search, execute_index_status, ClientSource,
    ContentSearchParams, FileSearchParams,
};
use std::io::{self, BufRead, BufReader, Write};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FramingMode {
    Ndjson,
    ContentLength,
}

pub fn run_mcp_stdio(backend: &dyn SearchBackend) -> io::Result<()> {
    let stdin = io::stdin();
    let stdout = io::stdout();
    let mut reader = BufReader::new(stdin.lock());
    let mut writer = stdout.lock();

    // 每次收到 tools/call 动态重载 config.json，用户在设置页实时开关外部接口立即生效，无需重启 Agent！
    run_mcp_loop(&mut reader, &mut writer, backend, load_cli_config)
}

pub fn run_mcp_loop<R, W, F>(
    reader: &mut R,
    writer: &mut W,
    backend: &dyn SearchBackend,
    mut config_loader: F,
) -> io::Result<()>
where
    R: BufRead,
    W: Write,
    F: FnMut() -> LoadedCliConfig,
{
    while let Some((raw_msg, framing)) = read_next_rpc_message(reader)? {
        let response = match parse_json(&raw_msg) {
            Ok(msg) => handle_rpc_message(&msg, backend, &mut config_loader),
            Err(err) => Some(make_jsonrpc_error(
                JsonValue::Null,
                -32700,
                &format!("Parse error: {}", err),
            )),
        };

        if let Some(resp) = response {
            write_rpc_message(writer, &resp, framing)?;
        }
    }
    Ok(())
}

fn read_next_rpc_message<R: BufRead>(reader: &mut R) -> io::Result<Option<(String, FramingMode)>> {
    let mut first_line = String::new();
    loop {
        first_line.clear();
        let n = reader.read_line(&mut first_line)?;
        if n == 0 {
            return Ok(None);
        }
        let trimmed = first_line.trim();
        if trimmed.is_empty() {
            continue;
        }

        // LSP / MCP Content-Length 帧头自动识别
        if trimmed.to_ascii_lowercase().starts_with("content-length:") {
            let len_str = trimmed["content-length:".len()..].trim();
            let content_len: usize = len_str.parse().map_err(|_| {
                io::Error::new(io::ErrorKind::InvalidData, "invalid Content-Length header")
            })?;
            if content_len > 8 * 1024 * 1024 {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "Content-Length exceeds 8 MiB limit",
                ));
            }
            // 读取剩余 Header 直至空行
            let mut hdr = String::new();
            loop {
                hdr.clear();
                let hn = reader.read_line(&mut hdr)?;
                if hn == 0 || hdr.trim().is_empty() {
                    break;
                }
            }
            let mut body = vec![0u8; content_len];
            reader.read_exact(&mut body)?;
            let text = String::from_utf8(body)
                .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "non-UTF-8 MCP payload"))?;
            return Ok(Some((text, FramingMode::ContentLength)));
        }

        return Ok(Some((trimmed.to_string(), FramingMode::Ndjson)));
    }
}

fn write_rpc_message<W: Write>(
    writer: &mut W,
    msg: &JsonValue,
    framing: FramingMode,
) -> io::Result<()> {
    let body = msg.to_json_string();
    match framing {
        FramingMode::Ndjson => {
            writer.write_all(body.as_bytes())?;
            writer.write_all(b"\n")?;
        }
        FramingMode::ContentLength => {
            let header = format!("Content-Length: {}\r\n\r\n", body.len());
            writer.write_all(header.as_bytes())?;
            writer.write_all(body.as_bytes())?;
        }
    }
    writer.flush()
}

pub fn handle_rpc_message<F>(
    msg: &JsonValue,
    backend: &dyn SearchBackend,
    config_loader: &mut F,
) -> Option<JsonValue>
where
    F: FnMut() -> LoadedCliConfig,
{
    let method = msg.get("method").and_then(|v| v.as_str()).unwrap_or("");
    let id_opt = msg.get("id").cloned();

    // JSON-RPC 通知 (无 id) 不发送响应
    if id_opt.is_none() {
        return None;
    }
    let id = id_opt.unwrap();

    match method {
        // 以下 3 个控制方法 100% 静态响应，绝对不唤醒 Tools3000_SearchService
        "initialize" => Some(make_jsonrpc_result(id, build_initialize_result(msg))),
        "ping" => Some(make_jsonrpc_result(id, JsonValue::object())),
        "tools/list" => Some(make_jsonrpc_result(id, build_tools_list_result())),
        "tools/call" => {
            let params = msg.get("params");
            let tool_name = params
                .and_then(|p| p.get("name"))
                .and_then(|v| v.as_str())
                .unwrap_or("");
            let args = params
                .and_then(|p| p.get("arguments"))
                .cloned()
                .unwrap_or_else(JsonValue::object);
            let cfg = config_loader();
            let tool_result = dispatch_tool_call(&cfg, backend, tool_name, &args);
            Some(make_jsonrpc_result(id, tool_result))
        }
        _ => Some(make_jsonrpc_error(
            id,
            -32601,
            &format!("Method not found: {}", method),
        )),
    }
}

fn build_initialize_result(req: &JsonValue) -> JsonValue {
    let client_proto = req
        .get("params")
        .and_then(|p| p.get("protocolVersion"))
        .and_then(|v| v.as_str())
        .unwrap_or(MCP_PROTOCOL_VERSION);

    let negotiated = match client_proto {
        "2024-11-05" | "2025-03-26" | "2025-06-18" => client_proto,
        _ => MCP_PROTOCOL_VERSION,
    };

    let mut tools_cap = JsonValue::object();
    tools_cap.insert("listChanged", JsonValue::Bool(false));

    let mut caps = JsonValue::object();
    caps.insert("tools", tools_cap);

    let mut info = JsonValue::object();
    info.insert("name", JsonValue::String(MCP_SERVER_NAME.to_string()));
    info.insert("version", JsonValue::String(CLI_VERSION.to_string()));

    let mut res = JsonValue::object();
    res.insert("protocolVersion", JsonValue::String(negotiated.to_string()));
    res.insert("capabilities", caps);
    res.insert("serverInfo", info);
    res.insert(
        "instructions",
        JsonValue::String(
            "Tools3000 read-only local NTFS file and document content search server. Sensitive credential paths (.ssh, .env, browser profiles, private keys) are strictly excluded."
                .to_string(),
        ),
    );
    res
}

fn build_tools_list_result() -> JsonValue {
    let tools = vec![
        build_tool_def(
            "search_files",
            "Search local Windows files and directories instantaneously via Tools3000 NTFS MFT index. Supports filename, path, Chinese pinyin, extension, and size filters.",
            build_search_files_schema(),
        ),
        build_tool_def(
            "search_content",
            "Search inside local text, source code, and Office/PDF/OFD documents with bounded UTF-8 snippets.",
            build_search_content_schema(),
        ),
        build_tool_def(
            "get_index_status",
            "Query Tools3000 NTFS search service status, indexed volumes, and record count. By default does not wake an idle DEMAND_START service.",
            build_index_status_schema(),
        ),
    ];

    let mut out = JsonValue::object();
    out.insert("tools", JsonValue::Array(tools));
    out
}

fn build_tool_def(name: &str, description: &str, input_schema: JsonValue) -> JsonValue {
    let mut annotations = JsonValue::object();
    annotations.insert("readOnlyHint", JsonValue::Bool(true));
    annotations.insert("destructiveHint", JsonValue::Bool(false));
    annotations.insert("idempotentHint", JsonValue::Bool(true));
    annotations.insert("openWorldHint", JsonValue::Bool(false));

    let mut tool = JsonValue::object();
    tool.insert("name", JsonValue::String(name.to_string()));
    tool.insert("description", JsonValue::String(description.to_string()));
    tool.insert("inputSchema", input_schema);
    tool.insert("annotations", annotations);
    tool
}

fn schema_prop(prop_type: &str, description: &str) -> JsonValue {
    let mut o = JsonValue::object();
    o.insert("type", JsonValue::String(prop_type.to_string()));
    o.insert("description", JsonValue::String(description.to_string()));
    o
}

fn schema_string_array(description: &str) -> JsonValue {
    let mut items = JsonValue::object();
    items.insert("type", JsonValue::String("string".to_string()));
    let mut o = JsonValue::object();
    o.insert("type", JsonValue::String("array".to_string()));
    o.insert("items", items);
    o.insert("description", JsonValue::String(description.to_string()));
    o
}

fn build_search_files_schema() -> JsonValue {
    let mut props = JsonValue::object();
    props.insert(
        "query",
        schema_prop(
            "string",
            "Search keywords or Tools3000 query syntax (e.g. 'report ext:pdf' or pinyin).",
        ),
    );
    props.insert(
        "scope",
        schema_prop("string", "Optional directory prefix to restrict results (e.g. 'C:\\repo\\Tools3000')."),
    );
    props.insert(
        "extensions",
        schema_string_array("Optional file extensions filter (e.g. ['rs', 'md', 'pdf'])."),
    );
    props.insert(
        "limit",
        schema_prop("integer", "Maximum results to return (1-200, default 30)."),
    );
    props.insert(
        "include_content_matches",
        schema_prop("boolean", "If true, also match document content in addition to filenames."),
    );
    props.insert(
        "case_sensitive",
        schema_prop("boolean", "Enable case-sensitive matching (default false)."),
    );
    props.insert(
        "match_path",
        schema_prop("boolean", "Match query against full path instead of filename only."),
    );

    let mut schema = JsonValue::object();
    schema.insert("type", JsonValue::String("object".to_string()));
    schema.insert("properties", props);
    schema.insert(
        "required",
        JsonValue::Array(vec![JsonValue::String("query".to_string())]),
    );
    schema
}

fn build_search_content_schema() -> JsonValue {
    let mut props = JsonValue::object();
    props.insert(
        "query",
        schema_prop("string", "Text or keywords to search inside file contents."),
    );
    props.insert(
        "scope",
        schema_prop("string", "Recommended directory scope to speed up content scanning."),
    );
    props.insert(
        "extensions",
        schema_string_array("Optional file extensions filter (e.g. ['md', 'ts', 'docx', 'pdf'])."),
    );
    props.insert(
        "limit",
        schema_prop("integer", "Maximum matched files to return (1-100, default 20)."),
    );
    props.insert(
        "max_snippet_chars",
        schema_prop("integer", "Maximum characters per matched line snippet (40-1200, default 240)."),
    );
    props.insert(
        "case_sensitive",
        schema_prop("boolean", "Enable case-sensitive content matching (default false)."),
    );

    let mut schema = JsonValue::object();
    schema.insert("type", JsonValue::String("object".to_string()));
    schema.insert("properties", props);
    schema.insert(
        "required",
        JsonValue::Array(vec![JsonValue::String("query".to_string())]),
    );
    schema
}

fn build_index_status_schema() -> JsonValue {
    let mut props = JsonValue::object();
    props.insert(
        "start_if_stopped",
        schema_prop(
            "boolean",
            "Whether to wake the idle DEMAND_START search service if not yet running (default false).",
        ),
    );
    let mut schema = JsonValue::object();
    schema.insert("type", JsonValue::String("object".to_string()));
    schema.insert("properties", props);
    schema
}

fn extract_extensions(args: &JsonValue) -> Vec<String> {
    if let Some(arr) = args.get("extensions").and_then(|v| v.as_array()) {
        return arr
            .iter()
            .filter_map(|v| v.as_str().map(|s| s.to_string()))
            .collect();
    }
    if let Some(s) = args.get("extensions").and_then(|v| v.as_str()) {
        return vec![s.to_string()];
    }
    Vec::new()
}

fn dispatch_tool_call(
    cfg: &LoadedCliConfig,
    backend: &dyn SearchBackend,
    tool_name: &str,
    args: &JsonValue,
) -> JsonValue {
    let result = match tool_name {
        "search_files" => {
            let params = FileSearchParams {
                query: args
                    .get("query")
                    .and_then(|v| v.as_str())
                    .unwrap_or("")
                    .to_string(),
                scope: args
                    .get("scope")
                    .and_then(|v| v.as_str())
                    .map(|s| s.to_string()),
                extensions: extract_extensions(args),
                limit: args
                    .get("limit")
                    .and_then(|v| v.as_u64())
                    .map(|v| v as usize)
                    .unwrap_or(DEFAULT_FILE_LIMIT),
                include_content_matches: args
                    .get("include_content_matches")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false),
                case_sensitive: args
                    .get("case_sensitive")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false),
                match_path: args
                    .get("match_path")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false),
                pinyin: args.get("pinyin").and_then(|v| v.as_bool()),
            };
            execute_file_search(cfg, backend, ClientSource::Mcp, &params)
        }
        "search_content" => {
            let params = ContentSearchParams {
                query: args
                    .get("query")
                    .and_then(|v| v.as_str())
                    .unwrap_or("")
                    .to_string(),
                scope: args
                    .get("scope")
                    .and_then(|v| v.as_str())
                    .map(|s| s.to_string()),
                extensions: extract_extensions(args),
                limit: args
                    .get("limit")
                    .and_then(|v| v.as_u64())
                    .map(|v| v as usize)
                    .unwrap_or(DEFAULT_CONTENT_LIMIT),
                max_snippet_chars: args
                    .get("max_snippet_chars")
                    .and_then(|v| v.as_u64())
                    .map(|v| v as usize)
                    .unwrap_or(DEFAULT_MAX_SNIPPET_CHARS),
                case_sensitive: args
                    .get("case_sensitive")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false),
            };
            execute_content_search(cfg, backend, ClientSource::Mcp, &params)
        }
        "get_index_status" => {
            let start_if_stopped = args
                .get("start_if_stopped")
                .and_then(|v| v.as_bool())
                .unwrap_or(false);
            execute_index_status(cfg, backend, ClientSource::Mcp, start_if_stopped)
        }
        _ => {
            return make_tool_text_result(
                format!("Unknown tool: {}", tool_name),
                None,
                true,
            );
        }
    };

    match result {
        Ok(payload) => {
            let text = payload.to_json_pretty();
            make_tool_text_result(text, Some(payload), false)
        }
        Err(err) => {
            let mut err_obj = JsonValue::object();
            err_obj.insert("errorCode", JsonValue::String(err.code().to_string()));
            err_obj.insert("error", JsonValue::String(err.message().to_string()));
            make_tool_text_result(err_obj.to_json_pretty(), Some(err_obj), true)
        }
    }
}

fn make_tool_text_result(
    text: String,
    structured: Option<JsonValue>,
    is_error: bool,
) -> JsonValue {
    let mut content_item = JsonValue::object();
    content_item.insert("type", JsonValue::String("text".to_string()));
    content_item.insert("text", JsonValue::String(text));

    let mut res = JsonValue::object();
    res.insert("content", JsonValue::Array(vec![content_item]));
    if let Some(sc) = structured {
        res.insert("structuredContent", sc);
    }
    if is_error {
        res.insert("isError", JsonValue::Bool(true));
    }
    res
}

fn make_jsonrpc_result(id: JsonValue, result: JsonValue) -> JsonValue {
    let mut resp = JsonValue::object();
    resp.insert("jsonrpc", JsonValue::String("2.0".to_string()));
    resp.insert("id", id);
    resp.insert("result", result);
    resp
}

fn make_jsonrpc_error(id: JsonValue, code: i64, message: &str) -> JsonValue {
    let mut err = JsonValue::object();
    err.insert("code", JsonValue::Int(code));
    err.insert("message", JsonValue::String(message.to_string()));

    let mut resp = JsonValue::object();
    resp.insert("jsonrpc", JsonValue::String("2.0".to_string()));
    resp.insert("id", id);
    resp.insert("error", err);
    resp
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;
    use std::io::Cursor;

    struct CountingMockBackend {
        running: bool,
        calls: Cell<usize>,
        auto_starts: Cell<usize>,
    }

    impl SearchBackend for CountingMockBackend {
        fn is_pipe_running(&self) -> bool {
            self.running
        }

        fn send_request(&self, _payload: &JsonValue, auto_start: bool) -> Result<JsonValue, String> {
            self.calls.set(self.calls.get() + 1);
            if auto_start {
                self.auto_starts.set(self.auto_starts.get() + 1);
            }
            let mut item_ok = JsonValue::object();
            item_ok.insert("name", JsonValue::String("README.md".to_string()));
            item_ok.insert("path", JsonValue::String(r"C:\repo\Tools3000\README.md".to_string()));
            item_ok.insert("isDirectory", JsonValue::Bool(false));
            item_ok.insert("size", JsonValue::Int(1024));

            let mut item_secret = JsonValue::object();
            item_secret.insert("name", JsonValue::String("id_ed25519".to_string()));
            item_secret.insert("path", JsonValue::String(r"C:\Users\alice\.ssh\id_ed25519".to_string()));
            item_secret.insert("isDirectory", JsonValue::Bool(false));

            let mut resp = JsonValue::object();
            resp.insert("results", JsonValue::Array(vec![item_secret, item_ok]));
            resp.insert("totalIndexedFiles", JsonValue::Int(50000));
            resp.insert("elapsedMs", JsonValue::Int(4));
            Ok(resp)
        }
    }

    #[test]
    fn handshake_and_tools_list_never_touch_or_start_service() {
        let backend = CountingMockBackend {
            running: false,
            calls: Cell::new(0),
            auto_starts: Cell::new(0),
        };
        let input = concat!(
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\"}}\n",
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n",
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}\n",
            "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}\n"
        );
        let mut reader = Cursor::new(input.as_bytes());
        let mut output = Vec::new();
        run_mcp_loop(&mut reader, &mut output, &backend, LoadedCliConfig::default).unwrap();

        // 严格 DEMAND_START 不变量：初始化、通知、心跳与工具列表必须 0 次触碰搜索服务
        assert_eq!(backend.calls.get(), 0);
        assert_eq!(backend.auto_starts.get(), 0);

        let lines: Vec<&str> = std::str::from_utf8(&output).unwrap().lines().collect();
        assert_eq!(lines.len(), 3);
        let list_resp = parse_json(lines[2]).unwrap();
        let tools = list_resp
            .get("result")
            .and_then(|r| r.get("tools"))
            .and_then(|t| t.as_array())
            .unwrap();
        assert_eq!(tools.len(), 3);
    }

    #[test]
    fn blocks_tool_calls_when_external_api_disabled_and_redacts_secrets_when_enabled() {
        let backend = CountingMockBackend {
            running: false,
            calls: Cell::new(0),
            auto_starts: Cell::new(0),
        };

        // 1. 默认关闭态：拒绝且 0 次唤醒服务
        let call_req = parse_json(
            r#"{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"search_files","arguments":{"query":"readme"}}}"#,
        )
        .unwrap();
        let resp_disabled = handle_rpc_message(&call_req, &backend, &mut LoadedCliConfig::default).unwrap();
        assert_eq!(
            resp_disabled.get("result").and_then(|r| r.get("isError")).and_then(|v| v.as_bool()),
            Some(true)
        );
        assert_eq!(backend.calls.get(), 0);

        // 2. 开启后：按需查询并自动剔除 .ssh/id_ed25519 敏感路径
        let mut loader = || {
            let mut c = LoadedCliConfig::default();
            c.policy.external_api_enabled = true;
            c
        };
        let resp_enabled = handle_rpc_message(&call_req, &backend, &mut loader).unwrap();
        assert_eq!(backend.calls.get(), 1);
        assert_eq!(backend.auto_starts.get(), 1);
        let sc = resp_enabled
            .get("result")
            .and_then(|r| r.get("structuredContent"))
            .unwrap();
        assert_eq!(sc.get("count").and_then(|v| v.as_i64()), Some(1));
        assert_eq!(sc.get("redactedCount").and_then(|v| v.as_i64()), Some(1));
    }

    #[test]
    fn supports_content_length_framing() {
        let backend = CountingMockBackend {
            running: false,
            calls: Cell::new(0),
            auto_starts: Cell::new(0),
        };
        let body = r#"{"jsonrpc":"2.0","id":7,"method":"ping"}"#;
        let wire = format!("Content-Length: {}\r\n\r\n{}", body.len(), body);
        let mut reader = Cursor::new(wire.as_bytes());
        let mut output = Vec::new();
        run_mcp_loop(&mut reader, &mut output, &backend, LoadedCliConfig::default).unwrap();
        let out_str = std::str::from_utf8(&output).unwrap();
        assert!(out_str.starts_with("Content-Length: "));
        assert!(out_str.contains("\"id\":7"));
    }
}
