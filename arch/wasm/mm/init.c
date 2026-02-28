// SPDX-License-Identifier: GPL-2.0-only

#include <linux/mm.h>
#include <linux/init.h>
#include <asm/page.h>

/* Zeroed page used by ZERO_PAGE() macro. */
unsigned long empty_zero_page __attribute__((aligned(PAGE_SIZE)));
