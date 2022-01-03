/*
 * Copyright (c) 2016 - 2021 Pedro Falcato
 * This file is part of Onyx, and is released under the terms of the MIT License
 * check LICENSE at the root directory for more information
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <cpuid.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <x86intrin.h>

#include <onyx/acpi.h>
#include <onyx/compiler.h>
#include <onyx/log.h>
#include <onyx/cpu.h>
#include <onyx/panic.h>
#include <onyx/acpi.h>
#include <onyx/spinlock.h>
#include <onyx/registers.h>
#include <onyx/irq.h>
#include <onyx/fpu.h>
#include <onyx/percpu.h>
#include <onyx/init.h>
#include <onyx/internal_abi.h>
#include <onyx/serial.h>

#include <onyx/x86/avx.h>
#include <onyx/x86/pat.h>
#include <onyx/x86/gdt.h>
#include <onyx/x86/apic.h>
#include <onyx/x86/pic.h>
#include <onyx/x86/msr.h>
#include <onyx/x86/platform_info.h>
#include <onyx/x86/tsc.h>
#include <onyx/x86/segments.h>
#include <onyx/x86/control_regs.h>
#include <onyx/x86/alternatives.h>
#include <onyx/x86/ktrace.h>

static cpu_t cpu;

static unsigned int booted_cpus = 1;
extern ACPI_TABLE_MADT *madt;
extern volatile uint32_t *bsp_lapic;
volatile unsigned int initialized_cpus = 0;
extern volatile uint64_t boot_ticks;
const int bits_per_long = sizeof(unsigned long) * 8;

struct x86_platform_info x86_platform = {};

__attribute__((hot))
bool x86_has_cap(int cap)
{
	/* Get the index in native word sizes(DWORDS in 32-bit systems and QWORDS in 64-bit ones) */
	int q_index = cap / bits_per_long;
	int bit_index = cap % bits_per_long;
	return cpu.caps[q_index] & (1UL << bit_index);
}

bool x86_has_usable_tsc(void)
{
	return cpu.invariant_tsc || cpu.constant_tsc;
}

void x86_set_tsc_rate(uint64_t rate)
{
	cpu.tsc_rate = rate;
}

uint64_t x86_get_tsc_rate(void)
{
	return cpu.tsc_rate;
}

void __cpu_identify(void)
{
	uint32_t eax = 0;
	uint32_t ebx = 0;
	uint32_t ecx = 0;
	uint32_t edx = 0;
	if(!__get_cpuid(CPUID_FEATURES, &eax, &ebx, &ecx, &edx))
	{
		INFO("x86cpu", "CPUID_FEATURES not supported!\n");
	}
	else cpu.caps[0] = edx | ((uint64_t) ecx << 32);

	eax = CPUID_FEATURES_EXT;
	if(!__get_cpuid_count(CPUID_FEATURES_EXT, 0, &eax, &ebx, &ecx, &edx))
	{
		INFO("x86cpu", "CPUID_FEATURES_EXT not supported!\n");
	}
	else
	{
		cpu.caps[1] = ebx | ((uint64_t) ecx << 32);
		cpu.caps[2] = edx;
	}
	
	eax = CPUID_EXTENDED_PROC_INFO;
	if(!__get_cpuid(CPUID_EXTENDED_PROC_INFO, &eax, &ebx, &ecx, &edx))
	{
		INFO("x86cpu", "CPUID_EXTENDED_PROC_INFO not supported!\n");
	}
	else
	{
		cpu.caps[2] |= ((uint64_t) edx) << 32;
		cpu.caps[3] = ecx;
	}

	if(!__get_cpuid(CPUID_ADVANCED_PM, &eax, &ebx, &ecx, &edx))
	{
		INFO("x86cpu", "CPUID_ADVANCED_PM not supported!\n");
	}
	else
	{
		cpu.invariant_tsc = (bool) (edx & (1 << 8));
	}

	/* Intel manuals 17.17 Time-Stamp Counter describes this in detail.
	 * In short, Pentium M, Pentium 4, some Xeons and some P6's, the TSC increments with
	 * every internal processor cycle, so it's not constant because power management
	 * may throttle it back and forth. However, since family 0xf & model 0x2 and 
	 * family 0x6 & model 0xe, things have been defacto constant, even without the invariant_tsc
	 * flag.
	 */
	if((cpu.family == 0xf && cpu.model > 0x2) || (cpu.family == 0x6 && cpu.model >= 0xe))
		cpu.constant_tsc = true;
#if 0
	/* TODO: Add 15h support */
	if(__get_cpuid(0x15, &eax, &ebx, &ecx, &edx))
	{
		INFO("x86cpu", "0x15 supported!\n");
		halt();
	}
#endif

}

char *cpu_get_name(void)
{
	uint32_t eax = 0;
	uint32_t ebx = 0;
	uint32_t edx = 0;
	uint32_t ecx = 0;
	if(__get_cpuid(0, &eax, &ebx, &ecx, &edx) == 0)
		panic("Odd cpuid error");
	
	uint32_t cpuid[4] = {};
	cpuid[0] = ebx;
	cpuid[1] = edx;
	cpuid[2] = ecx;
	memcpy(&cpu.manuid, &cpuid, 12);

	/* Zero terminate the string */
	cpu.manuid[12] = '\0';

	if(!strcmp(cpu.manuid, "GenuineIntel"))
	{
		cpu.manufacturer = X86_CPU_MANUFACTURER_INTEL;
	}
	else if(!strcmp(cpu.manuid, "AuthenticAMD"))
	{
		cpu.manufacturer = X86_CPU_MANUFACTURER_AMD;
	}
	else
		cpu.manufacturer = X86_CPU_MANUFACTURER_UNKNOWN;

	__get_cpuid(CPUID_MAXFUNCTIONSUPPORTED, &eax, &ebx, &ecx, &edx);
	cpu.max_function = eax;
	if(cpu.max_function >= 0x8000004)
	{
		__get_cpuid(CPUID_BRAND0, &eax, &ebx, &ecx, &edx);
		cpuid[0] = eax;
		cpuid[1] = ebx;
		cpuid[2] = ecx;
		cpuid[3] = edx;
		memcpy(&cpu.brandstr, &cpuid, 16);
		__get_cpuid(CPUID_BRAND1, &eax, &ebx, &ecx, &edx);
		cpuid[0] = eax;
		cpuid[1] = ebx;
		cpuid[2] = ecx;
		cpuid[3] = edx;
		memcpy(&cpu.brandstr[16], &cpuid, 16);
		__get_cpuid(CPUID_BRAND2, &eax, &ebx, &ecx, &edx);
		cpuid[0] = eax;
		cpuid[1] = ebx;
		cpuid[2] = ecx;
		cpuid[3] = edx;
		memcpy(&cpu.brandstr[32], &cpuid, 16);
		cpu.brandstr[47] = '\0';
		// Get the address space sizes
		__get_cpuid(CPUID_ADDR_SPACE_SIZE, &eax, &ebx, &ecx, &edx);
		cpu.physicalAddressSpace = eax & 0xFF;
		cpu.virtualAddressSpace = (eax >> 8) & 0xFF;
	}

	return &cpu.manuid[0];
}

void cpu_get_sign(void)
{
	uint32_t eax, ebx, edx, ecx;
	if(__get_cpuid(CPUID_SIGN, &eax, &ebx, &ecx, &edx) == 0)
		panic("Odd cpuid error getting signature");

	unsigned int stepping = eax & 0xf;
	unsigned int model = (eax >> 4) & 0xf;
	unsigned int family = (eax >> 8) & 0xf;
	unsigned int processor_type = (eax >> 12) & 0x3;
	unsigned int extended_model = (eax >> 16) & 0xf;
	unsigned int extended_family = (eax >> 20) & 0xff;

	unsigned int cpu_family = family;
	unsigned int cpu_model = model;
	if(family == 6 || family == 15)
		cpu_model = model + (extended_model << 4);
	if(family == 15)
		cpu_family = family + extended_family;
	
	printf("CPUID: %04x:%04x:%04x:%04x\n", cpu_family, cpu_model, stepping,
		processor_type);
	cpu.model = cpu_model;
	cpu.family = cpu_family;
	cpu.stepping = stepping;
}

void cpu_identify(void)
{
	INFO("cpu", "Detected x86_64 CPU\n");
	INFO("cpu", "Manufacturer ID: %s\n", cpu_get_name());
	if(cpu.brandstr[0] != '\0')
		printf("Name: %s\n", cpu.brandstr);
	cpu_get_sign();
	INFO("cpu", "Stepping %i, Model %i, Family %i\n", cpu.stepping, cpu.model, cpu.family);
	__cpu_identify();

	x86_do_alternatives();
}

extern "C" void syscall_ENTRY64(void);

void x86_setup_standard_control_registers(void)
{
	/* Note that we do not set floating point bits here, only in fpu_init and avx_init */
	const unsigned long cr0 = CR0_PE | CR0_PG | CR0_ET | CR0_WP;
	x86_write_cr0(cr0);

	unsigned long cr4 = CR4_DE | CR4_MCE | CR4_PAE | CR4_PGE | CR4_PSE;

	if(x86_has_cap(X86_FEATURE_SMAP))
	{
		cr4 |= CR4_SMAP;
	}

	if(x86_has_cap(X86_FEATURE_SMEP))
	{
		cr4 |= CR4_SMEP;
	}

	/* Note that CR4_PGE could only be set at this point in time since Intel
	 * strongly recommends for it to be set after enabling paging
	*/
	x86_write_cr4(cr4);
}

void x86_init_percpu_intel(void)
{
	uint64_t misc_enable = rdmsr(IA32_MISC_ENABLE);
	misc_enable &= ~IA32_MISC_ENABLE_XD_BIT_DISABLE;

	if(x86_has_cap(X86_FEATURE_ERMS))
		misc_enable |= IA32_MISC_ENABLE_FAST_STRINGS_ENABLE;
	if(x86_has_cap(X86_FEATURE_EST))
		misc_enable |= IA32_MISC_ENABLE_ENHANCED_INTEL_SPEEDSTEP;
	if(x86_has_cap(X86_FEATURE_SSE3))
		misc_enable |= IA32_MISC_ENABLE_ENABLE_MONITOR_FSM;
	
	wrmsr(IA32_MISC_ENABLE, misc_enable);
}

void x86_init_percpu(void)
{
	/* Set up the standard control registers to set an equal playing field for every CPU */
	x86_setup_standard_control_registers();
	
	/* Do floating point initialization now */
	fpu_init();
	avx_init();
	
	/* Now initialize caching structures */
	pat_init();

	uint64_t efer = rdmsr(IA32_EFER);
	efer |= IA32_EFER_SCE;
	wrmsr(IA32_EFER, efer);
	/* and finally, syscall instruction MSRs */
	wrmsr(IA32_MSR_STAR, (((uint64_t)((USER32_CS) << 16) | KERNEL_CS) << 32));
	wrmsr(IA32_MSR_LSTAR, (uint64_t) syscall_ENTRY64);
	wrmsr(IA32_MSR_SFMASK, EFLAGS_INT_ENABLED | EFLAGS_DIRECTION |
		EFLAGS_TRAP | EFLAGS_ALIGNMENT_CHECK);
	

	if(cpu.manufacturer == X86_CPU_MANUFACTURER_INTEL)
	{
		x86_init_percpu_intel();
	}

	printf("cpu#%u tsc: %lu\n", get_cpu_nr(), rdtsc());
}

void cpu_init_mp(void)
{
	smp_parse_cpus(madt);

	smp_boot_cpus();

	ENABLE_INTERRUPTS();
}

void cpu_init_late(void)
{
	/* Completely disable the PIC first */
	pic_remap();
	pic_disable();

	pat_init();

	/* Initialize the APIC and LAPIC */
	ioapic_init();
	lapic_init();

	/* Initialize timers and TSC timekeeping */
	apic_timer_init();

	/* Initialize the VDSO now */
	vdso_init();

	x86_init_percpu();

	/* Setup the x86 platform defaults */
	x86_platform.has_legacy_devices = true;
	x86_platform.i8042 = I8042_EXPECTED_PRESENT;
	x86_platform.has_msi = true;
	x86_platform.has_rtc = true;
	x86_platform.has_vga = true;

	cpu_init_mp();
}

INIT_LEVEL_EARLY_PLATFORM_ENTRY(cpu_init_late);

extern PML *boot_pml4;

extern "C"
void smpboot_main(unsigned long gs_base, volatile struct smp_header *header)
{
	lapic_init_per_cpu();

	sched_enable_pulse();

	tss_init();

	x86_init_percpu();

	booted_cpus++;

	/* Enable interrupts */
	ENABLE_INTERRUPTS();

	header->boot_done = true;

	sched_transition_to_idle();
}

unsigned int get_nr_cpus(void)
{
	return booted_cpus;
}

static void rep_movsb(void *dst, const void *src, size_t n)
{
    __asm__ __volatile__ ("rep movsb\n\t"
                         : "+D" (dst), "+S" (src), "+c" (n)
                         :
                         : "memory");
}

void *memcpy_fast(void *dst, void *src, size_t n)
{
	rep_movsb(dst, src, n);
	return dst;
}

bool is_kernel_ip(uintptr_t ip)
{
	return ip >= VM_HIGHER_HALF;
}

void cpu_notify(unsigned int cpu_nr)
{
	apic_send_ipi(apic_get_lapic_id(cpu_nr), 0, X86_MESSAGE_VECTOR);
}

void cpu_wait_for_msg_ack(volatile struct cpu_message *msg)
{
	while(!msg->ack)
		cpu_relax();
	msg->ack = false;
}

PER_CPU_VAR(struct spinlock msg_queue_lock);
PER_CPU_VAR(struct list_head message_queue);

void cpu_messages_init(unsigned int cpu)
{
	struct list_head *h = get_per_cpu_ptr_any(message_queue, cpu);
	INIT_LIST_HEAD(h);

	struct spinlock *l = get_per_cpu_ptr_any(msg_queue_lock, cpu);
	spinlock_init(l);

	do_init_level_percpu(INIT_LEVEL_CORE_PERCPU_CTOR, cpu);
}

void cpu_send_resched(unsigned int cpu)
{
	apic_send_ipi(apic_get_lapic_id(cpu), 0, X86_RESCHED_VECTOR);
}

void cpu_send_sync_notif(unsigned int cpu)
{
	apic_send_ipi(apic_get_lapic_id(cpu), 0, X86_SYNC_CALL_VECTOR);
}

bool cpu_send_message(unsigned int cpu, unsigned long message, void *arg, bool should_wait)
{
	assert(cpu <= booted_cpus);
	struct spinlock *message_queue_lock = get_per_cpu_ptr_any(msg_queue_lock, cpu);
	struct list_head *queue = get_per_cpu_ptr_any(message_queue, cpu);

	struct cpu_message msg;
	msg.message = message;
	msg.ptr = arg;
	msg.sent = true;
	msg.ack = false;
	
	INIT_LIST_HEAD(&msg.node);

	unsigned long cpu_flags = spin_lock_irqsave(message_queue_lock);

	list_add_tail(&msg.node, queue);

	spin_unlock_irqrestore(message_queue_lock, cpu_flags);

	cpu_notify(cpu);

	cpu_wait_for_msg_ack((volatile struct cpu_message *) &msg);

	return true;
}

void cpu_kill(int cpu_num)
{
	printf("Killing cpu %u\n", cpu_num);
	cpu_send_message(cpu_num, CPU_KILL, NULL, false);
}

void cpu_kill_other_cpus(void)
{
	unsigned int curr_cpu = get_cpu_nr();
	for(unsigned int i = 0; i < booted_cpus; i++)
	{
		if(i != curr_cpu)
			cpu_kill(i);
	}
}

void cpu_handle_kill(void)
{
	halt();
}

unsigned long total_resched = 0;
unsigned long success_resched = 0;

void cpu_try_resched(void)
{
	__sync_add_and_fetch(&total_resched, 1);
	sched_should_resched();
}

void cpu_handle_message(struct cpu_message *msg)
{
	unsigned int this_cpu = get_cpu_nr();
	const char *str = "";
	switch(msg->message)
	{
		case CPU_KILL:
			str = "CPU_KILL";
			msg->ack = true;
			cpu_handle_kill();
			break;
		case CPU_TRY_RESCHED:
			str = "CPU_TRY_RESCHED";
			cpu_try_resched();
			msg->ack = true;
			break;
	}

	(void) this_cpu;
	(void) str;
	//printf("cpu#%u handling %p, message type %s\n", this_cpu, msg, str);

}

void *cpu_handle_messages(void *stack)
{
	struct spinlock *cpu_msg_lock = get_per_cpu_ptr(msg_queue_lock);
	struct list_head *list = get_per_cpu_ptr(message_queue);

	unsigned long cpu_flags = spin_lock_irqsave(cpu_msg_lock);

	list_for_every_safe(list)
	{
		struct cpu_message *msg = container_of(l, struct cpu_message, node);

		list_remove(l);

		COMPILER_BARRIER();

		cpu_handle_message(msg);
	}

	spin_unlock_irqrestore(cpu_msg_lock, cpu_flags);
	
	return stack;
}

void *cpu_resched(void *stack)
{
	cpu_try_resched();

	if(sched_needs_resched(get_current_thread()))
	{
		stack = sched_preempt_thread(stack);
	}

	return stack;
}

static const uint8_t stac[] = {0x0f, 0x01, 0xcb};
static const uint8_t clac[] = {0x0f, 0x01, 0xca};

extern "C" void x86_smap_stac_patch(code_patch_location *loc)
{
	if(x86_has_cap(X86_FEATURE_SMAP))
		ktrace::replace_instructions(loc->address, stac, 3, loc->size);
	else
	{
		ktrace::nop_out(loc->address, loc->size);
	}
	
}

extern "C" void x86_smap_clac_patch(code_patch_location *loc)
{
	if(x86_has_cap(X86_FEATURE_SMAP))
		ktrace::replace_instructions(loc->address, clac, 3, loc->size);
	else
	{
		ktrace::nop_out(loc->address, loc->size);
	}
}
