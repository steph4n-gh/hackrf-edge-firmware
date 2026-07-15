/*
 * Copyright 2026 HackRF Edge contributors
 *
 * This file is part of HackRF.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 */

/* These strong wrappers are deliberately compiled without LTO. libopencm3's
 * vector table and weak aliases live in a non-LTO archive; keeping the public
 * handlers in the same link domain prevents the LTO plugin from resolving the
 * vector entries to blocking_handler/null_handler before materializing the
 * application implementations. The post-link vector gate checks this contract. */

#include <stdint.h>

void dma_isr_impl(void);
void usb0_isr_impl(void);
void sys_tick_handler_impl(void);
void hard_fault_handler_c(uint32_t* args);

__attribute__((naked, used, externally_visible)) void hard_fault_handler(void)
{
	__asm__("TST LR, #4");
	__asm__("ITE EQ");
	__asm__("MRSEQ R0, MSP");
	__asm__("MRSNE R0, PSP");
	__asm__("B hard_fault_handler_c");
}

void __attribute__((used, externally_visible)) mem_manage_handler(void)
{
	while (1) {}
}

void __attribute__((used, externally_visible)) bus_fault_handler(void)
{
	while (1) {}
}

void __attribute__((used, externally_visible)) usage_fault_handler(void)
{
	while (1) {}
}

void __attribute__((used, externally_visible)) dma_isr(void)
{
	dma_isr_impl();
}

void __attribute__((used, externally_visible)) usb0_isr(void)
{
	usb0_isr_impl();
}

void __attribute__((used, externally_visible)) sys_tick_handler(void)
{
	sys_tick_handler_impl();
}
