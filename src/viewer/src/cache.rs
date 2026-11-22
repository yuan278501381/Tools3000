// ─────────────────────────────────────────────────────────────────────────────
// cache.rs — 多线程异步预解码工作池 + 预算制 LRU 全尺寸缓存 + 胶片缩略图缓存
// ─────────────────────────────────────────────────────────────────────────────

use crate::constants::{
    MAX_DECODE_CACHE_BYTES, MAX_DECODE_CACHE_ENTRIES, MAX_PRELOAD_SINGLE_ENTRY_BYTES,
    MAX_THUMB_CACHE_ENTRIES, PRELOAD_RADIUS, THUMB_TARGET_SIZE, WORKER_THREAD_COUNT,
};
use crate::decode::{decode_image_file, decode_thumbnail};
use crate::types::{DecodeError, SharedImage, ThumbnailBitmap};
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
    full_errors: HashMap<PathBuf, DecodeError>,
    full_bytes: usize,
    active_path: Option<PathBuf>,
    thumb_cache: HashMap<PathBuf, ThumbEntry>,
    thumb_errors: HashSet<PathBuf>,
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
                active_path: None,
                thumb_cache: HashMap::new(),
                thumb_errors: HashSet::new(),
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
    pub fn get_cached_image(&self, path: &Path) -> Option<Result<SharedImage, DecodeError>> {
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

        guard.active_path = Some(current.clone());
        // 切换当前图后顺手清理超过预算的历史驻留超大图
        evict_full_lru_locked(&mut guard, current);

        // 1. 当前图最高优先级插队到队首
        enqueue_full_locked(&mut guard, current.clone(), true);

        // 2. 前后邻居按距离递增预解码（+1, -1, +2, -2），实现 Picasa 招牌丝滑连翻
        if siblings.len() > 1 {
            for dist in 1..=PRELOAD_RADIUS {
                let next_idx = (current_index + dist) % siblings.len();
                enqueue_full_locked(&mut guard, siblings[next_idx].clone(), false);
                let prev_idx =
                    (current_index + siblings.len() - (dist % siblings.len())) % siblings.len();
                if prev_idx != next_idx {
                    enqueue_full_locked(&mut guard, siblings[prev_idx].clone(), false);
                }
            }
        }
        cv.notify_all();
    }

    /// 启动早期（渲染器创建前）静默预热首张缩略图，不拷贝像素
    pub fn prefetch_thumb(&self, path: &Path) {
        let (lock, cv) = &*self.state;
        let Ok(mut guard) = lock.lock() else {
            return;
        };
        if guard.thumb_cache.contains_key(path)
            || guard.thumb_errors.contains(path)
            || guard.pending_thumb.contains(path)
        {
            return;
        }
        guard.pending_thumb.insert(path.to_path_buf());
        guard.queue.push_back(DecodeTask::Thumbnail {
            path: path.to_path_buf(),
        });
        cv.notify_one();
    }

    /// 取出待上传 GPU 的缩略图并立刻从 CPU 侧 `thumb_cache` 移除像素缓冲，彻底消除 CPU + GPU 双份常驻；
    /// 若尚未就绪且未标记损坏，则自动投递低优先级异步缩略图解码任务
    pub fn take_thumb_for_upload(&self, path: &Path) -> Option<ThumbnailBitmap> {
        let (lock, cv) = &*self.state;
        let mut guard = lock.lock().ok()?;
        if let Some(entry) = guard.thumb_cache.remove(path) {
            return Some(entry.thumb);
        }
        if guard.thumb_errors.contains(path) {
            return None;
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

    /// 静态当前图成功上传至 GPU `ID2D1Bitmap1` 后，立即从 CPU 全图缓存中释放该像素副本（保留 active_path）
    pub fn release_full_cache_entry(&self, path: &Path) {
        let (lock, _) = &*self.state;
        if let Ok(mut guard) = lock.lock() {
            release_full_cache_entry_locked(&mut guard, path);
        }
    }

    /// 判断某张缩略图是否已确认解码失败（用于胶片带渲染原版经典暗红无效微标且避免重复重试）
    pub fn is_thumb_failed(&self, path: &Path) -> bool {
        let (lock, _) = &*self.state;
        lock.lock()
            .map(|g| g.thumb_errors.contains(path) || g.full_errors.contains_key(path))
            .unwrap_or(false)
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
            guard.thumb_errors.remove(path);
            if guard.active_path.as_deref() == Some(path) {
                guard.active_path = None;
            }
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

fn release_full_cache_entry_locked(state: &mut SharedState, path: &Path) {
    if let Some(removed) = state.full_cache.remove(path) {
        state.full_bytes = state.full_bytes.saturating_sub(removed.bytes);
    }
}

/// 将新解码结果纳入预算制缓存：
/// - 非当前图的超大后台预解码条目（> 18MB）不入长期缓存，防止冲垮 36MB 预算并挤掉当前图；
/// - 当前图即使单张超大，也会先清空全部非当前条目并仅保留此当前图，一旦切图或上传 GPU 即被立刻回收。
fn insert_full_with_budget_locked(state: &mut SharedState, path: PathBuf, shared: SharedImage, bytes: usize) {
    let is_active = state.active_path.as_deref() == Some(path.as_path());
    if !is_active && bytes > MAX_PRELOAD_SINGLE_ENTRY_BYTES {
        return;
    }

    if is_active && bytes > MAX_DECODE_CACHE_BYTES {
        // 当前超大单图独占缓存，先彻底释放其余条目确保内存峰值收敛
        state.full_cache.clear();
        state.full_bytes = 0;
    }

    state.lru_tick += 1;
    let tick = state.lru_tick;
    if let Some(old) = state.full_cache.insert(
        path.clone(),
        CacheEntry {
            image: shared,
            bytes,
            tick,
        },
    ) {
        state.full_bytes = state.full_bytes.saturating_sub(old.bytes);
    }
    state.full_bytes = state.full_bytes.saturating_add(bytes);
    state.full_errors.remove(&path);

    let protected = state
        .active_path
        .clone()
        .unwrap_or_else(|| path.clone());
    evict_full_lru_locked(state, &protected);
}

fn evict_full_lru_locked(state: &mut SharedState, protected: &Path) {
    while state.full_bytes > MAX_DECODE_CACHE_BYTES
        || state.full_cache.len() > MAX_DECODE_CACHE_ENTRIES
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
                            insert_full_with_budget_locked(&mut guard, path, shared, bytes);
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
                        guard.thumb_errors.remove(&path);
                        guard
                            .thumb_cache
                            .insert(path, ThumbEntry { thumb: t, tick });
                        evict_thumb_lru_locked(&mut guard);
                    } else {
                        guard.thumb_errors.insert(path);
                    }
                    guard.notify_hwnd
                };
                post_ui_signal(notify_hwnd, WM_VIEWER_THUMB_READY);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::{DecodedImage, ImageFormatKind, ImageFrame};

    fn dummy_image(path: &str) -> SharedImage {
        Arc::new(DecodedImage {
            path: PathBuf::from(path),
            format_kind: ImageFormatKind::Static("PNG".to_string()),
            file_size_bytes: 1024,
            width: 1,
            height: 1,
            has_alpha: false,
            frames: vec![ImageFrame {
                width: 1,
                height: 1,
                delay_ms: 0,
                bgra_premul: vec![0, 0, 0, 255],
            }],
            total_duration_ms: 0,
            exif_summary: Vec::new(),
        })
    }

    fn empty_state() -> SharedState {
        SharedState {
            full_cache: HashMap::new(),
            full_errors: HashMap::new(),
            full_bytes: 0,
            active_path: None,
            thumb_cache: HashMap::new(),
            thumb_errors: HashSet::new(),
            pending_full: HashSet::new(),
            pending_thumb: HashSet::new(),
            queue: VecDeque::new(),
            lru_tick: 0,
            notify_hwnd: 0,
        }
    }

    #[test]
    fn oversized_background_preload_is_rejected_and_active_image_is_protected() {
        let mut st = empty_state();
        let cur = PathBuf::from("cur.png");
        let bg_huge = PathBuf::from("bg_huge.png");
        st.active_path = Some(cur.clone());

        insert_full_with_budget_locked(&mut st, cur.clone(), dummy_image("cur.png"), 16 * 1024 * 1024);
        // 超过 18MB 单图上限的后台预解码超大图不得挤入长期缓存或踢掉当前图
        insert_full_with_budget_locked(
            &mut st,
            bg_huge.clone(),
            dummy_image("bg_huge.png"),
            MAX_PRELOAD_SINGLE_ENTRY_BYTES + 1024,
        );
        assert!(st.full_cache.contains_key(&cur));
        assert!(!st.full_cache.contains_key(&bg_huge));
        assert!(st.full_bytes <= MAX_DECODE_CACHE_BYTES);
    }

    #[test]
    fn oversized_active_image_evicts_all_other_entries_and_yields_on_switch() {
        let mut st = empty_state();
        let prev = PathBuf::from("prev.png");
        let huge_cur = PathBuf::from("huge_cur.png");
        let next = PathBuf::from("next.png");

        st.active_path = Some(prev.clone());
        insert_full_with_budget_locked(&mut st, prev.clone(), dummy_image("prev.png"), 12 * 1024 * 1024);

        st.active_path = Some(huge_cur.clone());
        insert_full_with_budget_locked(
            &mut st,
            huge_cur.clone(),
            dummy_image("huge_cur.png"),
            48 * 1024 * 1024,
        );
        // 当前超大单图独占缓存，历史条目立即被清空
        assert_eq!(st.full_cache.len(), 1);
        assert!(st.full_cache.contains_key(&huge_cur));

        // 切换到下一张图时，原超大图立即被 LRU 回收，预算恢复正常
        st.active_path = Some(next.clone());
        evict_full_lru_locked(&mut st, &next);
        assert!(!st.full_cache.contains_key(&huge_cur));
        assert_eq!(st.full_bytes, 0);
    }

    #[test]
    fn compact_budget_enforces_36mb_3_entries_and_releases_static_cpu_pixels() {
        let mut st = empty_state();
        let p0 = PathBuf::from("a.jpg");
        let p1 = PathBuf::from("b.jpg");
        let p2 = PathBuf::from("c.jpg");
        let p3 = PathBuf::from("d.jpg");
        st.active_path = Some(p3.clone());

        for p in [&p0, &p1, &p2, &p3] {
            insert_full_with_budget_locked(&mut st, p.clone(), dummy_image("x.jpg"), 10 * 1024 * 1024);
        }
        // 总条目严守 <= MAX_DECODE_CACHE_ENTRIES (3) 且总字节 <= MAX_DECODE_CACHE_BYTES (36MB)
        assert!(st.full_cache.len() <= MAX_DECODE_CACHE_ENTRIES);
        assert!(st.full_bytes <= MAX_DECODE_CACHE_BYTES);
        assert!(st.full_cache.contains_key(&p3));
        assert!(!st.full_cache.contains_key(&p0));

        // 静态当前图上传至 GPU 后主动释放 CPU 像素副本，计数精准扣减
        let before = st.full_bytes;
        release_full_cache_entry_locked(&mut st, &p3);
        assert!(!st.full_cache.contains_key(&p3));
        assert_eq!(st.full_bytes, before - 10 * 1024 * 1024);
        assert_eq!(st.active_path.as_deref(), Some(p3.as_path()));
    }

    #[test]
    fn take_thumb_for_upload_removes_cpu_copy_to_prevent_double_residency() {
        let pool = ImageCachePool::new();
        let path = PathBuf::from("sample_thumb.jpg");
        {
            let (lock, _) = &*pool.state;
            let mut guard = lock.lock().unwrap();
            guard.thumb_cache.insert(
                path.clone(),
                ThumbEntry {
                    thumb: ThumbnailBitmap {
                        width: 128,
                        height: 96,
                        orig_width: 1024,
                        orig_height: 768,
                        bgra_premul: vec![255; 128 * 96 * 4],
                    },
                    tick: 1,
                },
            );
        }
        // 首次取出命中并返回像素缓冲，同时从 CPU thumb_cache 立即移除
        let taken = pool.take_thumb_for_upload(&path);
        assert!(taken.is_some());
        let (lock, _) = &*pool.state;
        let guard = lock.lock().unwrap();
        assert!(!guard.thumb_cache.contains_key(&path));
    }
}
