/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_WASM_WASM_H
#define _ASM_WASM_WASM_H

/*
 * Host function imports. These are extern functions that become WASM imports
 * via --import-undefined. The Go host (wazero) provides implementations.
 */

extern int host_console_write(const char *buf, int len);
extern int host_console_read(char *buf, int len);
extern unsigned long long host_clock_get(void);
extern int host_random(void *buf, int len);
extern void host_yield(void);
extern void host_exit(int code);
extern void host_panic(const char *msg);
extern void host_load_executable(unsigned long code_ptr, unsigned long code_len);

#endif
