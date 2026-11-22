// ─────────────────────────────────────────────────────────────────────────────
// main.rs — tools3000-cli.exe 入口点（极速纯 Rust CLI + MCP stdio Server）
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors
// ─────────────────────────────────────────────────────────────────────────────

mod cli;
mod config;
mod constants;
mod json;
mod mcp;
mod pipe_client;
mod policy;
mod service_api;

use std::env;
use std::process;

fn main() {
    let args: Vec<String> = env::args().skip(1).collect();
    let code = cli::run_cli(&args);
    process::exit(code);
}
