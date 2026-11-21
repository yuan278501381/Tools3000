// ─────────────────────────────────────────────────────────────────────────────
// cache.rs — 多线程异步预解码工作池 + 预算制 LRU 全尺寸缓存 + 胶片缩略图缓存
// ─────────────────────────────────────────────────────────────────────────────

use crate::constants::{
    MAX_DECODE_CACHE_BYTES, MAX_DECODE_CACHE_ENTRIES, MAX_THUMB_CACHE_ENTRIES, PRELOAD_RADIUS,
    THUMB_TARGET_SIZE, WORKER_THREAD_COUNT,
};
use crate::decode::{decode_image_file, decode_thumbnail};
use crate::types::{SharedImage, ThumbnailBitmap};
use std::collections::{HashMap, HashSet, VecDeque};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread;
use windows::Win32::Foundation::{HWND, LPARAM, WPARAM};
use windows::Win32::UI::WindowsAndMessaging::{PostMessageW, WM_APP};

/// 后台解码完成通知消息（唤醒 UI 线程立即重绘）
pub const WM_VIEWER_IMAGE_READY: u32 = WM_APP + 101;
pub const WM_VIEWER_THUMB_READY: u32 = WM_APP + 102;

#[derive(Clone, Debug)]
enum DecodeTask {
    FullImage { path: PathBuf },
    Thumbnail { path: PathBuf },
}

struct CacheEntry {
    image: SharedImage,
    bytes: usize,
    tick: u64,
}

struct ThumbEntry {
    thumb: ThumbnailBitmap,
    tick: u64,
}

struct SharedState {
    full_cache: HashMap<PathBuf, CacheEntry>,
    full_errors: HashMap<PathBuf, String>,
    full_bytes: usize,
    thumb_cache: HashMap<PathBuf, ThumbEntry>,
    pending_full: HashSet<PathBuf>,
    pending_thumb: HashSet<PathBuf>,
    queue: VecDeque<DecodeTask>,
    lru_tick: u64,
    notify_hwnd: isize,
}

pub struct ImageCachePool {
    state: Arc<(Mutex<SharedState>, Condvar)>,
    shutdown: Arc<AtomicBool>,
}

impl ImageCachePool {
    pub fn new() -> Self {
        let state = Arc::new((
            Mutex::new(SharedState {
                full_cache: HashMap::new(),
                full_errors: HashMap::new(),
                full_bytes: 0,
                thumb_cache: HashMap::new(),
                pending_full: HashSet::new(),
                pending_thumb: HashSet::new(),
                queue: VecDeque::new(),
                lru_tick: 0,
                notify_hwnd: 0,
            }),
            Condvar::new(),
        ));
        let shutdown = Arc::new(AtomicBool::new(false));

        for idx in 0..WORKER_THREAD_COUNT {
            let st = Arc::clone(&state);
            let sd = Arc::clone(&shutdown);
            let _ = thread::Builder::new()
                .name(format!("t3k-viewer-decode-{idx}"))
                .spawn(move || worker_loop(st, sd));
        }

        Self { state, shutdown }
    }

    /// 窗口开始关闭动画时立即静默中止后台预解码排队，保障退出动画 0 CPU 争抢与进程瞬时退场
    pub fn begin_shutdown(&self) {
        self.shutdown.store(true, Ordering::Release);
        let (lock, cv) = &*self.state;
        if let Ok(mut guard) = lock.lock() {
            guard.notify_hwnd = 0;
            guard.queue.clear();
        }
        cv.notify_all();
    }

    pub fn set_notify_hwnd(&self, hwnd: HWND) {
        let (lock, _) = &*self.state;
        if let Ok(mut guard) = lock.lock() {
            guard.notify_hwnd = hwnd.0 as isize;
        }
    }

    /// 同步查缓存；命中则刷新 LRU 计数直接零延迟返回
    pub fn get_cached_image(&self, path: &Path) -> Option<Result<SharedImage, String>> {
        let (lock, _) = &*self.state;
        let mut guard = lock.lock().ok()?;
        if let Some(err) = guard.full_errors.get(path) {
            return Some(Err(err.clone()));
        }
        guard.lru_tick += 1;
        let tick = guard.lru_tick;
        if let Some(entry) = guard.full_cache.get_mut(path) {
            entry.tick = tick;
            return Some(Ok(Arc::clone(&entry.image)));
        }
        None
    }

    /// 请求高优先级解码当前图，并自动提交前后邻居图的静默预解码任务
    pub fn request_image_and_neighbors(&self, siblings: &[PathBuf], current_index: usize) {
        let Some(current) = siblings.get(current_index) else {
            return;
        };
        let (lock, cv) = &*self.state;
        let Ok(mut guard) = lock.lock() else {
            return;
        };

        // 1. 当前图最高优先级插队到队首
        enqueue_full_locked(&mut guard, current.clone(), true);

        // 2. 前后邻居按距离递增预解码（+1, -1, +2, -2），实现 Picasa 招牌丝滑连翻
        if siblings.len() > 1 {
            for dist in 1..=PRELOAD_RADIUS {
                let next_idx = (current_index + dist) % siblings.len();
                enqueue_full_locked(&mut guard, siblings[next_idx].clone(), false);
                let prev_idx = (current_index + siblings.len() - (dist % siblings.len())) % siblings.len();
                if prev_idx != next_idx {
                    enqueue_full_locked(&mut guard, siblings[prev_idx].clone(), false);
                }
            }
        }
        cv.notify_all();
    }

    /// 查询缩略图缓存；未命中自动投递低优先级异步缩略图生成任务
    pub fn get_or_request_thumb(&self, path: &Path) -> Option<ThumbnailBitmap> {
        let (lock, cv) = &*self.state;
        let mut guard = lock.lock().ok()?;
        guard.lru_tick += 1;
        let tick = guard.lru_tick;
        if let Some(entry) = guard.thumb_cache.get_mut(path) {
            entry.tick = tick;
            return Some(entry.thumb.clone());
        }
        if !guard.pending_thumb.contains(path) {
            guard.pending_thumb.insert(path.to_path_buf());
            guard.queue.push_back(DecodeTask::Thumbnail {
                path: path.to_path_buf(),
            });
            cv.notify_one();
        }
        None
    }

    /// 文件被删除或移动后使对应缓存条目失效
    pub fn invalidate_path(&self, path: &Path) {
        let (lock, _) = &*self.state;
        if let Ok(mut guard) = lock.lock() {
            if let Some(removed) = guard.full_cache.remove(path) {
                guard.full_bytes = guard.full_bytes.saturating_sub(removed.bytes);
            }
            guard.full_errors.remove(path);
            guard.thumb_cache.remove(path);
        }
    }
}

impl Drop for ImageCachePool {
    fn drop(&mut self) {
        // 仅通知停止拉取新任务，不阻塞主进程等待正在执行的纯只读解码任务（杜绝关窗后残留后台进程）
        self.begin_shutdown();
    }
}

fn enqueue_full_locked(state: &mut SharedState, path: PathBuf, high_priority: bool) {
    if state.full_cache.contains_key(&path)
        || state.full_errors.contains_key(&path)
        || state.pending_full.contains(&path)
    {
        return;
    }
    state.pending_full.insert(path.clone());
    let task = DecodeTask::FullImage { path };
    if high_priority {
        state.queue.push_front(task);
    } else {
        state.queue.push_back(task);
    }
}

fn evict_full_lru_locked(state: &mut SharedState, protected: &Path) {
    while (state.full_bytes > MAX_DECODE_CACHE_BYTES
        || state.full_cache.len() > MAX_DECODE_CACHE_ENTRIES)
        && state.full_cache.len() > 1
    {
        let victim = state
            .full_cache
            .iter()
            .filter(|(k, _)| k.as_path() != protected)
            .min_by_key(|(_, v)| v.tick)
            .map(|(k, _)| k.clone());
        let Some(victim_key) = victim else {
            break;
        };
        if let Some(removed) = state.full_cache.remove(&victim_key) {
            state.full_bytes = state.full_bytes.saturating_sub(removed.bytes);
        }
    }
}

fn evict_thumb_lru_locked(state: &mut SharedState) {
    while state.thumb_cache.len() > MAX_THUMB_CACHE_ENTRIES {
        let victim = state
            .thumb_cache
            .iter()
            .min_by_key(|(_, v)| v.tick)
            .map(|(k, _)| k.clone());
        if let Some(k) = victim {
            state.thumb_cache.remove(&k);
        } else {
            break;
        }
    }
}

fn post_ui_signal(hwnd_raw: isize, msg: u32) {
    if hwnd_raw == 0 {
        return;
    }
    unsafe {
        let _ = PostMessageW(HWND(hwnd_raw as *mut _), msg, WPARAM(0), LPARAM(0));
    }
}

fn worker_loop(state: Arc<(Mutex<SharedState>, Condvar)>, shutdown: Arc<AtomicBool>) {
    let (lock, cv) = &*state;
    loop {
        let task = {
            let mut guard = match lock.lock() {
                Ok(g) => g,
                Err(_) => return,
            };
            while guard.queue.is_empty() && !shutdown.load(Ordering::Acquire) {
                guard = match cv.wait(guard) {
                    Ok(g) => g,
                    Err(_) => return,
                };
            }
            if shutdown.load(Ordering::Acquire) {
                return;
            }
            guard.queue.pop_front()
        };

        let Some(task) = task else { continue };
        match task {
            DecodeTask::FullImage { path } => {
                let decoded = decode_image_file(&path);
                let notify_hwnd = {
                    let Ok(mut guard) = lock.lock() else {
                        return;
                    };
                    guard.pending_full.remove(&path);
                    match decoded {
                        Ok(img) => {
                            let bytes = img.memory_bytes();
                            let shared = Arc::new(img);
                            guard.lru_tick += 1;
                            let tick = guard.lru_tick;
                            if let Some(old) = guard.full_cache.insert(
                                path.clone(),
                                CacheEntry {
                                    image: shared,
                                    bytes,
                                    tick,
                                },
                            ) {
                                guard.full_bytes = guard.full_bytes.saturating_sub(old.bytes);
                            }
                            guard.full_bytes += bytes;
                            guard.full_errors.remove(&path);
                            evict_full_lru_locked(&mut guard, &path);
                        }
                        Err(e) => {
                            guard.full_errors.insert(path.clone(), e);
                        }
                    }
                    guard.notify_hwnd
                };
                post_ui_signal(notify_hwnd, WM_VIEWER_IMAGE_READY);
            }
            DecodeTask::Thumbnail { path } => {
                let thumb = decode_thumbnail(&path, THUMB_TARGET_SIZE);
                let notify_hwnd = {
                    let Ok(mut guard) = lock.lock() else {
                        return;
                    };
                    guard.pending_thumb.remove(&path);
                    if let Some(t) = thumb {
                        guard.lru_tick += 1;
                        let tick = guard.lru_tick;
                        guard.thumb_cache.insert(path, ThumbEntry { thumb: t, tick });
                        evict_thumb_lru_locked(&mut guard);
                    }
                    guard.notify_hwnd
                };
                post_ui_signal(notify_hwnd, WM_VIEWER_THUMB_READY);
            }
        }
    }
}
