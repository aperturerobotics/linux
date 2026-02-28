/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_WASM_MMU_CONTEXT_H
#define _ASM_WASM_MMU_CONTEXT_H

#include <linux/mm_types.h>
#include <linux/sched.h>

/* NOMMU: no memory management context. */
static inline void switch_mm(struct mm_struct *prev,
			     struct mm_struct *next,
			     struct task_struct *tsk)
{
}

#include <asm-generic/mmu_context.h>

#endif
