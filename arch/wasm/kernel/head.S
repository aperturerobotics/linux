/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * WASI reactor entrypoint.
 */

.globaltype __stack_pointer, i32

.functype start_kernel() -> ()
.functype host_console_write(i32, i32) -> (i32)

.globl _initialize
_initialize:
	.functype _initialize() -> ()

	/*
	 * Grow memory to give the kernel room to work.
	 * Grow to at least 1024 pages (64 MB) total.
	 */
	i32.const 1010
	memory.grow 0
	/* Result: old size or -1 on failure. Drop it. */
	drop

	/*
	 * Set stack pointer to top of init stack (grows downward).
	 */
	i32.const __end_init_stack
	global.set __stack_pointer

	/*
	 * Initialize current task pointer.
	 */
	i32.const wasm_current_task
	i32.const init_task
	i32.store 0

	/* Record memory layout: start after kernel data, end at memory top. */
	i32.const memory_start
	i32.const __bss_stop
	i32.store 0

	i32.const memory_end
	memory.size 0
	i32.const 16
	i32.shl
	i32.store 0

	call start_kernel

	end_function

/*
 * Host calls this to deliver a timer tick.
 */
.globl tick
tick:
	.functype tick() -> ()
	/* TODO: call timer interrupt handler */
	end_function

/*
 * Host calls this to deliver an I/O event.
 */
.globl deliver_io
deliver_io:
	.functype deliver_io(i32, i32) -> ()
	/* TODO: call device interrupt handler */
	end_function
