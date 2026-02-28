/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_WASM_CACHE_H
#define _ASM_WASM_CACHE_H

/* WASM has no hardware cache lines. Use a reasonable default. */
#define L1_CACHE_SHIFT 5
#define L1_CACHE_BYTES (1 << L1_CACHE_SHIFT)

#endif
