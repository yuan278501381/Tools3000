#pragma once
// Memory-mapped file views can raise synchronous hardware exceptions
// (EXCEPTION_IN_PAGE_ERROR / EXCEPTION_ACCESS_VIOLATION) if the underlying disk,
// removable drive, or remote share vanishes or truncates the file mid-read.
//
// CRITICAL (/EHsc Zero-Leak Rule):
// Under MSVC /EHsc, a hardware exception caught by `__except` does NOT run C++
// destructors, zlib `inflateEnd`, or Rust `Drop` handlers on inner stack frames.
// Therefore:
// 1. Any parser that constructs C++/zlib/Rust RAII objects MUST first copy or slice
//    mapped pages into heap memory via `copyMappedViewSafe` / `safeMappedMemcpyEx`
//    (whose `__try` body contains ONLY raw `std::memcpy` with zero destructors).
// 2. `runWithMappedViewGuard` MUST ONLY wrap pure pointer/scalar operations that do
//    not construct heap-owning RAII objects inside the guarded call chain.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace tools3000::service {

namespace detail {

inline bool isMappedViewFault(unsigned long code) noexcept {
    return code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_ACCESS_VIOLATION;
}

using GuardedActionFn = bool (*)(void* ctx);

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4509)
#endif
inline bool runWithMappedViewGuardImpl(
    GuardedActionFn fn,
    void* ctx,
    bool* outInPageError
) noexcept {
    if (outInPageError) {
        *outInPageError = false;
    }
    __try {
        return fn(ctx);
    } __except (isMappedViewFault(GetExceptionCode())
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        if (outInPageError) {
            *outInPageError = true;
        }
        return false;
    }
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace detail

template <typename Fn>
bool runWithMappedViewGuard(Fn&& fn, bool* outInPageError = nullptr) {
    auto invoker = [](void* rawCtx) -> bool {
        auto* callable = static_cast<Fn*>(rawCtx);
        return (*callable)();
    };
    return detail::runWithMappedViewGuardImpl(invoker, &fn, outInPageError);
}

inline bool safeMappedMemcpyEx(
    void* dst,
    const void* src,
    size_t byteCount,
    bool* outInPageError = nullptr
) noexcept {
    if (outInPageError) {
        *outInPageError = false;
    }
    if (byteCount == 0) {
        return true;
    }
    if (!dst || !src) {
        return false;
    }
    __try {
        std::memcpy(dst, src, byteCount);
        return true;
    } __except (detail::isMappedViewFault(GetExceptionCode())
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        if (outInPageError) {
            *outInPageError = true;
        }
        return false;
    }
}

inline bool safeMappedMemcpy(void* dst, const void* src, size_t byteCount) noexcept {
    return safeMappedMemcpyEx(dst, src, byteCount, nullptr);
}

/// Safely copies a memory-mapped view into a heap-allocated `outBuffer` in 256 KB
/// chunks under pure-POD SEH protection. All dynamic allocation occurs OUTSIDE `__try`,
/// guaranteeing zero memory leaks under `/EHsc` even if `EXCEPTION_IN_PAGE_ERROR` fires.
inline bool copyMappedViewSafe(
    const uint8_t* mappedSrc,
    size_t byteCount,
    std::vector<uint8_t>& outBuffer,
    bool& outInPageError,
    const std::function<bool()>& isCancelled = {}
) noexcept {
    outInPageError = false;
    outBuffer.clear();
    if (byteCount == 0) {
        return true;
    }
    if (!mappedSrc) {
        return false;
    }

    try {
        outBuffer.resize(byteCount);
    } catch (...) {
        outBuffer.clear();
        return false;
    }

    constexpr size_t CopyChunkBytes = 256u * 1024u;
    size_t offset = 0;
    while (offset < byteCount) {
        if (isCancelled) {
            try {
                if (isCancelled()) {
                    outBuffer.clear();
                    return false;
                }
            } catch (...) {
                outBuffer.clear();
                return false;
            }
        }
        const size_t chunk = (std::min)(byteCount - offset, CopyChunkBytes);
        bool chunkFault = false;
        if (!safeMappedMemcpyEx(outBuffer.data() + offset, mappedSrc + offset, chunk, &chunkFault)) {
            outInPageError = chunkFault;
            outBuffer.clear();
            outBuffer.shrink_to_fit();
            return false;
        }
        offset += chunk;
    }
    return true;
}

} // namespace tools3000::service
