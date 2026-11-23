// ─────────────────────────────────────────────────────────────────────────────
// pipe_client.rs — 纯 Win32 FFI 命名管道分帧通信与 DEMAND_START 按需服务客户端
//
// 契约对齐:
//   - 帧协议 100% 对齐 src/service/PipeProtocol.h (4 字节小端长度前缀 + UTF-8 JSON)；
//   - 端点优先链 100% 对齐 src/search/SearchPlugin.cpp (KernelAuth -> UserSid -> Token)；
//   - 严格恪守 DEMAND_START 铁律：auto_start=false 时零 SCM 启动、零便携进程拉起。
// ─────────────────────────────────────────────────────────────────────────────

use crate::constants::{MAX_REQUEST_BYTES, TOKEN_HEX_LEN};
use crate::json::JsonValue;

/// 可测试的抽象搜索后端接口（便于单元测试注入离线 Mock，验证零唤醒不变量）
pub trait SearchBackend {
    fn is_pipe_running(&self) -> bool;
    fn send_request(&self, payload: &JsonValue, auto_start: bool) -> Result<JsonValue, String>;
}

// ── 4 字节小端分帧编码/解码纯函数 ────────────────────────────────────────────

#[cfg_attr(not(windows), allow(dead_code))]
pub fn encode_frame_header(payload_len: usize) -> Result<[u8; 4], String> {
    if payload_len == 0 || payload_len > MAX_REQUEST_BYTES {
        return Err(format!("request payload size {} out of range", payload_len));
    }
    Ok((payload_len as u32).to_le_bytes())
}

#[cfg_attr(not(windows), allow(dead_code))]
pub fn decode_frame_header(header: [u8; 4], limit: usize) -> Result<usize, String> {
    let len = u32::from_le_bytes(header) as usize;
    if len == 0 || len > limit {
        return Err(format!("invalid frame length {} (limit {})", len, limit));
    }
    Ok(len)
}

#[cfg_attr(not(windows), allow(dead_code))]
pub fn is_valid_pipe_token(token: &str) -> bool {
    token.len() == TOKEN_HEX_LEN
        && token
            .bytes()
            .all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

// ── Win32 FFI 实现（仅在 Windows 目标编译；Linux 容器仅执行 MCP Introspection 静态响应） ──

#[cfg(windows)]
mod platform {
    use super::{decode_frame_header, encode_frame_header, is_valid_pipe_token, SearchBackend};
    use crate::config::LoadedCliConfig;
    use crate::constants::{
        KERNEL_AUTH_PIPE_NAME, MAX_RESPONSE_BYTES, PIPE_IO_CHUNK_BYTES, SEARCH_SERVICE_NAME,
        SERVICE_INIT_WAIT_DEADLINE_MS, SERVICE_POLL_INTERVAL_MS, SERVICE_START_HARD_DEADLINE_MS,
        TOKEN_PIPE_PREFIX, USER_SID_PIPE_PREFIX,
    };
    use crate::json::{parse_json, JsonValue};
    use std::env;
    use std::ffi::OsStr;
    use std::os::windows::ffi::OsStrExt;
    use std::path::PathBuf;
    use std::ptr;
    use std::thread;
    use std::time::{Duration, Instant};

    // ── Win32 FFI 声明（kernel32 + advapi32，兼容 x64 与 ARM64 Windows MSVC） ─────

type Handle = *mut core::ffi::c_void;
type ScHandle = *mut core::ffi::c_void;
type Bool = i32;
type Dword = u32;

const INVALID_HANDLE_VALUE: Handle = -1isize as Handle;
const GENERIC_READ: Dword = 0x8000_0000;
const GENERIC_WRITE: Dword = 0x4000_0000;
const OPEN_EXISTING: Dword = 3;
const ERROR_PIPE_BUSY: Dword = 231;
const ERROR_SEM_TIMEOUT: Dword = 121;
const ERROR_ACCESS_DENIED: Dword = 5;
const ERROR_SERVICE_DOES_NOT_EXIST: Dword = 1060;
const ERROR_SERVICE_ALREADY_RUNNING: Dword = 1056;
const TOKEN_QUERY: Dword = 0x0008;
const TOKEN_USER_CLASS: Dword = 1;

const SC_MANAGER_CONNECT: Dword = 0x0001;
const SERVICE_QUERY_STATUS: Dword = 0x0004;
const SERVICE_START: Dword = 0x0010;
const SC_STATUS_PROCESS_INFO: Dword = 0;

const SERVICE_STOPPED: Dword = 1;
const SERVICE_START_PENDING: Dword = 2;
const SERVICE_STOP_PENDING: Dword = 3;
const SERVICE_RUNNING: Dword = 4;

const CREATE_NO_WINDOW: Dword = 0x0800_0000;
const DETACHED_PROCESS: Dword = 0x0000_0008;
const STARTF_USESHOWWINDOW: Dword = 0x0000_0001;
const SW_HIDE: u16 = 0;
const WAIT_OBJECT_0: Dword = 0;
const WAIT_ABANDONED: Dword = 0x0000_0080;

#[repr(C)]
struct SidAndAttributes {
    sid: *mut core::ffi::c_void,
    attributes: Dword,
}

#[repr(C)]
struct TokenUser {
    user: SidAndAttributes,
}

#[repr(C)]
struct ServiceStatusProcess {
    service_type: Dword,
    current_state: Dword,
    controls_accepted: Dword,
    win32_exit_code: Dword,
    service_specific_exit_code: Dword,
    check_point: Dword,
    wait_hint: Dword,
    process_id: Dword,
    service_flags: Dword,
}

#[repr(C)]
struct StartupInfoW {
    cb: Dword,
    reserved: *mut u16,
    desktop: *mut u16,
    title: *mut u16,
    x: Dword,
    y: Dword,
    x_size: Dword,
    y_size: Dword,
    x_count_chars: Dword,
    y_count_chars: Dword,
    fill_attribute: Dword,
    flags: Dword,
    show_window: u16,
    cb_reserved2: u16,
    lp_reserved2: *mut u8,
    std_input: Handle,
    std_output: Handle,
    std_error: Handle,
}

#[repr(C)]
struct ProcessInformation {
    process: Handle,
    thread: Handle,
    process_id: Dword,
    thread_id: Dword,
}

#[link(name = "kernel32")]
extern "system" {
    fn GetLastError() -> Dword;
    fn CloseHandle(handle: Handle) -> Bool;
    fn LocalFree(hmem: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    fn GetCurrentProcess() -> Handle;
    fn WaitNamedPipeW(pipe_name: *const u16, timeout_ms: Dword) -> Bool;
    fn CreateFileW(
        file_name: *const u16,
        desired_access: Dword,
        share_mode: Dword,
        security_attributes: *mut core::ffi::c_void,
        creation_disposition: Dword,
        flags_and_attributes: Dword,
        template_file: Handle,
    ) -> Handle;
    fn ReadFile(
        file: Handle,
        buffer: *mut u8,
        bytes_to_read: Dword,
        bytes_read: *mut Dword,
        overlapped: *mut core::ffi::c_void,
    ) -> Bool;
    fn WriteFile(
        file: Handle,
        buffer: *const u8,
        bytes_to_write: Dword,
        bytes_written: *mut Dword,
        overlapped: *mut core::ffi::c_void,
    ) -> Bool;
    fn CreateMutexW(
        mutex_attributes: *mut core::ffi::c_void,
        initial_owner: Bool,
        name: *const u16,
    ) -> Handle;
    fn WaitForSingleObject(handle: Handle, milliseconds: Dword) -> Dword;
    fn ReleaseMutex(handle: Handle) -> Bool;
    fn CreateProcessW(
        application_name: *const u16,
        command_line: *mut u16,
        process_attributes: *mut core::ffi::c_void,
        thread_attributes: *mut core::ffi::c_void,
        inherit_handles: Bool,
        creation_flags: Dword,
        environment: *mut core::ffi::c_void,
        current_directory: *const u16,
        startup_info: *const StartupInfoW,
        process_information: *mut ProcessInformation,
    ) -> Bool;
}

#[link(name = "advapi32")]
extern "system" {
    fn OpenProcessToken(
        process_handle: Handle,
        desired_access: Dword,
        token_handle: *mut Handle,
    ) -> Bool;
    fn GetTokenInformation(
        token_handle: Handle,
        token_information_class: Dword,
        token_information: *mut u8,
        token_information_length: Dword,
        return_length: *mut Dword,
    ) -> Bool;
    fn ConvertSidToStringSidW(sid: *mut core::ffi::c_void, string_sid: *mut *mut u16) -> Bool;
    fn OpenSCManagerW(
        machine_name: *const u16,
        database_name: *const u16,
        desired_access: Dword,
    ) -> ScHandle;
    fn OpenServiceW(
        sc_manager: ScHandle,
        service_name: *const u16,
        desired_access: Dword,
    ) -> ScHandle;
    fn QueryServiceStatusEx(
        service: ScHandle,
        info_level: Dword,
        buffer: *mut u8,
        buf_size: Dword,
        bytes_needed: *mut Dword,
    ) -> Bool;
    fn StartServiceW(
        service: ScHandle,
        num_service_args: Dword,
        service_arg_vectors: *const *const u16,
    ) -> Bool;
    fn CloseServiceHandle(sc_object: ScHandle) -> Bool;
}

fn to_wide_null(s: &str) -> Vec<u16> {
    OsStr::new(s).encode_wide().chain(std::iter::once(0)).collect()
}

fn wide_ptr_to_string(ptr: *const u16) -> String {
    if ptr.is_null() {
        return String::new();
    }
    let mut len = 0usize;
    unsafe {
        while *ptr.add(len) != 0 {
            len += 1;
        }
        let slice = std::slice::from_raw_parts(ptr, len);
        String::from_utf16_lossy(slice)
    }
}

pub fn query_current_user_sid() -> Option<String> {
    let mut token: Handle = ptr::null_mut();
    unsafe {
        if OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) == 0 || token.is_null() {
            return None;
        }
        let mut needed: Dword = 0;
        GetTokenInformation(token, TOKEN_USER_CLASS, ptr::null_mut(), 0, &mut needed);
        if needed == 0 {
            CloseHandle(token);
            return None;
        }
        let mut buf = vec![0u8; needed as usize];
        let ok = GetTokenInformation(token, TOKEN_USER_CLASS, buf.as_mut_ptr(), needed, &mut needed);
        CloseHandle(token);
        if ok == 0 {
            return None;
        }
        let user = &*(buf.as_ptr() as *const TokenUser);
        if user.user.sid.is_null() {
            return None;
        }
        let mut raw_sid: *mut u16 = ptr::null_mut();
        if ConvertSidToStringSidW(user.user.sid, &mut raw_sid) == 0 || raw_sid.is_null() {
            return None;
        }
        let sid = wide_ptr_to_string(raw_sid);
        LocalFree(raw_sid as *mut core::ffi::c_void);
        if sid.is_empty() {
            None
        } else {
            Some(sid)
        }
    }
}

fn is_pipe_transient_busy(err: Dword) -> bool {
    err == ERROR_PIPE_BUSY || err == ERROR_SEM_TIMEOUT
}

fn is_pipe_present_or_busy(pipe_name: &str, wait_ms: Dword) -> bool {
    if pipe_name.is_empty() {
        return false;
    }
    let wide = to_wide_null(pipe_name);
    unsafe {
        if WaitNamedPipeW(wide.as_ptr(), wait_ms) != 0 {
            return true;
        }
        is_pipe_transient_busy(GetLastError())
    }
}

fn read_exact_handle(handle: Handle, buf: &mut [u8]) -> Result<(), String> {
    let mut offset = 0usize;
    while offset < buf.len() {
        let want = (buf.len() - offset).min(PIPE_IO_CHUNK_BYTES) as Dword;
        let mut got: Dword = 0;
        let ok = unsafe {
            ReadFile(
                handle,
                buf[offset..].as_mut_ptr(),
                want,
                &mut got,
                ptr::null_mut(),
            )
        };
        if ok == 0 || got == 0 {
            let err = unsafe { GetLastError() };
            return Err(format!("pipe ReadFile failed (win32={})", err));
        }
        offset += got as usize;
    }
    Ok(())
}

fn write_exact_handle(handle: Handle, data: &[u8]) -> Result<(), String> {
    let mut offset = 0usize;
    while offset < data.len() {
        let want = (data.len() - offset).min(PIPE_IO_CHUNK_BYTES) as Dword;
        let mut sent: Dword = 0;
        let ok = unsafe {
            WriteFile(
                handle,
                data[offset..].as_ptr(),
                want,
                &mut sent,
                ptr::null_mut(),
            )
        };
        if ok == 0 || sent == 0 {
            let err = unsafe { GetLastError() };
            return Err(format!("pipe WriteFile failed (win32={})", err));
        }
        offset += sent as usize;
    }
    Ok(())
}

/// 真实 Windows 命名管道搜索客户端（直连 Tools3000_Service.exe）
pub struct WinPipeBackend {
    pub client_sid: String,
    pub pipe_token: String,
    pub endpoints: Vec<String>,
}

impl WinPipeBackend {
    pub fn from_config(cfg: &LoadedCliConfig) -> Self {
        let sid = query_current_user_sid().unwrap_or_default();
        let mut endpoints = Vec::with_capacity(3);
        endpoints.push(KERNEL_AUTH_PIPE_NAME.to_string());
        if !sid.is_empty() {
            endpoints.push(format!("{}{}", USER_SID_PIPE_PREFIX, sid.to_ascii_uppercase()));
        }
        if is_valid_pipe_token(&cfg.pipe_token) {
            endpoints.push(format!("{}{}", TOKEN_PIPE_PREFIX, cfg.pipe_token));
        }
        Self {
            client_sid: sid,
            pipe_token: cfg.pipe_token.clone(),
            endpoints,
        }
    }

    fn resolve_active_pipe(&self, primary_wait_ms: Dword) -> Option<String> {
        for (idx, ep) in self.endpoints.iter().enumerate() {
            let wait = if idx == 0 { primary_wait_ms } else { 1 };
            if is_pipe_present_or_busy(ep, wait) {
                return Some(ep.clone());
            }
        }
        None
    }

    /// 按需启动 Tools3000_SearchService（严格遵循 ServiceStartupPolicy.h 单例与 DEMAND_START 契约）
    fn ensure_service_running(&self) -> Result<(), String> {
        if self.resolve_active_pipe(80).is_some() {
            return Ok(());
        }

        // 与 SearchPlugin.cpp 共享同一把进程间互斥锁，绝不并发拉起第二个索引进程
        let mutex_name = if !self.client_sid.is_empty() {
            format!("Local\\Tools3000SearchLaunch-{}", self.client_sid)
        } else {
            "Local\\Tools3000SearchLaunch-CLI".to_string()
        };
        let wide_mutex = to_wide_null(&mutex_name);
        let h_mutex = unsafe { CreateMutexW(ptr::null_mut(), 0, wide_mutex.as_ptr()) };
        if h_mutex.is_null() {
            return Err("failed to create service launch mutex".to_string());
        }
        let wait = unsafe { WaitForSingleObject(h_mutex, 10_000) };
        let locked = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        let res = if !locked {
            Err("timed out waiting for search service launch lock".to_string())
        } else if self.resolve_active_pipe(80).is_some() {
            Ok(())
        } else {
            self.start_scm_or_portable()
        };

        unsafe {
            if locked {
                ReleaseMutex(h_mutex);
            }
            CloseHandle(h_mutex);
        }
        res
    }

    fn start_scm_or_portable(&self) -> Result<(), String> {
        match self.try_start_scm_service() {
            ScmAttempt::Ready => return Ok(()),
            ScmAttempt::Unavailable(msg) => return Err(msg),
            ScmAttempt::AllowPortableFallback => {}
        }

        // SCM 不存在或无权限启动且未运行时，尝试拉起同目录 Tools3000_Service.exe 便携实例
        let service_exe = find_companion_service_exe().ok_or_else(|| {
            "Tools3000_SearchService is not running and companion Tools3000_Service.exe was not found"
                .to_string()
        })?;
        let exe_dir = service_exe
            .parent()
            .map(|p| p.to_path_buf())
            .unwrap_or_else(|| PathBuf::from("."));

        let mut cmd = format!("\"{}\"", service_exe.display());
        if is_valid_pipe_token(&self.pipe_token) {
            cmd.push_str(&format!(" --pipe-token={}", self.pipe_token));
        }
        if !self.client_sid.is_empty() {
            cmd.push_str(&format!(" --client-sid={}", self.client_sid));
        }

        let wide_exe = to_wide_null(&service_exe.to_string_lossy());
        let mut wide_cmd = to_wide_null(&cmd);
        let wide_dir = to_wide_null(&exe_dir.to_string_lossy());
        let mut si = StartupInfoW {
            cb: std::mem::size_of::<StartupInfoW>() as Dword,
            reserved: ptr::null_mut(),
            desktop: ptr::null_mut(),
            title: ptr::null_mut(),
            x: 0,
            y: 0,
            x_size: 0,
            y_size: 0,
            x_count_chars: 0,
            y_count_chars: 0,
            fill_attribute: 0,
            flags: STARTF_USESHOWWINDOW,
            show_window: SW_HIDE,
            cb_reserved2: 0,
            lp_reserved2: ptr::null_mut(),
            std_input: ptr::null_mut(),
            std_output: ptr::null_mut(),
            std_error: ptr::null_mut(),
        };
        let mut pi = ProcessInformation {
            process: ptr::null_mut(),
            thread: ptr::null_mut(),
            process_id: 0,
            thread_id: 0,
        };

        let ok = unsafe {
            CreateProcessW(
                wide_exe.as_ptr(),
                wide_cmd.as_mut_ptr(),
                ptr::null_mut(),
                ptr::null_mut(),
                0,
                CREATE_NO_WINDOW | DETACHED_PROCESS,
                ptr::null_mut(),
                wide_dir.as_ptr(),
                &mut si,
                &mut pi,
            )
        };
        if ok == 0 {
            let err = unsafe { GetLastError() };
            return Err(format!("failed to start Tools3000_Service.exe (win32={})", err));
        }
        unsafe {
            if !pi.process.is_null() {
                CloseHandle(pi.process);
            }
            if !pi.thread.is_null() {
                CloseHandle(pi.thread);
            }
        }

        let deadline = Instant::now() + Duration::from_millis(8_000);
        while Instant::now() < deadline {
            if self.resolve_active_pipe(150).is_some() {
                return Ok(());
            }
            thread::sleep(Duration::from_millis(SERVICE_POLL_INTERVAL_MS));
        }
        Err("Tools3000_Service.exe started but named pipe did not become ready in time".to_string())
    }

    fn try_start_scm_service(&self) -> ScmAttempt {
        unsafe {
            let scm = OpenSCManagerW(ptr::null(), ptr::null(), SC_MANAGER_CONNECT);
            if scm.is_null() {
                let err = GetLastError();
                return if err == ERROR_ACCESS_DENIED {
                    ScmAttempt::AllowPortableFallback
                } else {
                    ScmAttempt::Unavailable(format!("OpenSCManagerW failed (win32={})", err))
                };
            }

            let svc_name = to_wide_null(SEARCH_SERVICE_NAME);
            let mut can_start = true;
            let mut svc = OpenServiceW(scm, svc_name.as_ptr(), SERVICE_START | SERVICE_QUERY_STATUS);
            if svc.is_null() {
                let err = GetLastError();
                if err == ERROR_SERVICE_DOES_NOT_EXIST {
                    CloseServiceHandle(scm);
                    return ScmAttempt::AllowPortableFallback;
                }
                if err == ERROR_ACCESS_DENIED {
                    svc = OpenServiceW(scm, svc_name.as_ptr(), SERVICE_QUERY_STATUS);
                    if svc.is_null() {
                        CloseServiceHandle(scm);
                        return ScmAttempt::AllowPortableFallback;
                    }
                    can_start = false;
                } else {
                    CloseServiceHandle(scm);
                    return ScmAttempt::Unavailable(format!("OpenServiceW failed (win32={})", err));
                }
            }

            let mut status = ServiceStatusProcess {
                service_type: 0,
                current_state: 0,
                controls_accepted: 0,
                win32_exit_code: 0,
                service_specific_exit_code: 0,
                check_point: 0,
                wait_hint: 0,
                process_id: 0,
                service_flags: 0,
            };
            let mut bytes_needed: Dword = 0;
            if QueryServiceStatusEx(
                svc,
                SC_STATUS_PROCESS_INFO,
                &mut status as *mut ServiceStatusProcess as *mut u8,
                std::mem::size_of::<ServiceStatusProcess>() as Dword,
                &mut bytes_needed,
            ) == 0
            {
                let err = GetLastError();
                CloseServiceHandle(svc);
                CloseServiceHandle(scm);
                return if can_start {
                    ScmAttempt::Unavailable(format!("QueryServiceStatusEx failed (win32={})", err))
                } else {
                    ScmAttempt::AllowPortableFallback
                };
            }

            if status.current_state == SERVICE_STOPPED {
                if !can_start {
                    CloseServiceHandle(svc);
                    CloseServiceHandle(scm);
                    return ScmAttempt::AllowPortableFallback;
                }
                let mut wide_args: Vec<Vec<u16>> = Vec::new();
                if is_valid_pipe_token(&self.pipe_token) {
                    wide_args.push(to_wide_null(&format!("--pipe-token={}", self.pipe_token)));
                }
                if !self.client_sid.is_empty() {
                    wide_args.push(to_wide_null(&format!("--client-sid={}", self.client_sid)));
                }
                let ptrs: Vec<*const u16> = wide_args.iter().map(|v| v.as_ptr()).collect();
                let start_ok = StartServiceW(
                    svc,
                    ptrs.len() as Dword,
                    if ptrs.is_empty() { ptr::null() } else { ptrs.as_ptr() },
                );
                if start_ok == 0 {
                    let err = GetLastError();
                    if err != ERROR_SERVICE_ALREADY_RUNNING {
                        CloseServiceHandle(svc);
                        CloseServiceHandle(scm);
                        return ScmAttempt::AllowPortableFallback;
                    }
                }
            } else if status.current_state == SERVICE_STOP_PENDING {
                CloseServiceHandle(svc);
                CloseServiceHandle(scm);
                return ScmAttempt::Unavailable(
                    "Tools3000_SearchService is currently stopping; please retry shortly".to_string(),
                );
            }

            let deadline = Instant::now() + Duration::from_millis(SERVICE_START_HARD_DEADLINE_MS);
            while Instant::now() < deadline {
                if self.resolve_active_pipe(100).is_some() {
                    CloseServiceHandle(svc);
                    CloseServiceHandle(scm);
                    return ScmAttempt::Ready;
                }
                if QueryServiceStatusEx(
                    svc,
                    SC_STATUS_PROCESS_INFO,
                    &mut status as *mut ServiceStatusProcess as *mut u8,
                    std::mem::size_of::<ServiceStatusProcess>() as Dword,
                    &mut bytes_needed,
                ) != 0
                    && status.current_state == SERVICE_STOPPED
                {
                    CloseServiceHandle(svc);
                    CloseServiceHandle(scm);
                    return ScmAttempt::AllowPortableFallback;
                }
                thread::sleep(Duration::from_millis(SERVICE_POLL_INTERVAL_MS));
            }

            let final_state = status.current_state;
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            if final_state == SERVICE_RUNNING || final_state == SERVICE_START_PENDING {
                ScmAttempt::Unavailable(
                    "Tools3000_SearchService is running but named pipe endpoint is unavailable".to_string(),
                )
            } else {
                ScmAttempt::AllowPortableFallback
            }
        }
    }

    fn exchange_once(&self, pipe_name: &str, request_bytes: &[u8]) -> Result<JsonValue, String> {
        let header = encode_frame_header(request_bytes.len())?;
        let wide_pipe = to_wide_null(pipe_name);

        let mut h_pipe = INVALID_HANDLE_VALUE;
        for attempt in 0..8 {
            h_pipe = unsafe {
                CreateFileW(
                    wide_pipe.as_ptr(),
                    GENERIC_READ | GENERIC_WRITE,
                    0,
                    ptr::null_mut(),
                    OPEN_EXISTING,
                    0,
                    ptr::null_mut(),
                )
            };
            if h_pipe != INVALID_HANDLE_VALUE {
                break;
            }
            let err = unsafe { GetLastError() };
            if is_pipe_transient_busy(err) && attempt < 7 {
                unsafe {
                    WaitNamedPipeW(wide_pipe.as_ptr(), 400);
                }
                continue;
            }
            return Err(format!("CreateFileW({}) failed (win32={})", pipe_name, err));
        }
        if h_pipe == INVALID_HANDLE_VALUE {
            return Err(format!("could not open pipe {}", pipe_name));
        }

        let io_result = (|| -> Result<JsonValue, String> {
            write_exact_handle(h_pipe, &header)?;
            write_exact_handle(h_pipe, request_bytes)?;

            let mut resp_header = [0u8; 4];
            read_exact_handle(h_pipe, &mut resp_header)?;
            let resp_len = decode_frame_header(resp_header, MAX_RESPONSE_BYTES)?;
            let mut resp_buf = vec![0u8; resp_len];
            read_exact_handle(h_pipe, &mut resp_buf)?;

            let resp_str = String::from_utf8(resp_buf)
                .map_err(|_| "service returned non-UTF-8 response".to_string())?;
            parse_json(&resp_str).map_err(|e| format!("service returned invalid JSON: {}", e))
        })();

        unsafe {
            CloseHandle(h_pipe);
        }
        io_result
    }
}

enum ScmAttempt {
    Ready,
    AllowPortableFallback,
    Unavailable(String),
}

fn find_companion_service_exe() -> Option<PathBuf> {
    let exe = env::current_exe().ok()?;
    let dir = exe.parent()?;
    let candidate = dir.join("Tools3000_Service.exe");
    if candidate.exists() {
        return Some(candidate);
    }
    None
}

impl SearchBackend for WinPipeBackend {
    fn is_pipe_running(&self) -> bool {
        self.resolve_active_pipe(40).is_some()
    }

    fn send_request(&self, payload: &JsonValue, auto_start: bool) -> Result<JsonValue, String> {
        if auto_start {
            self.ensure_service_running()?;
        }
        let pipe = self.resolve_active_pipe(150).ok_or_else(|| {
            "Tools3000_SearchService is not running (DEMAND_START active)".to_string()
        })?;

        let raw_json = payload.to_json_string();
        let req_bytes = raw_json.as_bytes();
        let init_deadline = Instant::now() + Duration::from_millis(SERVICE_INIT_WAIT_DEADLINE_MS);

        loop {
            let resp = self.exchange_once(&pipe, req_bytes)?;
            let initializing = resp
                .get("initializing")
                .and_then(|v| v.as_bool())
                .unwrap_or(false);
            if initializing && auto_start && Instant::now() < init_deadline {
                thread::sleep(Duration::from_millis(200));
                continue;
            }
            return Ok(resp);
        }
    }
}
} // mod platform (windows)

#[cfg(not(windows))]
mod platform {
    use super::SearchBackend;
    use crate::config::LoadedCliConfig;
    use crate::json::JsonValue;

    /// 非 Windows 平台（如 Linux Docker / Glama MCP Introspection 沙箱）占位后端：
    /// 允许 `tools3000-cli mcp` 完整响应 initialize / ping / tools/list / resources/list / prompts/list，
    /// 仅在实际执行依赖 NTFS MFT 服务的 tools/call 时给出清晰的跨平台提示。
    pub struct WinPipeBackend;

    impl WinPipeBackend {
        pub fn from_config(_cfg: &LoadedCliConfig) -> Self {
            Self
        }
    }

    impl SearchBackend for WinPipeBackend {
        fn is_pipe_running(&self) -> bool {
            false
        }

        fn send_request(&self, _payload: &JsonValue, _auto_start: bool) -> Result<JsonValue, String> {
            Err("Tools3000 NTFS search service requires Windows".to_string())
        }
    }
}

pub use platform::WinPipeBackend;

#[cfg(test)]
mod tests {
    use super::*;
    use crate::constants::MAX_RESPONSE_BYTES;

    #[test]
    fn frame_header_codec_matches_cpp_contract() {
        let h = encode_frame_header(0x0102_0304.min(MAX_REQUEST_BYTES)).unwrap();
        let decoded = decode_frame_header(h, MAX_RESPONSE_BYTES).unwrap();
        assert_eq!(decoded, MAX_REQUEST_BYTES);

        let exact = encode_frame_header(513).unwrap();
        assert_eq!(exact, [0x01, 0x02, 0x00, 0x00]);
        assert_eq!(decode_frame_header(exact, MAX_RESPONSE_BYTES).unwrap(), 513);

        assert!(encode_frame_header(0).is_err());
        assert!(encode_frame_header(MAX_REQUEST_BYTES + 1).is_err());
        assert!(decode_frame_header([0, 0, 0, 0], MAX_RESPONSE_BYTES).is_err());
        assert!(decode_frame_header(u32::MAX.to_le_bytes(), MAX_RESPONSE_BYTES).is_err());
    }
}
