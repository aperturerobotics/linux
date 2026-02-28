/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_WASM_IRQFLAGS_H
#define _ASM_WASM_IRQFLAGS_H

/* Software interrupt enable/disable flag. */
extern unsigned long wasm_irq_flags;

static inline unsigned long arch_local_save_flags(void)
{
	return wasm_irq_flags;
}

static inline void arch_local_irq_disable(void)
{
	wasm_irq_flags = 0;
}

static inline void arch_local_irq_enable(void)
{
	wasm_irq_flags = 1;
}

static inline unsigned long arch_local_irq_save(void)
{
	unsigned long flags = wasm_irq_flags;
	wasm_irq_flags = 0;
	return flags;
}

static inline void arch_local_irq_restore(unsigned long flags)
{
	wasm_irq_flags = flags;
}

static inline bool arch_irqs_disabled_flags(unsigned long flags)
{
	return flags == 0;
}

static inline bool arch_irqs_disabled(void)
{
	return wasm_irq_flags == 0;
}

#endif
