/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_WASM_ELF_H
#define _ASM_WASM_ELF_H

/* WASM is not ELF, but the kernel build system needs these. */
#define ELF_ARCH	264
#define ELF_CLASS	ELFCLASS32
#define ELF_DATA	ELFDATA2LSB

#define elf_check_arch(x) (0)

#define ELF_EXEC_PAGESIZE	PAGE_SIZE
#define ELF_ET_DYN_BASE	0

typedef unsigned long elf_greg_t;
#define ELF_NGREG 1
typedef elf_greg_t elf_gregset_t[ELF_NGREG];
typedef unsigned long elf_fpregset_t;

#define SET_PERSONALITY(ex) set_personality(PER_LINUX)

#endif
