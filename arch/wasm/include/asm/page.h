/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_WASM_PAGE_H
#define _ASM_WASM_PAGE_H

#include <linux/const.h>

/* WASM pages are 64K (2^16). */
#define PAGE_SHIFT	16
#define PAGE_SIZE	(_AC(1, UL) << PAGE_SHIFT)
#define PAGE_MASK	(~(PAGE_SIZE - 1))

/* NOMMU: flat memory model. */
#define PAGE_OFFSET	0UL

#ifndef __ASSEMBLY__

#define clear_page(page)	memset((page), 0, PAGE_SIZE)
#define copy_page(to, from)	memcpy((to), (from), PAGE_SIZE)

#define clear_user_page(page, vaddr, pg)	clear_page(page)
#define copy_user_page(to, from, vaddr, pg)	copy_page(to, from)

typedef unsigned long pte_t;
typedef unsigned long pgd_t;
typedef unsigned long pgprot_t;

#define pte_val(x)	(x)
#define pgd_val(x)	(x)
#define pgprot_val(x)	(x)

#define __pte(x)	(x)
#define __pgd(x)	(x)
#define __pgprot(x)	(x)

typedef struct page *pgtable_t;

/* NOMMU: no virtual/physical distinction. */
#define __pa(x)		((unsigned long)(x))
#define __va(x)		((void *)(x))

#define virt_to_pfn(kaddr)	(__pa(kaddr) >> PAGE_SHIFT)
#define pfn_to_virt(pfn)	__va((pfn) << PAGE_SHIFT)

#define virt_to_page(addr)	pfn_to_page(virt_to_pfn(addr))
#define page_to_virt(page)	pfn_to_virt(page_to_pfn(page))

#define pfn_valid(pfn)		((pfn) < max_mapnr)
#define virt_addr_valid(kaddr)	pfn_valid(virt_to_pfn(kaddr))

/* NOMMU: vmalloc maps directly. */
#define VMALLOC_START	PAGE_OFFSET
#define VMALLOC_END	(PAGE_OFFSET + 0x40000000UL)
#define PAGE_KERNEL	__pgprot(0)

#endif /* !__ASSEMBLY__ */

#include <asm-generic/memory_model.h>
#include <asm-generic/getorder.h>

#endif /* _ASM_WASM_PAGE_H */
