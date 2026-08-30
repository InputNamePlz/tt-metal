// SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Minimal ELF definitions for hosts without <elf.h> (Windows). Layouts and values follow the
// System V gABI and the RISC-V psABI; only the subset used by tt_elffile.cpp is provided.

#include <cstdint>

using Elf32_Half = std::uint16_t;
using Elf32_Word = std::uint32_t;
using Elf32_Sword = std::int32_t;
using Elf32_Addr = std::uint32_t;
using Elf32_Off = std::uint32_t;

using Elf64_Half = std::uint16_t;
using Elf64_Word = std::uint32_t;
using Elf64_Sword = std::int32_t;
using Elf64_Xword = std::uint64_t;
using Elf64_Sxword = std::int64_t;
using Elf64_Addr = std::uint64_t;
using Elf64_Off = std::uint64_t;

inline constexpr int EI_NIDENT = 16;

struct Elf32_Ehdr {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half e_type;
    Elf32_Half e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;
    Elf32_Off e_phoff;
    Elf32_Off e_shoff;
    Elf32_Word e_flags;
    Elf32_Half e_ehsize;
    Elf32_Half e_phentsize;
    Elf32_Half e_phnum;
    Elf32_Half e_shentsize;
    Elf32_Half e_shnum;
    Elf32_Half e_shstrndx;
};

struct Elf64_Ehdr {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off e_phoff;
    Elf64_Off e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
};

struct Elf32_Phdr {
    Elf32_Word p_type;
    Elf32_Off p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
};

struct Elf64_Phdr {
    Elf64_Word p_type;
    Elf64_Word p_flags;
    Elf64_Off p_offset;
    Elf64_Addr p_vaddr;
    Elf64_Addr p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
};

struct Elf32_Shdr {
    Elf32_Word sh_name;
    Elf32_Word sh_type;
    Elf32_Word sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off sh_offset;
    Elf32_Word sh_size;
    Elf32_Word sh_link;
    Elf32_Word sh_info;
    Elf32_Word sh_addralign;
    Elf32_Word sh_entsize;
};

struct Elf64_Shdr {
    Elf64_Word sh_name;
    Elf64_Word sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr sh_addr;
    Elf64_Off sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word sh_link;
    Elf64_Word sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
};

struct Elf32_Sym {
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Half st_shndx;
};

struct Elf64_Sym {
    Elf64_Word st_name;
    unsigned char st_info;
    unsigned char st_other;
    Elf64_Half st_shndx;
    Elf64_Addr st_value;
    Elf64_Xword st_size;
};

struct Elf32_Rela {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
    Elf32_Sword r_addend;
};

struct Elf64_Rela {
    Elf64_Addr r_offset;
    Elf64_Xword r_info;
    Elf64_Sxword r_addend;
};

// e_ident indices and values.
#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3
#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6

#define ELFCLASS32 1
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1

// e_type / e_machine.
#define ET_REL 1
#define ET_EXEC 2
#define EM_RISCV 243

// Program header types.
#define PT_LOAD 1
#define PT_TLS 7

// Section header types and flags.
#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_RELA 4
#define SHT_NOBITS 8

#define SHF_ALLOC 0x2
#define SHF_TLS 0x400

// Symbol binding and type.
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2

#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_COMMON 5
#define STT_TLS 6

#define ELF32_ST_BIND(val) (((unsigned char)(val)) >> 4)
#define ELF32_ST_TYPE(val) ((val) & 0xf)
#define ELF32_ST_INFO(bind, type) (((bind) << 4) + ((type) & 0xf))
#define ELF64_ST_BIND(val) ELF32_ST_BIND(val)
#define ELF64_ST_TYPE(val) ELF32_ST_TYPE(val)
#define ELF64_ST_INFO(bind, type) ELF32_ST_INFO(bind, type)

// Relocation info packing.
#define ELF32_R_SYM(val) ((val) >> 8)
#define ELF32_R_TYPE(val) ((val) & 0xff)
#define ELF32_R_INFO(sym, type) (((sym) << 8) + ((type) & 0xff))
#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)
#define ELF64_R_INFO(sym, type) ((((Elf64_Xword)(sym)) << 32) + (type))

// RISC-V relocation types (psABI).
#define R_RISCV_NONE 0
#define R_RISCV_32 1
#define R_RISCV_BRANCH 16
#define R_RISCV_JAL 17
#define R_RISCV_CALL 18
#define R_RISCV_CALL_PLT 19
#define R_RISCV_PCREL_HI20 23
#define R_RISCV_PCREL_LO12_I 24
#define R_RISCV_PCREL_LO12_S 25
#define R_RISCV_HI20 26
#define R_RISCV_LO12_I 27
#define R_RISCV_LO12_S 28
#define R_RISCV_ADD32 35
#define R_RISCV_SUB32 39
#define R_RISCV_RELAX 51
#define R_RISCV_32_PCREL 57
