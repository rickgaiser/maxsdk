# The vector of the CPU exceptions that mx_exception_init installs. The
# kernel's level 1 handler jumps here with only k0 and k1 changed. Save the
# registers, HI/LO, the FPU and COP0 state, then return from the exception into
# mx__exception_report on its own stack: there, interrupts and the SIF work,
# so the report reaches the host.

	.set noreorder
	.set noat
	.text

	.globl mx__exception_vector
	.type mx__exception_vector, @function
	.ent mx__exception_vector
mx__exception_vector:
	lui $26, %hi(mx__exception_state)
	addiu $26, $26, %lo(mx__exception_state)
	sq $0, 0($26)
	sq $1, 16($26)
	sq $2, 32($26)
	sq $3, 48($26)
	sq $4, 64($26)
	sq $5, 80($26)
	sq $6, 96($26)
	sq $7, 112($26)
	sq $8, 128($26)
	sq $9, 144($26)
	sq $10, 160($26)
	sq $11, 176($26)
	sq $12, 192($26)
	sq $13, 208($26)
	sq $14, 224($26)
	sq $15, 240($26)
	sq $16, 256($26)
	sq $17, 272($26)
	sq $18, 288($26)
	sq $19, 304($26)
	sq $20, 320($26)
	sq $21, 336($26)
	sq $22, 352($26)
	sq $23, 368($26)
	sq $24, 384($26)
	sq $25, 400($26)
	sq $28, 448($26)
	sq $29, 464($26)
	sq $30, 480($26)
	sq $31, 496($26)
	mfc0 $27, $12
	sw $27, 512($26)
	mfc0 $27, $13
	sw $27, 516($26)
	mfc0 $27, $14
	sw $27, 520($26)
	mfc0 $27, $8
	sw $27, 524($26)
	mfc0 $27, $23
	sw $27, 528($26)
	# HI and LO (pipeline 0). The GPRs are saved, so any register is free now.
	mfhi $8
	sd $8, 536($26)
	mflo $8
	sd $8, 544($26)
	# The FPU registers, only when COP1 is usable (Status.CU1): otherwise
	# touching them raises a coprocessor unusable exception here.
	sw $0, 556($26)
	lui $9, 0x2000
	mfc0 $8, $12
	and $8, $8, $9
	beqz $8, 1f
	nop
	cfc1 $8, $31
	sw $8, 552($26)
	addiu $8, $0, 1
	sw $8, 556($26)
	swc1 $f0, 560($26)
	swc1 $f1, 564($26)
	swc1 $f2, 568($26)
	swc1 $f3, 572($26)
	swc1 $f4, 576($26)
	swc1 $f5, 580($26)
	swc1 $f6, 584($26)
	swc1 $f7, 588($26)
	swc1 $f8, 592($26)
	swc1 $f9, 596($26)
	swc1 $f10, 600($26)
	swc1 $f11, 604($26)
	swc1 $f12, 608($26)
	swc1 $f13, 612($26)
	swc1 $f14, 616($26)
	swc1 $f15, 620($26)
	swc1 $f16, 624($26)
	swc1 $f17, 628($26)
	swc1 $f18, 632($26)
	swc1 $f19, 636($26)
	swc1 $f20, 640($26)
	swc1 $f21, 644($26)
	swc1 $f22, 648($26)
	swc1 $f23, 652($26)
	swc1 $f24, 656($26)
	swc1 $f25, 660($26)
	swc1 $f26, 664($26)
	swc1 $f27, 668($26)
	swc1 $f28, 672($26)
	swc1 $f29, 676($26)
	swc1 $f30, 680($26)
	swc1 $f31, 684($26)
1:
	lui $29, %hi(mx__exception_stack + 8192)
	addiu $29, $29, %lo(mx__exception_stack + 8192)
	lui $27, %hi(mx__exception_report)
	addiu $27, $27, %lo(mx__exception_report)
	mtc0 $27, $14
	sync.p
	eret
	nop
	.end mx__exception_vector
	.size mx__exception_vector, .-mx__exception_vector
