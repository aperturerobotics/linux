// SPDX-License-Identifier: GPL-2.0-only
#define COMPILE_OFFSETS

#include <linux/kbuild.h>
#include <linux/sched.h>
#include <asm/thread_info.h>
#include <asm/ptrace.h>

void asm_offsets(void)
{
	DEFINE(THREAD_SIZE, THREAD_SIZE);
	DEFINE(PT_REGS_SIZE, sizeof(struct pt_regs));
	BLANK();
	OFFSET(PT_SP, pt_regs, sp);
	OFFSET(PT_CPUFLAGS, pt_regs, cpuflags);
	OFFSET(PT_ORIG_R0, pt_regs, orig_r0);
	OFFSET(PT_R0, pt_regs, r0);
}
