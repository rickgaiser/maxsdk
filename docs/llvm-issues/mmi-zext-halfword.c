/* Issue 18: four bytes zero-extended to words. The byte step is one PEXTLB
   with $zero; the halfword step (data in the low halves, zeros above) goes
   through the stack (SQ, 4 x LHU + 8 x SH, LQ) instead of PEXTLH rd, $zero, rs.
   The signed variant with the zeros below (n32: n << 24) is PEXTLB + PEXTLH.
   clang --target=mips64el -mcpu=r5900 -mabi=n32 -O2 -S mmi-zext-halfword.c -o - */
typedef int s32x4 __attribute__((vector_size(16)));
typedef short s16x8 __attribute__((vector_size(16)));
typedef unsigned char u8x16 __attribute__((vector_size(16)));

s32x4 u8_to_u32(const int *v) {
  u8x16 b = (u8x16)(s32x4){*v, 0, 0, 0}, z = {0};
  s16x8 h = (s16x8)__builtin_shufflevector(b, z, 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22,
                                           7, 23),
        hz = {0};
  return (s32x4)__builtin_shufflevector(h, hz, 0, 8, 1, 9, 2, 10, 3, 11);
}
