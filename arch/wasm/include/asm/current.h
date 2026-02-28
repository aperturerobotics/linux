/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_WASM_CURRENT_H
#define _ASM_WASM_CURRENT_H

#ifndef __ASSEMBLY__

struct task_struct;

extern struct task_struct *wasm_current_task;

#define current wasm_current_task

#endif /* !__ASSEMBLY__ */

#endif /* _ASM_WASM_CURRENT_H */
