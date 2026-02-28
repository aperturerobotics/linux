// SPDX-License-Identifier: GPL-2.0-only

#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <asm/wasm.h>

static u64 wasm_clocksource_read(struct clocksource *cs)
{
	return host_clock_get();
}

static struct clocksource wasm_clocksource = {
	.name	= "wasm_host_clock",
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
	.rating	= 200,
	.read	= wasm_clocksource_read,
	.mask	= CLOCKSOURCE_MASK(64),
};

void __init time_init(void)
{
	if (clocksource_register_khz(&wasm_clocksource, 1000000U))
		panic("Failed to register WASM clocksource");
}
