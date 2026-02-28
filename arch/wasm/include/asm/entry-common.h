/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_WASM_ENTRY_COMMON_H
#define _ASM_WASM_ENTRY_COMMON_H

#include <asm/ptrace.h>

static inline bool on_thread_stack(void)
{
	return true;
}

static inline bool regs_irqs_disabled(struct pt_regs *regs)
{
	return regs->cpuflags & 2;
}

static inline int arch_syscall_is_vdso_sigreturn(struct pt_regs *regs)
{
	return 0;
}

static inline void syscall_rollback(struct task_struct *task,
				    struct pt_regs *regs)
{
}

#endif
