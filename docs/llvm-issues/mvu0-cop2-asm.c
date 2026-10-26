/* Issues 16 and 17: VU0 micro mode from EE code.
   clang --target=mips64el -mcpu=r5900 -mabi=n32 -mvu0 -O2 -c mvu0-cop2-asm.c

   16: the assembler accepts QMTC2, QMFC2, LQC2, SQC2 and VNOP, but not
   VCALLMS, VCALLMSR, or CFC2 / CTC2 with a VU0 control register (vi16..vi31:
   VPU-STAT vi29, CMSAR0 vi27, FBRST vi28) or a VI register. maxsdk writes
   them as .word (vcallms imm: 0x4a000038 | imm << 6; cfc2.i $8, vi29:
   0x4848e801).

   17: a register-fed micro program keeps its inputs and results in fixed
   VF registers between the asm statements (here vf1..vf6); nothing tells
   the compiler that they are live there, so -mvu0 code in between may use
   them. A clobber only covers its own statement; there is no -ffixed-vfN. */
void call(const void *in, void *out) {
  __asm__ volatile("lqc2 $vf1, 0(%0)" : : "r"(in) : "$vf1");
  __asm__ volatile("vcallms 0");             /* error: unknown instruction */
  __asm__ volatile("cfc2.i $8, $vi29" ::: "$8"); /* error: invalid operand */
  __asm__ volatile("ctc2 $8, $vi27" ::: "$8");   /* error: invalid operand */
  __asm__ volatile("vcallmsr $vi27");        /* error: unknown instruction */
  __asm__ volatile("sqc2 $vf5, 0(%0)" : : "r"(out) : "memory");
}
