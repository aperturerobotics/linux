// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/wasm/kernel/setup.c - Architecture setup and boot overrides
 * for the WebAssembly (WASI reactor) kernel port.
 *
 * WASM is single-threaded: no real context switching, no SMP, no
 * kernel threads. The standard rest_init() path spawns kthreadd
 * and calls schedule(), which we cannot do. Instead, we override
 * arch_call_rest_init() to run the essential init path synchronously:
 *   1. Register binfmt_wasm
 *   2. Unpack the built-in initramfs
 *   3. exec /init via kernel_execve()
 *   4. Enter the idle loop
 *
 * The initcall mechanism (core_initcall, rootfs_initcall, etc.)
 * relies on ELF section attributes (.initcallN.init) that the WASM
 * backend silently drops. Critical init functions must be called
 * explicitly from arch_call_rest_init().
 */

#include <linux/init.h>
#include <linux/sched.h>
#include <linux/cpuhotplug.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/screen_info.h>
#include <linux/binfmts.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/initrd.h>
#include <linux/namei.h>
#include <linux/path.h>

struct task_struct *wasm_current_task;

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

	max_pfn = max_low_pfn = memory_end >> PAGE_SHIFT;
	min_low_pfn = memory_start >> PAGE_SHIFT;

	memblock_reserve(0, memory_start);
	memblock_add(memory_start, memory_end - memory_start);
	memblock_set_current_limit(memory_end);
	memblock_allow_resize();
}

/*
 * binfmt_wasm: binary format handler for WebAssembly modules.
 *
 * Simplified handler: reads the WASM binary and hands it to the host
 * via host_load_executable(). Skips begin_new_exec/setup_new_exec
 * because those require scheduler support (de_thread, exec_mmap).
 * The host runtime handles process creation outside the kernel.
 */

static const unsigned char wasm_magic[] = { 0x00, 0x61, 0x73, 0x6d,
					    0x01, 0x00, 0x00, 0x00 };

extern void host_load_executable(unsigned long code_ptr,
				 unsigned long code_len);

static int load_wasm_binary(struct linux_binprm *bprm);

static struct linux_binfmt wasm_format = {
	.module		= THIS_MODULE,
	.load_binary	= load_wasm_binary,
};

static int load_wasm_binary(struct linux_binprm *bprm)
{
	struct file *file = bprm->file;
	loff_t size;
	void *buf;
	loff_t pos = 0;
	ssize_t nread;

	if (memcmp(bprm->buf, wasm_magic, sizeof(wasm_magic)) != 0)
		return -ENOEXEC;

	size = i_size_read(file_inode(file));
	if (size <= (loff_t)sizeof(wasm_magic) || size > (loff_t)(16 << 20))
		return -ENOEXEC;

	buf = kvmalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	nread = kernel_read(file, buf, size, &pos);
	if (nread != size) {
		kvfree(buf);
		return -EIO;
	}

	pr_info("binfmt_wasm: loading %lld byte module\n", size);
	host_load_executable((unsigned long)buf, (unsigned long)size);

	kvfree(buf);
	return 0;
}

/*
 * arch_call_rest_init - WASM override of the default rest_init() path.
 *
 * In a normal kernel, rest_init() spawns kthreadd and kernel_init as
 * separate kernel threads, then enters the idle loop. WASM cannot do
 * context switching between threads, so we run the essential init
 * path synchronously: register binfmt_wasm, unpack initramfs, exec
 * /init, then enter the idle loop.
 */
extern void cpu_startup_entry(enum cpuhp_state state);
extern void rcu_scheduler_starting(void);
extern char *unpack_to_rootfs(char *buf, unsigned long len);
extern char __initramfs_start[];
extern unsigned long __initramfs_size;

void __init __noreturn arch_call_rest_init(void)
{
	char *err;
	int ret;

	rcu_scheduler_starting();
	system_state = SYSTEM_SCHEDULING;

	/* Register WASM binary format handler. */
	register_binfmt(&wasm_format);

	/* Unpack built-in initramfs (normally done by rootfs_initcall). */
	if (__initramfs_size > 0) {
		pr_info("wasm: unpacking initramfs (%lu bytes)\n",
			__initramfs_size);
		err = unpack_to_rootfs(__initramfs_start, __initramfs_size);
		if (err)
			pr_err("wasm: initramfs unpack failed: %s\n", err);
	}

	system_state = SYSTEM_RUNNING;

	/* Fix i_writecount left by deferred fput from cpio extractor.
	 * The cpio extractor uses deferred fput (task_work) which does
	 * not run without the scheduler. Reset i_writecount on /init so
	 * deny_write_access() in execve succeeds. */
	{
		struct path p;
		ret = kern_path("/init", LOOKUP_FOLLOW, &p);
		if (!ret) {
			struct inode *inode = p.dentry->d_inode;
			if (atomic_read(&inode->i_writecount) > 0) {
				pr_info("wasm: resetting /init i_writecount\n");
				atomic_set(&inode->i_writecount, 0);
			}
			/* Skip path_put - hangs in single-threaded WASM
			 * because mntput involves RCU synchronization. */
		}
	}

	/* Clear PF_KTHREAD so kernel_execve accepts the call.
	 * The idle task (PID 0) has PF_KTHREAD set. In the normal
	 * kernel, user_mode_thread() creates init without this flag. */
	current->flags &= ~PF_KTHREAD;

	/* Try to exec /init from the initramfs. */
	{
		static const char *argv[] = { "/init", NULL };
		static const char *envp[] = { "HOME=/", NULL };
		ret = kernel_execve("/init", argv, envp);
		if (ret)
			pr_err("wasm: kernel_execve /init failed: %d\n", ret);
	}

	/* Enter the idle loop. If binfmt_wasm called
	 * host_load_executable(), host_yield() will detect the pending
	 * exec and panic to break back to the Go host. */
	cpu_startup_entry(CPUHP_ONLINE);
}
