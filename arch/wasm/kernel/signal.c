// SPDX-License-Identifier: GPL-2.0-only

#include <linux/signal.h>
#include <linux/sched/signal.h>
#include <asm/ptrace.h>

void do_signal(struct pt_regs *regs)
{
	/* Stub: signal delivery not yet implemented for WASM. */
}

asmlinkage long sys_rt_sigreturn(void)
{
	return -ENOSYS;
}
