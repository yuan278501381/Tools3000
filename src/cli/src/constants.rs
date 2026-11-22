// ─────────────────────────────────────────────────────────────────────────────
// constants.rs — Tools3000 CLI & MCP 单一事实源常量定义
// ─────────────────────────────────────────────────────────────────────────────

pub const CLI_NAME: &str = "tools3000-cli";
pub const MCP_SERVER_NAME: &str = "tools3000-search";
pub const CLI_VERSION: &str = env!("CARGO_PKG_VERSION");
pub const MCP_PROTOCOL_VERSION: &str = "2024-11-05";

// Windows 搜索服务与命名管道端点契约（与 src/service/PipeEndpoint.h 1:1 对齐）
pub const SEARCH_SERVICE_NAME: &str = "Tools3000_SearchService";
pub const KERNEL_AUTH_PIPE_NAME: &str = r"\\.\pipe\Tools3000SearchPipe_KernelAuth";
pub const USER_SID_PIPE_PREFIX: &str = r"\\.\pipe\Tools3000SearchPipe_Sid_";
pub const TOKEN_PIPE_PREFIX: &str = r"\\.\pipe\Tools3000SearchPipe-";
pub const TOKEN_HEX_LEN: usize = 32;

// 4 字节小端分帧协议限制（与 src/service/PipeProtocol.h 1:1 对齐）
pub const MAX_REQUEST_BYTES: usize = 1024 * 1024;
pub const MAX_RESPONSE_BYTES: usize = 64 * 1024 * 1024;
pub const PIPE_IO_CHUNK_BYTES: usize = 64 * 1024;

// 查询与上下文 Token 预算限制
pub const DEFAULT_FILE_LIMIT: usize = 30;
pub const MAX_FILE_LIMIT: usize = 200;
pub const DEFAULT_CONTENT_LIMIT: usize = 20;
pub const MAX_CONTENT_LIMIT: usize = 100;
pub const DEFAULT_MAX_SNIPPET_CHARS: usize = 240;
pub const MIN_SNIPPET_CHARS: usize = 40;
pub const MAX_SNIPPET_CHARS: usize = 1200;
pub const MAX_SNIPPETS_PER_FILE: usize = 3;
pub const MAX_QUERY_BYTES: usize = 1024;

// 服务唤醒等待上限
pub const SERVICE_START_HARD_DEADLINE_MS: u64 = 30_000;
pub const SERVICE_POLL_INTERVAL_MS: u64 = 120;
pub const SERVICE_INIT_WAIT_DEADLINE_MS: u64 = 12_000;

// 退出码规范
pub const EXIT_OK: i32 = 0;
pub const EXIT_USAGE: i32 = 2;
pub const EXIT_API_DISABLED: i32 = 3;
pub const EXIT_SERVICE_ERROR: i32 = 4;

/// 零信任内置敏感路径片段黑名单（路径中任一目录段或完整后缀命中即拦截）
pub const BUILTIN_SENSITIVE_SEGMENTS: &[&str] = &[
    ".ssh",
    ".gnupg",
    ".aws",
    ".azure",
    ".kube",
    ".gcloud",
    ".terraform.d",
    "1password",
    "bitwarden",
    "keepass",
    "microsoft\\credentials",
    "microsoft\\vault",
    "microsoft\\protect",
    "microsoft\\crypto",
    "google\\chrome\\user data",
    "microsoft\\edge\\user data",
    "mozilla\\firefox\\profiles",
    "bravesoftware\\brave-browser\\user data",
];

/// 零信任内置敏感文件名/扩展名黑名单（小写比对）
pub const BUILTIN_SENSITIVE_FILENAMES: &[&str] = &[
    ".env",
    ".env.local",
    ".env.production",
    ".env.development",
    ".env.staging",
    ".env.secret",
    ".git-credentials",
    ".npmrc",
    ".pypirc",
    ".netrc",
    "_netrc",
    "id_rsa",
    "id_ed25519",
    "id_ecdsa",
    "id_dsa",
    "authorized_keys",
    "known_hosts",
    "credentials.json",
    "service-account.json",
    "secrets.json",
    "secrets.yaml",
    "secrets.yml",
    "login data",
    "local state",
    "cookies",
    "web data",
    "key4.db",
    "logins.json",
    "ntuser.dat",
];

/// 内置敏感凭证文件扩展名（小写比对）
pub const BUILTIN_SENSITIVE_EXTENSIONS: &[&str] = &[
    "pem",
    "key",
    "p12",
    "pfx",
    "kdbx",
    "jks",
    "keystore",
    "ppk",
];
