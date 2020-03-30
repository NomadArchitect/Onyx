/*
* Copyright (c) 2016, 2017 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <xmmintrin.h>

#include <signal.h>

#include <onyx/disassembler.h>
#include <onyx/process.h>
#include <onyx/signal.h>
#include <onyx/task_switching.h>
#include <onyx/vm.h>
#include <onyx/panic.h>
#include <onyx/compiler.h>
#include <onyx/x86/mce.h>
#include <onyx/cpu.h>
#include <onyx/atomic.h>
#include <onyx/percpu.h>
#include <onyx/exceptions.h>
#include <onyx/x86/ktrace.h>

const char* exception_msg[] = {
    "Division by zero exception",
    "Debug Trap",
    "Non-maskable interrupt",
    "Hit a breakpoint",
    "Overflow trap",
    "Fault: Bound range exceeded",
    "Invalid Instruction",
    "FPU not detected",
    "Critical error: DOUBLE FAULT",
    "Invalid TSS",
    "Segment not present",
    "Stack segment fault",
    "General Protection Fault",
    "Page fault at ",
    "",
    "x87 floating point exception",
    "Alignment check exception",
    "Machine check exception",
    "SIMD floating point exception",
    "Virtualization exception",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "Security exception"
};

void dump_interrupt_context(struct registers *ctx)
{
	printk("Exception at %016lx\n", ctx->rip);
	printk("Registers: rax: %016lx\nrbx: %016lx\nrcx: %016lx\nrdx: %016lx\n"
	       "rdi: %016lx\nrsi: %016lx\nrbp: %016lx\nr8:  %016lx\nr9:  %016lx\n"
	       "r10: %016lx\nr11: %016lx\nr12: %016lx\nr13: %016lx\nr14: %016lx\n"
	       "r15: %016lx\nrsp: %016lx\nrflags: %016lx\nds: %016lx\ncs: %016lx\n", 
	ctx->rax, ctx->rbx, ctx->rcx, ctx->rdx, ctx->rdi, ctx->rsi, ctx->rbp, 
	ctx->r8, ctx->r9, ctx->r10, ctx->r11, ctx->r12, ctx->r13, ctx->r14, 
	ctx->r15, ctx->rsp, ctx->rflags, ctx->ds, ctx->cs);

	if(ctx->cs == 0x08) stack_trace_ex((uint64_t *) ctx->rbp);
}

static bool is_kernel_exception(struct registers *ctx)
{
	/* We can't use %rip here since, for example, in a page fault, %rip is
	 * set to the target address before the page fault actually triggers.
	 * User-space cannot set cs.
	*/
	return ctx->cs == 0x08;
}

void div0_exception(struct registers *ctx)
{
	if(is_kernel_exception(ctx))
	{
		dump_interrupt_context(ctx);
		panic("Divide by zero exception");
	}

	struct thread *current = get_current_thread();
	siginfo_t info = {};
	info.si_code = SI_KERNEL;

	kernel_tkill(SIGFPE, current, SIGNAL_FORCE, &info);
}

void setup_debug_register(unsigned long addr, unsigned int size, unsigned int condition);

void debug_trap(struct registers *ctx)
{
	if(is_kernel_exception(ctx))
	{
		dump_interrupt_context(ctx);
		panic("Debug trap");
	}

	//printk("Trap at %lx rbp %lx!\n", ctx->rip, ctx->rbp);

	struct thread *current = get_current_thread();

	siginfo_t info = {};
	info.si_code = SI_KERNEL;

	kernel_tkill(SIGTRAP, current, SIGNAL_FORCE, &info);
}

void nmi_exception(struct registers *ctx)
{
	dump_interrupt_context(ctx);
	/* TODO: Handle this in a better, less destructive and useful fashion */
	panic("Unexpected NMI exception\n");
}

void overflow_trap(struct registers *ctx)
{
	if(is_kernel_exception(ctx))
	{
		dump_interrupt_context(ctx);
		panic("Overflow trap");
	}

	struct thread *current = get_current_thread();

	siginfo_t info = {};
	info.si_code = SI_KERNEL;

	kernel_tkill(SIGSEGV, current, SIGNAL_FORCE, &info);
}

void boundrange_exception(struct registers *ctx)
{
	if(is_kernel_exception(ctx))
	{
		dump_interrupt_context(ctx);
		panic("Bound range trap");
	}

	struct thread *current = get_current_thread();

	siginfo_t info = {};
	info.si_code = SI_KERNEL;

	kernel_tkill(SIGILL, current, SIGNAL_FORCE, &info);
}

void invalid_opcode_exception(struct registers *ctx)
{
	if(is_kernel_exception(ctx))
	{
		dump_interrupt_context(ctx);
		panic("Invalid instruction exception");
	}

	struct thread *current = get_current_thread();

	siginfo_t info = {};
	info.si_code = SI_KERNEL;

	kernel_tkill(SIGILL, current, SIGNAL_FORCE, &info);
}

void device_not_avail_excp(struct registers *ctx)
{
	/* We don't support FPU lazy switching right now, so we just send SIGFPE
	 * to the process
	*/
	if(is_kernel_exception(ctx))
	{
		dump_interrupt_context(ctx);
		panic("FPU exception");
	}

	struct thread *current = get_current_thread();

	siginfo_t info = {};
	info.si_code = SI_KERNEL;

	kernel_tkill(SIGFPE, current, SIGNAL_FORCE, &info);
}
void __double_fault(struct registers *ctx)
{
	dump_interrupt_context(ctx);
	panic("Double fault");
}

void exception_panic(struct registers *ctx)
{
	dump_interrupt_context(ctx);
	panic("Misc/Unknown exception triggered.");
}

void stack_segment_fault(struct registers *ctx)
{
	if(is_kernel_exception(ctx))
	{
		dump_interrupt_context(ctx);
		panic("Stack segment fault");
	}

	struct thread *current = get_current_thread();

	siginfo_t info = {};
	info.si_code = SI_KERNEL;

	kernel_tkill(SIGSEGV, current, SIGNAL_FORCE, &info);
}

void general_protection_fault(struct registers *ctx)
{
	if(is_kernel_exception(ctx))
	{
		unsigned long fixup;
		if((fixup = exceptions_get_fixup(ctx->rip)) != NO_FIXUP_EXISTS)
		{
			ctx->rip = fixup;
			return;
		}

		dump_interrupt_context(ctx);
		printk("GPF error code: %04x\n", (uint16_t) ctx->int_err_code);
		panic("General protection fault");
	}

	struct thread *current = get_current_thread();
	(void) current;
	dump_interrupt_context(ctx);
	printk("GPF error code: %04x\n", (uint16_t) ctx->int_err_code);

	siginfo_t info = {};
	info.si_code = SI_KERNEL;

	kernel_tkill(SIGSEGV, current, SIGNAL_FORCE, &info);
}

void stack_trace_user(uintptr_t *stack);

void page_fault_handler(struct registers *ctx)
{
	uintptr_t fault_address = cpu_get_cr2();
	uint16_t error_code = ctx->int_err_code;

	struct fault_info info;
	info.fault_address = fault_address;
	info.write = error_code & 0x2;
	info.read = info.write ? 0 : 1;
	info.exec = error_code & 0x10;
	info.user = error_code & 0x4;
	info.ip = ctx->rip;

	if(vm_handle_page_fault(&info) < 0)
	{
		if(!info.user)
		{
			unsigned long fixup;
			if((fixup = exceptions_get_fixup(info.ip)) != NO_FIXUP_EXISTS)
			{
				ctx->rip = fixup;
				return;
			}
		}
		
		vm_do_fatal_page_fault(&info);
	}
}

void x87_fpu_exception(struct registers *ctx)
{
	/* We don't support FPU lazy switching right now, so we just send SIGFPE
	 * to the process
	*/
	if(is_kernel_exception(ctx))
	{
		dump_interrupt_context(ctx);
		panic("FPU exception");
	}

	struct thread *current = get_current_thread();

	siginfo_t info = {};
	info.si_code = SI_KERNEL;

	kernel_tkill(SIGFPE, current, SIGNAL_FORCE, &info);
}

void alignment_check_excp(struct registers *ctx)
{
	if(is_kernel_exception(ctx))
	{
		dump_interrupt_context(ctx);
		panic("Alignment check exception");
	}

	struct thread *current = get_current_thread();

	siginfo_t info = {};
	info.si_code = SI_KERNEL;

	kernel_tkill(SIGSEGV, current, SIGNAL_FORCE, &info);
}

void simd_fpu_exception(struct registers *ctx)
{
	/* We don't support FPU lazy switching right now, so we just send SIGFPE
	 * to the process
	*/
	if(is_kernel_exception(ctx))
	{
		dump_interrupt_context(ctx);
		panic("FPU exception");
	}

	struct thread *current = get_current_thread();

	siginfo_t info = {};
	info.si_code = SI_KERNEL;

	kernel_tkill(SIGFPE, current, SIGNAL_FORCE, &info);
}

void virtualization_exception(struct registers *ctx)
{
	if(is_kernel_exception(ctx))
	{
		dump_interrupt_context(ctx);
		panic("Virtualization exception");
	}

	struct thread *current = get_current_thread();

	siginfo_t info = {};
	info.si_code = SI_KERNEL;

	kernel_tkill(SIGSEGV, current, SIGNAL_FORCE, &info);
}

void security_exception(struct registers *ctx)
{
	if(is_kernel_exception(ctx))
	{
		dump_interrupt_context(ctx);
		panic("Security exception");
	}

	struct thread *current = get_current_thread();

	siginfo_t info = {};
	info.si_code = SI_KERNEL;

	kernel_tkill(SIGSEGV, current, SIGNAL_FORCE, &info);
}

#ifdef CONFIG_KTRACE

bool ktrace_enabled_int3 = false;

void ktrace_enable_int3(void)
{
	ktrace_enabled_int3 = true;
}

void ktrace_disable_int3(void)
{
	ktrace_enabled_int3 = true;
}
#endif

void breakpoint_exception(struct registers *ctx)
{
	if(is_kernel_exception(ctx))
	{
#ifdef CONFIG_KTRACE
		if(ktrace_enabled_int3)
		{
			ktrace_int3_handler(ctx);
			return;
		}
#endif
		dump_interrupt_context(ctx);
		panic("Breakpoint exception");
	}

	struct thread *current = get_current_thread();

	siginfo_t info = {};
	info.si_code = SI_KERNEL;

	kernel_tkill(SIGTRAP, current, SIGNAL_FORCE, &info);
}

void invalid_tss_exception(struct registers *ctx)
{
	dump_interrupt_context(ctx);
	panic("Invalid TSS exception");
}

void segment_not_present_excp(struct registers *ctx)
{
	if(is_kernel_exception(ctx))
	{
		dump_interrupt_context(ctx);
		panic("Segment not present exception");
	}

	struct thread *current = get_current_thread();

	siginfo_t info = {};
	info.si_code = SI_KERNEL;

	kernel_tkill(SIGSEGV, current, SIGNAL_FORCE, &info);
}

void machine_check(struct registers *ctx)
{
	do_machine_check(ctx);
}

void (* const int_handlers[])(struct registers *ctx) = 
{
	div0_exception,
	debug_trap,
	nmi_exception,
	breakpoint_exception,
	overflow_trap,
	boundrange_exception,
	invalid_opcode_exception,
	device_not_avail_excp,
	__double_fault,
	exception_panic,
	invalid_tss_exception,
	segment_not_present_excp,
	stack_segment_fault,
	general_protection_fault,
	page_fault_handler,
	exception_panic,
	x87_fpu_exception,
	alignment_check_excp,
	machine_check,
	simd_fpu_exception,
	virtualization_exception,
	exception_panic,
	exception_panic,
	exception_panic,
	exception_panic,
	exception_panic,
	exception_panic,
	exception_panic,
	exception_panic,
	exception_panic,
	security_exception,
	exception_panic
};

PER_CPU_VAR(struct spinlock isr_lock);
uint32_t in_isr = 0;

static void exit_isr_handler(void)
{
	in_isr--;
	//spin_unlock(&GET_PER_CPU(isr_lock, struct spinlock));
}

static void enter_isr_handler(void)
{
	if(in_isr > 10) { halt(); }
	in_isr++;
	//spin_lock(&GET_PER_CPU(isr_lock, struct spinlock));
}

void dump_stack(uintptr_t *__rsp)
{
	unsigned char *rsp = (unsigned char *) __rsp;
	printk("Stack dump: ");
	/* Lets dump a comfortable number of bytes */
	for(int i = 0; i < 20; i++, rsp--)
		printk("%02x ", *rsp);
	printk("\n");
}

void isr_handler(struct registers *ctx)
{
	int int_no = ctx->int_no;

	//enter_isr_handler();

	irq_restore(ctx->rflags);
	int_handlers[int_no](ctx);

	//exit_isr_handler();
}
