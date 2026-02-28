/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_WASM_PGTABLE_H
#define _ASM_WASM_PGTABLE_H

/* NOMMU: no page tables. Include generic NOMMU support. */
#include <asm-generic/pgtable-nopmd.h>

#ifndef __ASSEMBLY__

#include <asm/page.h>

extern unsigned long empty_zero_page;

#define ZERO_PAGE(vaddr)	(virt_to_page(&empty_zero_page))

/* NOMMU: no page directory. */
#define swapper_pg_dir ((pgd_t *) 0)

#endif /* !__ASSEMBLY__ */

#endif
