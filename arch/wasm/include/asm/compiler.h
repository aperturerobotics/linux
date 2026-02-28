/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_WASM_COMPILER_H
#define _ASM_WASM_COMPILER_H

/*
 * WASM has no instruction pointer or return address.
 * Override the builtin to return 0.
 */
#define __builtin_return_address(x) ((void *)0)

#endif
