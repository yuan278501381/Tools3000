// ─────────────────────────────────────────────────────────────────────────────
// core/logger.rs — 结构化日志引擎（5MB 轮转 · TraceID 透传 · 严禁 Emoji）
//                  格式: [YYYY-MM-DD HH:mm:ss.mmm] [level] [tid:xxxx] [traceId] [feature] message
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::constants::{LOG_BACKUP_FILE_NAME, LOG_FILE_NAME, LOG_MAX_FILE_BYTES};
use std::fs::{self, File, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::{Mutex, OnceLock};
use windows::Win32::Foundation::SYSTEMTIME;
use windows::Win32::System::SystemInformation::GetLocalTime;
use windows::Win32::System::Threading::GetCurrentThreadId;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
}

impl LogLevel {
    pub fn from_str_config(s: &str) -> Self {
        match s.trim().to_ascii_lowercase().as_str() {
            "trace" => LogLevel::Trace,
            "debug" => LogLevel::Debug,
            "warn" | "warning" => LogLevel::Warn,
            "error" | "critical" => LogLevel::Error,
            _ => LogLevel::Info,
        }
    }

    fn as_tag(self) -> &'static str {
        match self {
            LogLevel::Trace => "trace",
            LogLevel::Debug => "debug",
            LogLevel::Info => "info",
            LogLevel::Warn => "warn",
            LogLevel::Error => "error",
        }
    }
}

struct LoggerState {
    log_path: Option<PathBuf>,
    backup_path: Option<PathBuf>,
    file: Option<File>,
    written_bytes: u64,
}

static LOGGER: OnceLock<Mutex<LoggerState>> = OnceLock::new();
// TODO(pre-release): 待缺陷收敛进入正式发版周期前，将默认日志级别切回 LogLevel::Info
static MIN_LEVEL: AtomicU32 = AtomicU32::new(LogLevel::Trace as u32);
static TRACE_SEQ: AtomicU64 = AtomicU64::new(1);

/// 初始化或热更新日志目录与日志级别（无 panic）
pub fn configure_logger(log_dir: Option<&Path>, level: LogLevel) {
    MIN_LEVEL.store(level as u32, Ordering::Relaxed);
    let mutex = LOGGER.get_or_init(|| {
        Mutex::new(LoggerState {
            log_path: None,
            backup_path: None,
            file: None,
            written_bytes: 0,
        })
    });
    if let Ok(mut guard) = mutex.lock() {
        if let Some(dir) = log_dir {
            let target = dir.join(LOG_FILE_NAME);
            if guard.log_path.as_ref() != Some(&target) {
                let _ = fs::create_dir_all(dir);
                let backup = dir.join(LOG_BACKUP_FILE_NAME);
                let existing_len = fs::metadata(&target).map(|m| m.len()).unwrap_or(0);
                let opened = OpenOptions::new().create(true).append(true).open(&target).ok();
                guard.log_path = Some(target);
                guard.backup_path = Some(backup);
                guard.file = opened;
                guard.written_bytes = existing_len;
            }
        }
    }
}

/// 生成递增短 TraceID（便于同一次控制动作跨阶段串联日志）
pub fn next_trace_id(prefix: &str) -> String {
    let seq = TRACE_SEQ.fetch_add(1, Ordering::Relaxed);
    format!("{prefix}-{seq:06x}")
}

/// 写入一条规范化英文结构化日志
pub fn log_event(level: LogLevel, trace_id: &str, feature: &str, message: &str) {
    if (level as u32) < MIN_LEVEL.load(Ordering::Relaxed) {
        return;
    }

    let tid = unsafe { GetCurrentThreadId() };
    let ts = format_local_timestamp();
    let trace = if trace_id.is_empty() { "-" } else { trace_id };
    let line = format!(
        "[{ts}] [{}] [tid:{tid:04x}] [{trace}] [{feature}] {message}\n",
        level.as_tag()
    );

    if let Some(mutex) = LOGGER.get() {
        if let Ok(mut guard) = mutex.lock() {
            if guard.written_bytes + (line.len() as u64) >= LOG_MAX_FILE_BYTES {
                rotate_locked(&mut guard);
            }
            if guard.file.is_none() {
                if let Some(path) = guard.log_path.as_ref() {
                    guard.file = OpenOptions::new().create(true).append(true).open(path).ok();
                }
            }
            if let Some(file) = guard.file.as_mut() {
                if file.write_all(line.as_bytes()).is_ok() {
                    guard.written_bytes += line.len() as u64;
                }
            }
        }
    }
}

/// 刷新磁盘日志缓冲（用于优雅退出前）
pub fn flush_logger() {
    if let Some(mutex) = LOGGER.get() {
        if let Ok(mut guard) = mutex.lock() {
            if let Some(file) = guard.file.as_mut() {
                let _ = file.flush();
            }
        }
    }
}

fn rotate_locked(state: &mut LoggerState) {
    state.file = None;
    if let (Some(log_path), Some(backup_path)) = (state.log_path.as_ref(), state.backup_path.as_ref()) {
        let _ = fs::remove_file(backup_path);
        let _ = fs::rename(log_path, backup_path);
        state.file = OpenOptions::new().create(true).write(true).truncate(true).open(log_path).ok();
        state.written_bytes = 0;
    }
}

fn format_local_timestamp() -> String {
    let st: SYSTEMTIME = unsafe { GetLocalTime() };
    format!(
        "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds
    )
}

#[macro_export]
macro_rules! wk_trace {
    ($trace:expr, $feature:expr, $($arg:tt)*) => {
        $crate::core::logger::log_event(
            $crate::core::logger::LogLevel::Trace,
            $trace,
            $feature,
            &format!($($arg)*),
        )
    };
}

#[macro_export]
macro_rules! wk_debug {
    ($trace:expr, $feature:expr, $($arg:tt)*) => {
        $crate::core::logger::log_event(
            $crate::core::logger::LogLevel::Debug,
            $trace,
            $feature,
            &format!($($arg)*),
        )
    };
}

#[macro_export]
macro_rules! wk_info {
    ($trace:expr, $feature:expr, $($arg:tt)*) => {
        $crate::core::logger::log_event(
            $crate::core::logger::LogLevel::Info,
            $trace,
            $feature,
            &format!($($arg)*),
        )
    };
}

#[macro_export]
macro_rules! wk_warn {
    ($trace:expr, $feature:expr, $($arg:tt)*) => {
        $crate::core::logger::log_event(
            $crate::core::logger::LogLevel::Warn,
            $trace,
            $feature,
            &format!($($arg)*),
        )
    };
}

#[macro_export]
macro_rules! wk_error {
    ($trace:expr, $feature:expr, $($arg:tt)*) => {
        $crate::core::logger::log_event(
            $crate::core::logger::LogLevel::Error,
            $trace,
            $feature,
            &format!($($arg)*),
        )
    };
}
