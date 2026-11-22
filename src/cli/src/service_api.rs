// ─────────────────────────────────────────────────────────────────────────────
// service_api.rs — CLI 与 MCP 共享的高内聚查询编排单一事实源 (SSOT)
// ─────────────────────────────────────────────────────────────────────────────

use crate::config::LoadedCliConfig;
use crate::constants::{
    DEFAULT_CONTENT_LIMIT, DEFAULT_FILE_LIMIT, DEFAULT_MAX_SNIPPET_CHARS, MAX_CONTENT_LIMIT,
    MAX_FILE_LIMIT, MAX_QUERY_BYTES, MAX_SNIPPET_CHARS, MIN_SNIPPET_CHARS,
};
use crate::json::JsonValue;
use crate::pipe_client::SearchBackend;
use crate::policy::build_service_query_expression;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ClientSource {
    Cli,
    Mcp,
}

impl ClientSource {
    pub fn as_str(&self) -> &'static str {
        match self {
            ClientSource::Cli => "external_cli",
            ClientSource::Mcp => "external_mcp",
        }
    }
}

#[derive(Debug, Clone)]
pub struct FileSearchParams {
    pub query: String,
    pub scope: Option<String>,
    pub extensions: Vec<String>,
    pub limit: usize,
    pub include_content_matches: bool,
    pub case_sensitive: bool,
    pub match_path: bool,
    pub pinyin: Option<bool>,
}

#[derive(Debug, Clone)]
pub struct ContentSearchParams {
    pub query: String,
    pub scope: Option<String>,
    pub extensions: Vec<String>,
    pub limit: usize,
    pub max_snippet_chars: usize,
    pub case_sensitive: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ApiError {
    ExternalApiDisabled(String),
    InvalidInput(String),
    ServiceUnavailable(String),
}

impl ApiError {
    pub fn code(&self) -> &'static str {
        match self {
            ApiError::ExternalApiDisabled(_) => "external_api_disabled",
            ApiError::InvalidInput(_) => "invalid_input",
            ApiError::ServiceUnavailable(_) => "service_unavailable",
        }
    }

    pub fn message(&self) -> &str {
        match self {
            ApiError::ExternalApiDisabled(m)
            | ApiError::InvalidInput(m)
            | ApiError::ServiceUnavailable(m) => m.as_str(),
        }
    }
}

pub fn require_external_api_enabled(cfg: &LoadedCliConfig) -> Result<(), ApiError> {
    if cfg.policy.external_api_enabled {
        return Ok(());
    }
    let cfg_hint = cfg
        .config_path
        .as_ref()
        .map(|p| p.display().to_string())
        .unwrap_or_else(|| "%LOCALAPPDATA%\\Tools3000\\config\\config.json".to_string());
    Err(ApiError::ExternalApiDisabled(format!(
        "External CLI / MCP Search API is disabled by default for security. Enable it in Tools3000 Settings > Search > External CLI & MCP API (config: {}).",
        cfg_hint
    )))
}

pub fn execute_file_search(
    cfg: &LoadedCliConfig,
    backend: &dyn SearchBackend,
    source: ClientSource,
    params: &FileSearchParams,
) -> Result<JsonValue, ApiError> {
    require_external_api_enabled(cfg)?;

    let expr = build_service_query_expression(
        &params.query,
        params.scope.as_deref(),
        &params.extensions,
        false,
    );
    if expr.trim().is_empty() {
        return Err(ApiError::InvalidInput(
            "query cannot be empty (provide query, scope, or extensions)".to_string(),
        ));
    }
    if expr.len() > MAX_QUERY_BYTES {
        return Err(ApiError::InvalidInput(format!(
            "query expression exceeds {} bytes limit",
            MAX_QUERY_BYTES
        )));
    }

    let limit = if params.limit == 0 {
        DEFAULT_FILE_LIMIT
    } else {
        params.limit.clamp(1, MAX_FILE_LIMIT)
    };
    let fetch_limit = (limit.saturating_mul(3) + 16).min(400);

    let mut req = JsonValue::object();
    req.insert("clientSource", JsonValue::String(source.as_str().to_string()));
    req.insert("query", JsonValue::String(expr.clone()));
    req.insert(
        "searchMode",
        JsonValue::String(if params.include_content_matches {
            "both".to_string()
        } else {
            "name".to_string()
        }),
    );
    req.insert("limit", JsonValue::Int(fetch_limit as i64));
    req.insert("caseSensitive", JsonValue::Bool(params.case_sensitive));
    req.insert("matchPath", JsonValue::Bool(params.match_path));
    req.insert("excludeHidden", JsonValue::Bool(cfg.exclude_hidden));
    req.insert("excludeSystem", JsonValue::Bool(cfg.exclude_system));
    if let Some(py) = params.pinyin.or(cfg.pinyin_enabled) {
        req.insert("pinyinEnabled", JsonValue::Bool(py));
    }
    if !cfg.enabled_drives.is_empty() {
        req.insert(
            "drives",
            JsonValue::Array(
                cfg.enabled_drives
                    .iter()
                    .map(|d| JsonValue::String(d.clone()))
                    .collect(),
            ),
        );
    }
    let excludes = cfg.policy.merged_service_excludes(&cfg.global_excludes);
    if !excludes.is_empty() {
        req.insert(
            "excludes",
            JsonValue::Array(excludes.into_iter().map(JsonValue::String).collect()),
        );
    }

    let raw = backend
        .send_request(&req, true)
        .map_err(ApiError::ServiceUnavailable)?;
    if let Some(err) = raw.get("error").and_then(|v| v.as_str()) {
        if !err.is_empty() {
            return Err(ApiError::ServiceUnavailable(err.to_string()));
        }
    }

    let raw_items = raw
        .get("results")
        .and_then(|v| v.as_array())
        .unwrap_or(&[]);
    let (clean_items, redacted) =
        cfg.policy
            .sanitize_results(raw_items, limit, DEFAULT_MAX_SNIPPET_CHARS);

    let mut out = JsonValue::object();
    out.insert("success", JsonValue::Bool(true));
    out.insert("mode", JsonValue::String(if params.include_content_matches { "both".to_string() } else { "name".to_string() }));
    out.insert("query", JsonValue::String(expr));
    out.insert("count", JsonValue::Int(clean_items.len() as i64));
    out.insert("redactedCount", JsonValue::Int(redacted as i64));
    if let Some(total) = raw.get("totalIndexedFiles").and_then(|v| v.as_i64()) {
        out.insert("totalIndexedFiles", JsonValue::Int(total));
    }
    if let Some(ms) = raw.get("elapsedMs").and_then(|v| v.as_i64()) {
        out.insert("elapsedMs", JsonValue::Int(ms));
    }
    out.insert("results", JsonValue::Array(clean_items));
    Ok(out)
}

pub fn execute_content_search(
    cfg: &LoadedCliConfig,
    backend: &dyn SearchBackend,
    source: ClientSource,
    params: &ContentSearchParams,
) -> Result<JsonValue, ApiError> {
    require_external_api_enabled(cfg)?;

    if params.query.trim().is_empty() {
        return Err(ApiError::InvalidInput(
            "content search query cannot be empty".to_string(),
        ));
    }
    let expr = build_service_query_expression(
        &params.query,
        params.scope.as_deref(),
        &params.extensions,
        true,
    );
    if expr.len() > MAX_QUERY_BYTES {
        return Err(ApiError::InvalidInput(format!(
            "query expression exceeds {} bytes limit",
            MAX_QUERY_BYTES
        )));
    }

    let limit = if params.limit == 0 {
        DEFAULT_CONTENT_LIMIT
    } else {
        params.limit.clamp(1, MAX_CONTENT_LIMIT)
    };
    let fetch_limit = (limit.saturating_mul(2) + 8).min(200);
    let max_snippet_chars = if params.max_snippet_chars == 0 {
        DEFAULT_MAX_SNIPPET_CHARS
    } else {
        params
            .max_snippet_chars
            .clamp(MIN_SNIPPET_CHARS, MAX_SNIPPET_CHARS)
    };

    let mut req = JsonValue::object();
    req.insert("clientSource", JsonValue::String(source.as_str().to_string()));
    req.insert("query", JsonValue::String(expr.clone()));
    req.insert("searchMode", JsonValue::String("content".to_string()));
    req.insert("progressive", JsonValue::Bool(false));
    req.insert("limit", JsonValue::Int(fetch_limit as i64));
    req.insert("caseSensitive", JsonValue::Bool(params.case_sensitive));
    req.insert("excludeHidden", JsonValue::Bool(cfg.exclude_hidden));
    req.insert("excludeSystem", JsonValue::Bool(cfg.exclude_system));
    if !cfg.enabled_drives.is_empty() {
        req.insert(
            "drives",
            JsonValue::Array(
                cfg.enabled_drives
                    .iter()
                    .map(|d| JsonValue::String(d.clone()))
                    .collect(),
            ),
        );
    }
    let excludes = cfg.policy.merged_service_excludes(&cfg.global_excludes);
    if !excludes.is_empty() {
        req.insert(
            "excludes",
            JsonValue::Array(excludes.into_iter().map(JsonValue::String).collect()),
        );
    }

    let raw = backend
        .send_request(&req, true)
        .map_err(ApiError::ServiceUnavailable)?;
    if let Some(err) = raw.get("error").and_then(|v| v.as_str()) {
        if !err.is_empty() {
            return Err(ApiError::ServiceUnavailable(err.to_string()));
        }
    }

    let raw_items = raw
        .get("results")
        .and_then(|v| v.as_array())
        .unwrap_or(&[]);
    let (clean_items, redacted) = cfg
        .policy
        .sanitize_results(raw_items, limit, max_snippet_chars);

    let mut out = JsonValue::object();
    out.insert("success", JsonValue::Bool(true));
    out.insert("mode", JsonValue::String("content".to_string()));
    out.insert("query", JsonValue::String(expr));
    out.insert("count", JsonValue::Int(clean_items.len() as i64));
    out.insert("redactedCount", JsonValue::Int(redacted as i64));
    out.insert("maxSnippetChars", JsonValue::Int(max_snippet_chars as i64));
    if let Some(total) = raw.get("totalIndexedFiles").and_then(|v| v.as_i64()) {
        out.insert("totalIndexedFiles", JsonValue::Int(total));
    }
    if let Some(ms) = raw.get("elapsedMs").and_then(|v| v.as_i64()) {
        out.insert("elapsedMs", JsonValue::Int(ms));
    }
    out.insert("results", JsonValue::Array(clean_items));
    Ok(out)
}

pub fn execute_index_status(
    cfg: &LoadedCliConfig,
    backend: &dyn SearchBackend,
    source: ClientSource,
    start_if_stopped: bool,
) -> Result<JsonValue, ApiError> {
    require_external_api_enabled(cfg)?;

    // 严格遵守 DEMAND_START：默认不唤醒处于待命状态的搜索服务
    if !start_if_stopped && !backend.is_pipe_running() {
        let mut idle = JsonValue::object();
        idle.insert("success", JsonValue::Bool(true));
        idle.insert("running", JsonValue::Bool(false));
        idle.insert("startMode", JsonValue::String("DEMAND_START".to_string()));
        idle.insert(
            "externalApiEnabled",
            JsonValue::Bool(cfg.policy.external_api_enabled),
        );
        idle.insert(
            "message",
            JsonValue::String(
                "Tools3000_SearchService is currently stopped (DEMAND_START). It will start automatically on the first search query."
                    .to_string(),
            ),
        );
        return Ok(idle);
    }

    let mut req = JsonValue::object();
    req.insert("clientSource", JsonValue::String(source.as_str().to_string()));
    req.insert("action", JsonValue::String("getDbStats".to_string()));

    let raw = backend
        .send_request(&req, start_if_stopped)
        .map_err(ApiError::ServiceUnavailable)?;

    let mut out = JsonValue::object();
    out.insert("success", JsonValue::Bool(true));
    out.insert("running", JsonValue::Bool(true));
    out.insert("startMode", JsonValue::String("DEMAND_START".to_string()));
    out.insert(
        "externalApiEnabled",
        JsonValue::Bool(cfg.policy.external_api_enabled),
    );
    for key in [
        "totalRecords",
        "volumeCount",
        "indexedVolumes",
        "totalVolumes",
        "indexing",
        "phase",
        "progressPercent",
        "volumes",
    ] {
        if let Some(val) = raw.get(key) {
            out.insert(key, val.clone());
        }
    }
    Ok(out)
}
