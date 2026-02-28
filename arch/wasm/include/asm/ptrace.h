/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_WASM_PTRACE_H
#define _ASM_WASM_PTRACE_H

#include <uapi/asm/ptrace.h>

struct pt_regs {
	unsigned long sp;
	unsigned long cpuflags;
	unsigned long orig_r0;
	unsigned long r0;
	unsigned long r1;
	unsigned long r2;
	unsigned long r3;
	unsigned long r4;
	unsigned long r5;
};

#define user_mode(regs) ((regs)->cpuflags & 1)
#define instruction_pointer(regs) (0)
#define profile_pc(regs) (0)

static inline unsigned long user_stack_pointer(struct pt_regs *regs)
{
	return regs->sp;
}

#endif
