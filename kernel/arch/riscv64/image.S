/*
* Copyright (c) 2020 - 2023 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the GPLv2 License
* check LICENSE at the root directory for more information
*/
.section .text
.global __cxa_atexit
__cxa_atexit:
	li a0, 0
	ret

.section .bss
.align 16

stack_bottom:
.skip 4096

stack_top:

.section .boot,"ax"

#include <onyx/image.h>

.global entry_point
.type entry_point,@function
entry_point:
	# Header time!
	j entry
	# Code 1 - unused
	.word 0
	.dword RISCV_LOAD_ADDRESS
	.dword __kernel_size
	.dword 0
	.word RISCV_HEADER_VERSION
	# Reserved 
	.word 0
	.dword 0
	.ascii RISCV_IMAGE_MAGIC
	.balign 4
	.ascii RISCV_IMAGE_MAGIC2
	# Reserved
	.word 0

booted: .dword 0

virtual_entry: .dword riscv_virtual_entry
secondary_ventry: .dword riscv_secondary_virtual_entry

entry:
.option push
.option norelax
	lla gp, __global_pointer$
.option pop

	/* a0 = hartid, a1 = device tree */
	mv s1, a1
	mv s2, a0

	# Stop possible infinite loops of boots from happening
	lw t1, booted
	bnez t1, 2f

	li t1, 1
	sw t1, booted, t2

	lla sp, stack_top
	lla a0, boot_page_tables
	call early_paging_setup

	lla t0, boot_page_tables
	srli t1, t0, 12
	li t5, 9 << 60
	or t1, t1, t5

	csrw satp, t1
	sfence.vma zero, zero
	ld ra, virtual_entry
	ret
2:
	j 2b

.global riscv_virtual_entry
riscv_virtual_entry:
	# Reload the gp, sp registers
.option push
.option norelax
	lla gp, __global_pointer$
.option pop
	lla sp, stack_top
	lla tp, percpu_base
	mv a1, s1
	mv a0, s2
	call kernel_entry

	call runtime_call_constructors
	# rdi = cpu nr
	li a0, 0
	call init_ssp_for_cpu

	call kernel_main
1:
	wfi
	j 1b

.global riscv_secondary_hart_start
riscv_secondary_hart_start:
.option push
.option norelax
	lla gp, __global_pointer$
.option pop

	/* Status: interrupts disabled, satp = 0,
	 * a0 = hartid, a1 = opaque.
	 *
	 * We use opaque to store our stack, which we will quickly use to get
	 * the percpu data.
	 */
	mv sp, a1

	/* Get the bootstrap page tables */
	lla t0, boot_page_tables
	srli t1, t0, 12
	li t5, 9 << 60
	or t1, t1, t5

	csrw satp, t1
	sfence.vma zero, zero
	ld ra, secondary_ventry
	ret
riscv_secondary_virtual_entry:
.option push
.option norelax
	lla gp, __global_pointer$
.option pop

	/* Load the actual page tables */
	ld t0, boot_pt
	srli t1, t0, 12
	li t5, 9 << 60
	or t1, t1, t5

	csrw satp, t1
	sfence.vma zero, zero

	/* Load the tp */
	ld tp, -8(sp)

	mv s1, a0
	/* Get the cpunr and init ssp */
	la a0, cpu_nr
	add a0, a0, tp
	ld a0, 0(a0)
	call init_ssp_for_cpu

	mv a0, s1
	call smpboot_main
1:
	wfi
	j 1b
