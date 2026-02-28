// SPDX-License-Identifier: GPL-2.0-only

/* Software interrupt flag for single-threaded WASM. */
unsigned long wasm_irq_flags = 1;  /* Interrupts enabled at boot. */
