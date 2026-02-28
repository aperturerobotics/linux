// SPDX-License-Identifier: GPL-2.0-only

#include <linux/init.h>
#include <linux/sched.h>

struct task_struct *wasm_current_task;

#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/screen_info.h>

unsigned long memory_start;
unsigned long memory_end;

struct screen_info screen_info = {
	.orig_x = 0,
	.orig_y = 25,
	.orig_video_cols = 80,
	.orig_video_lines = 25,
	.orig_video_isVGA = 1,
	.orig_video_points = 16,
};

void __init arch_zone_limits_init(unsigned long *max_zone_pfn)
{
	max_zone_pfn[ZONE_NORMAL] = memory_end >> PAGE_SHIFT;
}

extern void wasm_console_init(void);

void __init setup_arch(char **cmdline_p)
{
	wasm_console_init();

	*cmdline_p = boot_command_line;
	parse_early_param();

	/* Set max PFN values for the page allocator. */
	max_pfn = max_low_pfn = memory_end >> PAGE_SHIFT;
	min_low_pfn = memory_start >> PAGE_SHIFT;

	/* Reserve kernel image: text, data, BSS (0 through memory_start). */
	memblock_reserve(0, memory_start);

	memblock_add(memory_start, memory_end - memory_start);
	memblock_set_current_limit(memory_end);
	memblock_allow_resize();
}
