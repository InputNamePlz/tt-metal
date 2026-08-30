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
#define TT_PACK_BEGIN
#define TT_PACK_END
#else
#define TT_PACKED
#define TT_PACK_BEGIN __pragma(pack(push, 1))
#define TT_PACK_END __pragma(pack(pop))
#endif
