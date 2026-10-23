# Entry point. The loader (ps2link, the BIOS, PCSX2 -elf) jumps here with
# a0 = its argument block (ps2link passes { pid; argc; argv[16]; payload }),
# or garbage when there is none.

	.set noreorder
	.set noat
	.section .text.start, "ax", @progbits
	.globl __start
	.type __start, @function
	.ent __start
__start:
	move $16, $4                # keep the loader argument across the BSS clear

	# Clear BSS (16-byte aligned by the linker script).
	la $2, __bss_start
	la $3, __bss_end
1:	sltu $1, $2, $3
	beq $1, $0, 2f
	nop
	sq $0, 0($2)
	b 1b
	addiu $2, $2, 16
2:
	la $2, mx__loader_args
	sw $16, 0($2)

	# SetupThread(gp, stack, stack_size, args, root): the kernel sets up the
	# main thread, fills args with argc/argv and returns the stack pointer.
	la $4, _gp
	la $5, __stack
	la $6, __stack_size
	la $7, mx__kernel_args
	la $8, mx_bios_exit_thread
	move $28, $4
	li $3, 0x3c
	syscall
	move $29, $2

	j mx__main
	nop
	.end __start
	.size __start, .-__start
