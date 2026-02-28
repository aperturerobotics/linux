/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_WASM_PROCESSOR_H
#define _ASM_WASM_PROCESSOR_H

#include <asm/ptrace.h>

/* WASM32: 4GB address space, user gets 3GB. */
#define TASK_SIZE	0xC0000000UL

/*
 * WASM has no hardware registers to save. Thread state is minimal.
 */
struct thread_struct {
	unsigned long sp;
};

#define INIT_THREAD { .sp = 0, }

#define task_pt_regs(task) \
	((struct pt_regs *)(task_stack_page(task) + THREAD_SIZE) - 1)

static inline void cpu_relax(void)
{
	/* No-op. WASM has no pause instruction. */
}

#define KSTK_EIP(task) (0)
#define KSTK_ESP(task) (task_pt_regs(task)->sp)


unsigned long __get_wchan(struct task_struct *p);

#endif
