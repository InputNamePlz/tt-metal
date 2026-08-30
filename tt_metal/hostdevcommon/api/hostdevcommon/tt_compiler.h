// SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Portable spellings for compiler-specific attributes and intrinsics used by code shared
// between host and device. On GCC/Clang (including the RISC-V device toolchain) everything
// expands to the exact GNU construct previously spelled inline, so device codegen and host
// GCC/Clang builds are unchanged; MSVC gets an equivalent or a conservative fallback.

#include <cstdint>

#if defined(__GNUC__) || defined(__clang__)
// Expands to `inline __attribute__((always_inline))` -- use in place of that exact spelling.
#define TT_FORCE_INLINE inline __attribute__((always_inline))
#define TT_ATTR_NONNULL(...) __attribute__((nonnull(__VA_ARGS__)))
#else
#define TT_FORCE_INLINE __forceinline
// nonnull is a diagnostics/optimization hint only; MSVC has no equivalent taking indices.
#define TT_ATTR_NONNULL(...)
#endif

namespace tt::compiler {

#if defined(__GNUC__) || defined(__clang__)
constexpr int popcount32(std::uint32_t v) { return __builtin_popcount(v); }
// Undefined for v == 0, matching __builtin_clz.
constexpr int count_leading_zeros32(std::uint32_t v) { return __builtin_clz(v); }
// Undefined for v == 0, matching __builtin_ctz.
constexpr int count_trailing_zeros32(std::uint32_t v) { return __builtin_ctz(v); }
// Undefined for v == 0, matching __builtin_clzll / __builtin_ctzll.
constexpr int count_leading_zeros64(std::uint64_t v) { return __builtin_clzll(v); }
constexpr int count_trailing_zeros64(std::uint64_t v) { return __builtin_ctzll(v); }
#else
// Constexpr-capable fallbacks (MSVC's <intrin.h> popcnt/bitscan intrinsics are not constexpr,
// and device-shared headers are C++17 so std::popcount/std::countl_zero are unavailable).
constexpr int popcount32(std::uint32_t v) {
    int n = 0;
    while (v != 0) {
        v &= v - 1;
        ++n;
    }
    return n;
}
constexpr int count_leading_zeros32(std::uint32_t v) {
    int n = 0;
    for (std::uint32_t bit = 0x80000000u; bit != 0 && (v & bit) == 0; bit >>= 1) {
        ++n;
    }
    return n;
}
constexpr int count_trailing_zeros32(std::uint32_t v) {
    int n = 0;
    for (std::uint32_t bit = 1u; bit != 0 && (v & bit) == 0; bit <<= 1) {
        ++n;
    }
    return n;
}
constexpr int count_leading_zeros64(std::uint64_t v) {
    int n = 0;
    for (std::uint64_t bit = 0x8000000000000000ull; bit != 0 && (v & bit) == 0; bit >>= 1) {
        ++n;
    }
    return n;
}
constexpr int count_trailing_zeros64(std::uint64_t v) {
    int n = 0;
    for (std::uint64_t bit = 1ull; bit != 0 && (v & bit) == 0; bit <<= 1) {
        ++n;
    }
    return n;
}
#endif

}  // namespace tt::compiler
