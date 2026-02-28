/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_WASM_THREAD_INFO_H
#define _ASM_WASM_THREAD_INFO_H

#include <asm/page.h>

#define THREAD_SIZE_ORDER 1
#define THREAD_SIZE (PAGE_SIZE << THREAD_SIZE_ORDER)

#ifndef __ASSEMBLY__

#include <asm/processor.h>

struct thread_info {
	unsigned long		syscall_work;
	int			preempt_count;
	unsigned long		flags;
};

#define INIT_THREAD_INFO(tsk) { .flags = 0, }

/*
 * TIF flags.
 */
#define TIF_SYSCALL_TRACE	0
#define TIF_NOTIFY_RESUME	1
#define TIF_SIGPENDING		2
#define TIF_NEED_RESCHED	3
#define TIF_MEMDIE		4
#define TIF_SYSCALL_AUDIT	5
#define TIF_NOTIFY_SIGNAL	6
#define TIF_UPROBE		7

#define _TIF_SYSCALL_TRACE	(1 << TIF_SYSCALL_TRACE)
#define _TIF_NOTIFY_RESUME	(1 << TIF_NOTIFY_RESUME)
#define _TIF_SIGPENDING		(1 << TIF_SIGPENDING)
#define _TIF_NEED_RESCHED	(1 << TIF_NEED_RESCHED)
#define _TIF_MEMDIE		(1 << TIF_MEMDIE)
#define _TIF_SYSCALL_AUDIT	(1 << TIF_SYSCALL_AUDIT)
#define _TIF_NOTIFY_SIGNAL	(1 << TIF_NOTIFY_SIGNAL)
#define _TIF_UPROBE		(1 << TIF_UPROBE)

#define _TIF_WORK_MASK		(_TIF_NOTIFY_RESUME | _TIF_SIGPENDING | \
				 _TIF_NEED_RESCHED | _TIF_NOTIFY_SIGNAL)

#endif /* !__ASSEMBLY__ */

#endif
