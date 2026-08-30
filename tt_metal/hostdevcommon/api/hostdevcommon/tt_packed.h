// SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Portable struct packing for structures shared between host and device:
//
//   TT_PACK_BEGIN
//   struct TT_PACKED foo { ... };
//   TT_PACK_END
//
// On GCC/Clang (including the RISC-V device toolchain) this expands to exactly
// __attribute__((packed)), so existing layouts are unchanged. On MSVC it uses
// #pragma pack(push, 1) / pack(pop) instead.
#if defined(__GNUC__) || defined(__clang__)
#define TT_PACKED __attribute__((packed))
#define TT_PACKED_ALIGNED(n) __attribute__((packed, aligned(n)))
// For a struct whose natural GCC alignment is already n this is a no-op on GCC/Clang; inside
// an MSVC TT_PACK region it restores the alignment (and trailing-padding size rounding) that
// pack(1) would otherwise strip. Use it to keep byte-identical sizes without adding `packed`
// (which would change GCC member-access codegen on the device).
#define TT_ALIGNED(n) __attribute__((aligned(n)))
#define TT_PACK_BEGIN
#define TT_PACK_END
#else
#define TT_PACKED
// Inside a TT_PACK_BEGIN/END region, alignas(n) reproduces GCC's packed+aligned(n): members
// are byte-packed by the pragma while the struct's alignment (and size rounding) is n.
#define TT_PACKED_ALIGNED(n) alignas(n)
#define TT_ALIGNED(n) alignas(n)
#define TT_PACK_BEGIN __pragma(pack(push, 1))
#define TT_PACK_END __pragma(pack(pop))
#endif

// Padding member whose byte count is a compile-time C++ expression that may evaluate to zero.
// GCC/Clang express this directly as a zero-length array (their extension); MSVC has no
// zero-size members, so an empty [[msvc::no_unique_address]] struct stands in for the N == 0
// case (it occupies zero bytes, keeping layouts byte-identical).
#if defined(__GNUC__) || defined(__clang__)
#define TT_PAD_BYTES(name, n) volatile unsigned char name[n]
#else
namespace tt::detail {
// Tag makes each zero-size instantiation a distinct type: identically-typed empty members may
// not overlap, so distinct types are required for [[msvc::no_unique_address]] to erase them all.
template <int N, int Tag>
struct PadBytes {
    volatile unsigned char bytes[N];
};
template <int Tag>
struct PadBytes<0, Tag> {};
}  // namespace tt::detail
#define TT_PAD_BYTES(name, n) [[msvc::no_unique_address]] ::tt::detail::PadBytes<(n), __COUNTER__> name
#endif
