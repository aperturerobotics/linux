// SPDX-License-Identifier: GPL-2.0-only

#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/ptrace.h>
#include <linux/printk.h>
#include <asm/wasm.h>

void arch_cpu_idle(void)
{
	host_yield();
}

int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)
{
	struct pt_regs *childregs = task_pt_regs(p);

	if (unlikely(args->fn)) {
		/* Kernel thread. */
		memset(childregs, 0, sizeof(*childregs));
		childregs->sp = (unsigned long)childregs;
		return 0;
	}

	/* User thread: copy parent regs. */
	*childregs = *task_pt_regs(current);
	if (args->stack)
		childregs->sp = args->stack;
	childregs->r0 = 0;  /* Child returns 0 from fork. */

	return 0;
}

void flush_thread(void)
{
}

void show_regs(struct pt_regs *regs)
{
	show_regs_print_info(KERN_DEFAULT);
	pr_cont("sp: %08lx cpuflags: %08lx\n", regs->sp, regs->cpuflags);
}

void show_stack(struct task_struct *task, unsigned long *stack,
		const char *loglvl)
{
	pr_info("%sStack: (WASM - no hardware stack trace)\n", loglvl);
}

unsigned long __get_wchan(struct task_struct *p)
{
	return 0;
}
