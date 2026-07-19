#include "riscv-virt.h"

#define STACK_PER_HART 0x40000   /* 256 KiB per hart */

	.section .init
	.globl _start
	.type _start,@function
_start:
	.cfi_startproc
	.cfi_undefined ra
.option push
.option norelax
	la  gp, __global_pointer$
.option pop

	csrr a0, mhartid

	// Per-hart stack: sp = _stack_top - mhartid * STACK_PER_HART
	la sp, _stack_top
	li t0, STACK_PER_HART
	mul t1, a0, t0
	sub sp, sp, t1

	// Hart 0 initializes .data/.bss; secondaries spin until it signals.
	bnez a0, secondary_wait

	// ----- Hart 0: copy .data from LMA -----
	la a0, _data_lma
	la a1, _data
	la a2, _edata
	bgeu a1, a2, 2f
1:
	LOAD t0, (a0)
	STOR t0, (a1)
	addi a0, a0, REGSIZE
	addi a1, a1, REGSIZE
	bltu a1, a2, 1b
2:

	// ----- Hart 0: clear .bss -----
	la a0, _bss
	la a1, _ebss
	bgeu a0, a1, 2f
1:
	STOR zero, (a0)
	addi a0, a0, REGSIZE
	bltu a0, a1, 1b
2:

	fence rw, rw
	la t0, hart0_init_done
	li t1, 1
	sw t1, 0(t0)
	j enter_main

secondary_wait:
	la t0, hart0_init_done
3:
	lw t1, 0(t0)
	beqz t1, 3b
	fence r, r

enter_main:
	li  a0, 0
	li  a1, 0
	li  a2, 0
	li  t6, (3<<13) | (3<<11) | (1<<7) | (1<<3)   // MPP=M, MPIE, MIE, FS=dirty
	csrw mstatus, t6

	jal main
1:
	wfi
	j 1b
	.cfi_endproc

	.section .data
	.align 3
	.global hart0_init_done
hart0_init_done:
	.word 0
