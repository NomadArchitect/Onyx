riscv-y:= image.o fpu.o copy_user.o stubs.o vdso_helper.o early_paging.o entry.o virt-uart.o mmu.o interrupts.o traps.o usercopy.o sbi.o \
          power.o time.o scheduler.o debug.o signal.o smp.o syscall.o process.o cpu.o fpu_state.o plic.o code_patch.o

obj-y+= $(patsubst %, arch/riscv64/%, $(riscv-y))

arch/riscv64/vdso_helper.o: arch/riscv64/vdso_helper.S onyx-vdso.so.0 .config
		$(CC) -c $(CFLAGS) $(CPPFLAGS) -mno-relax -o $@ $<
