#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <cstring>
#include <type_traits>

namespace tools3000::service {

namespace detail {

// Pure POD C-style SEH boundary so MSVC /EHsc /W4 never emits C2712 or C4509
// when callers hold C++ RAII objects in outer frames.
inline bool invokeWithInPageGuardRaw(bool (*fn)(void*) noexcept, void* ctx) noexcept {
#ifdef _MSC_VER
    __try {
        return fn(ctx);
    } __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
#else
    return fn(ctx);
#endif
}

} // namespace detail

inline bool safeMappedMemcpy(void* dst, const void* src, size_t byteCount) noexcept {
    if (byteCount == 0) {
        return true;
    }
    if (!dst || !src) {
        return false;
    }
#ifdef _MSC_VER
    __try {
        std::memcpy(dst, src, byteCount);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
#else
    std::memcpy(dst, src, byteCount);
    return true;
#endif
}

template <typename Fn>
bool runWithMappedViewGuard(Fn&& fn) noexcept {
    using FnType = std::remove_reference_t<Fn>;
    struct CallContext {
        FnType* fnPtr;
        static bool invoke(void* raw) noexcept {
            auto* self = static_cast<CallContext*>(raw);
            try {
                if constexpr (std::is_void_v<decltype((*self->fnPtr)())>) {
                    (*self->fnPtr)();
                    return true;
                } else {
                    return static_cast<bool>((*self->fnPtr)());
                }
            } catch (...) {
                return false;
            }
        }
    } ctx{&fn};

    return detail::invokeWithInPageGuardRaw(&CallContext::invoke, &ctx);
}

} // namespace tools3000::service
