/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_WASM_BARRIER_H
#define _ASM_WASM_BARRIER_H

/*
 * WASM is single-threaded in our configuration. Barriers are compiler-only.
 */
#define nop() do { } while (0)

#define mb()  __sync_synchronize()
#define rmb() mb()
#define wmb() mb()

#include <asm-generic/barrier.h>

#endif
