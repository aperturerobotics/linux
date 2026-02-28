# SPDX-License-Identifier: GPL-2.0-only

KBUILD_DEFCONFIG := wasm_defconfig

KBUILD_CFLAGS += --sysroot=$(HOME)/wasi-sdk/share/wasi-sysroot
KBUILD_CFLAGS += -nostdlib -fno-builtin -ffreestanding
KBUILD_CFLAGS += -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables
KBUILD_CFLAGS += -D__DISABLE_EXPORTS
KBUILD_CFLAGS += -include asm/compiler.h

KBUILD_AFLAGS += --sysroot=$(HOME)/wasi-sdk/share/wasi-sysroot


PHONY += vmlinux.bin

all: vmlinux

define archhelp
  echo  '  vmlinux         - Raw kernel ELF image'
endef
