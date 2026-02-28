/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_WASM_SYSCALL_H
#define _ASM_WASM_SYSCALL_H

#include <linux/sched.h>
#include <asm/ptrace.h>

static inline int syscall_get_nr(struct task_struct *task,
				 struct pt_regs *regs)
{
	return regs->orig_r0;
}

static inline void syscall_set_return_value(struct task_struct *task,
					    struct pt_regs *regs,
					    int error, long val)
{
	regs->r0 = error ? error : val;
}

static inline long syscall_get_error(struct task_struct *task,
				     struct pt_regs *regs)
{
	return (long)regs->r0;
}

static inline long syscall_get_return_value(struct task_struct *task,
					    struct pt_regs *regs)
{
	return regs->r0;
}

static inline void syscall_get_arguments(struct task_struct *task,
					 struct pt_regs *regs,
					 unsigned long *args)
{
	args[0] = regs->r0;
	args[1] = regs->r1;
	args[2] = regs->r2;
	args[3] = regs->r3;
	args[4] = regs->r4;
	args[5] = regs->r5;
}

static inline int syscall_get_arch(struct task_struct *task)
{
	return 0;
}

#endif
