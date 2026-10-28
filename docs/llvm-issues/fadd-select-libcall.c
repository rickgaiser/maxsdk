/* llvm-issues 26: a conditional float add becomes __addsf3 on the R5900.
   clang --target=mips64el -mcpu=r5900 -mabi=n32 -O2 -ffinite-math-only -S
   InstCombine folds select(c, r, r + bit) into r + select(c, -0.0, bit); the
   new fadd has no nnan/ninf (the select has no fast-math flags), and the
   R5900 lowers an fadd without them to a call of __addsf3. */
float f(float m, float r) {
  float bit = 0.5f;
  for (int i = 0; i < 8; i++, bit *= 0.5f) {
    m *= m;
    if (m >= 2.0f)
      m *= 0.5f, r += bit;
  }
  return r;
}
