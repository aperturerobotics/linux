// SPDX-License-Identifier: GPL-2.0-only
//
// WASM host console driver.
// Routes kernel printk output through host_console_write() import.

#include <linux/console.h>
#include <linux/init.h>
#include <asm/wasm.h>

static void wasm_console_write(struct console *co, const char *s, unsigned int count)
{
	host_console_write(s, count);
}

static struct console wasm_console = {
	.name	= "wasm",
	.write	= wasm_console_write,
	.flags	= CON_PRINTBUFFER | CON_BOOT,
	.index	= -1,
};

void __init wasm_console_init(void)
{
	register_console(&wasm_console);
}
