/*
* Copyright (c) 2016, 2017 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/

#include <stdint.h>
#include <stdio.h>

#include <onyx/irq.h>
#include <onyx/portio.h>
#include <onyx/pit.h>
#include <onyx/pic.h>
#include <onyx/compiler.h>

static volatile uint64_t timer_ticks = 0;
uintptr_t timer_handler(registers_t *regs)
{
	timer_ticks++;
	return 0;
}
void pit_init(uint32_t frequency)
{
	int divisor = 1193180 / frequency;

	// Install the IRQ handler
	irq_install_handler(2, timer_handler);

	outb(0x43, 0x34);
	io_wait();
	outb(0x40, divisor & 0xFF);   // Set low byte of divisor
	io_wait();
	outb(0x40, divisor >> 8);     // Set high byte of divisor
	io_wait();
}
void pit_deinit()
{
	outb(0x42, 0x34);
	irq_uninstall_handler(2, timer_handler);
}
uint64_t pit_get_tick_count()
{
	return (uint64_t) timer_ticks;
}
