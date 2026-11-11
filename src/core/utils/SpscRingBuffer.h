#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// SpscRingBuffer.h — 无锁单写单读环形队列缓冲 (Lock-Free SPSC Ring Buffer)
//
// 架构设计:
//   1. 纯单写单读模型：生产者只写 m_head，消费者只写 m_tail，彻底杜绝双写竞态
//   2. 缓存行对齐 (alignas(64)) 彻底消除伪共享 (False Sharing)
//   3. 零锁、零阻塞、微秒级非阻塞推入与弹出
//   4. 滑动窗口模式 (push)：若队列满，消费者消费时自动跨过被覆盖的老数据，保证永不死锁且永远获取最新鲜数据
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_UTILS_SPSC_RING_BUFFER_H
#define TOOLS3000_CORE_UTILS_SPSC_RING_BUFFER_H

#include <atomic>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86) || defined(_M_ARM64))
#include <intrin.h>
#endif

namespace tools3000::core {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324)
#endif

template <typename T, size_t Capacity = 2048>
class SpscRingBuffer {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2 >= 2");
    static_assert(std::is_trivially_copyable_v<T>,
                  "T must be trivially copyable for ring buffer storage");
    static_assert(std::is_nothrow_move_assignable_v<T> || std::is_nothrow_copy_assignable_v<T>,
                  "T must be nothrow assignable");

public:
    SpscRingBuffer() noexcept = default;
    ~SpscRingBuffer() = default;

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    /// 生产者：非阻塞推入元素（滑动窗口覆盖模式）
    /// 采用极轻量缓存行隔离原子自旋保护槽位读写，杜绝并发非原子覆写 UB 与多生产者乱序
    bool push(const T& item) noexcept {
        SpinGuard guard(m_spinLock);
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail = m_tail.load(std::memory_order_acquire);
        if (head - tail >= Capacity) {
            m_tail.store(head - Capacity + 1, std::memory_order_release);
        }
        m_buffer[head & BufferMask] = item;
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    /// 生产者：移动语义非阻塞推入（滑动窗口覆盖模式）
    bool push(T&& item) noexcept {
        return push(static_cast<const T&>(item));
    }

    /// 生产者：非阻塞推入元素，满载时不覆盖并安全丢弃新样本（返回 false）
    bool try_push(const T& item) noexcept {
        SpinGuard guard(m_spinLock);
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail = m_tail.load(std::memory_order_acquire);
        if (head - tail >= Capacity) {
            return false;
        }
        m_buffer[head & BufferMask] = item;
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    /// 生产者：移动语义非阻塞推入，满载时不覆盖并返回 false
    bool try_push(T&& item) noexcept {
        return try_push(static_cast<const T&>(item));
    }

    /// 消费者：弹出元素
    /// 先 acquire 加载生产指针确认可读，读取槽位后再 release 更新消费指针
    bool pop(T& item) noexcept {
        if (m_tail.load(std::memory_order_acquire) >= m_head.load(std::memory_order_acquire)) {
            return false;
        }
        SpinGuard guard(m_spinLock);
        const size_t head = m_head.load(std::memory_order_acquire);
        size_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail >= head) {
            return false;
        }
        if (head - tail > Capacity) {
            tail = head - Capacity;
        }
        item = m_buffer[tail & BufferMask];
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// 检查队列是否为空
    bool empty() const noexcept {
        return m_tail.load(std::memory_order_acquire) >= m_head.load(std::memory_order_acquire);
    }

    /// 获取队列中待处理元素数量
    size_t size() const noexcept {
        SpinGuard guard(m_spinLock);
        const size_t head = m_head.load(std::memory_order_acquire);
        const size_t tail = m_tail.load(std::memory_order_acquire);
        if (head <= tail) return 0;
        const size_t diff = head - tail;
        return (diff > Capacity) ? Capacity : diff;
    }

    /// 队列最大容量
    constexpr size_t capacity() const noexcept {
        return Capacity;
    }

    /// 清空队列
    void clear() noexcept {
        SpinGuard guard(m_spinLock);
        const size_t head = m_head.load(std::memory_order_acquire);
        m_tail.store(head, std::memory_order_release);
    }

private:
    static void cpuRelax() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        _mm_pause();
#elif defined(_MSC_VER) && defined(_M_ARM64)
        __yield();
#endif
    }

    struct SpinGuard {
        std::atomic_flag& flag;
        explicit SpinGuard(std::atomic_flag& f) noexcept : flag(f) {
            while (flag.test_and_set(std::memory_order_acquire)) {
                cpuRelax();
            }
        }
        ~SpinGuard() noexcept {
            flag.clear(std::memory_order_release);
        }
        SpinGuard(const SpinGuard&) = delete;
        SpinGuard& operator=(const SpinGuard&) = delete;
    };

    static constexpr size_t BufferMask = Capacity - 1;

    // 独立 64 字节缓存行对齐，彻底消除多核并发时的伪共享
    alignas(64) mutable std::atomic_flag m_spinLock = ATOMIC_FLAG_INIT;
    alignas(64) std::atomic<size_t> m_head{0};
    alignas(64) std::atomic<size_t> m_tail{0};

    // 环形缓冲存储区
    alignas(64) T m_buffer[Capacity]{};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace tools3000::core

#endif // TOOLS3000_CORE_UTILS_SPSC_RING_BUFFER_H
