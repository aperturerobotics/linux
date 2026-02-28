/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_WASM_CMPXCHG_H
#define _ASM_WASM_CMPXCHG_H

#include <linux/types.h>

/*
 * Single-threaded WASM: cmpxchg is just a compare-and-swap with no
 * contention possible.
 */

#define arch_xchg(ptr, new)					\
({								\
	__typeof__(*(ptr)) __old = *(ptr);			\
	*(ptr) = (new);						\
	__old;							\
})

#define arch_cmpxchg(ptr, old, new)				\
({								\
	__typeof__(*(ptr)) __old = *(ptr);			\
	if (__old == (__typeof__(*(ptr)))(old))			\
		*(ptr) = (new);					\
	__old;							\
})

#define arch_cmpxchg64(ptr, old, new) arch_cmpxchg((ptr), (old), (new))

#include <asm-generic/cmpxchg-local.h>

#endif
