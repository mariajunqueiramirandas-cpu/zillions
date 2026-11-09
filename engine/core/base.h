// Fundamental types and macros shared by every engine module.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace zl {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;

#if defined(__GNUC__) || defined(__clang__)
#define ZL_LIKELY(x) __builtin_expect(!!(x), 1)
#define ZL_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define ZL_FORCE_INLINE inline __attribute__((always_inline))
#else
#define ZL_LIKELY(x) (x)
#define ZL_UNLIKELY(x) (x)
#define ZL_FORCE_INLINE __forceinline
#endif

[[noreturn]] inline void panic(const char* file, int line, const char* msg) {
    std::fprintf(stderr, "[zillions] fatal: %s (%s:%d)\n", msg, file, line);
    std::abort();
}

// ZL_CHECK is always on and guards conditions that depend on the outside world.
// ZL_ASSERT guards programmer errors and compiles away in release builds.
#define ZL_CHECK(cond, msg)                                                    \
    do {                                                                       \
        if (ZL_UNLIKELY(!(cond))) ::zl::panic(__FILE__, __LINE__, msg);        \
    } while (0)

#ifdef NDEBUG
#define ZL_ASSERT(cond) ((void)0)
#else
#define ZL_ASSERT(cond) ZL_CHECK(cond, "assertion failed: " #cond)
#endif

#define ZL_LOG(...)                                                            \
    do {                                                                       \
        std::fprintf(stderr, "[zillions] " __VA_ARGS__);                       \
        std::fputc('\n', stderr);                                              \
    } while (0)

}  // namespace zl
