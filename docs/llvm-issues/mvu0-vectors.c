typedef float f4 __attribute__((vector_size(16)));
typedef int i4 __attribute__((vector_size(16)));
typedef unsigned char u8x4 __attribute__((vector_size(4)));
typedef signed char s8x4 __attribute__((vector_size(4)));
/* 1: a scalar float into a VU0 vector (broadcast operand) */
f4 scal(f4 m, float w) { return m * w; }
/* 2: an int vector from four GPR values */
f4 build(f4 m, unsigned b) {
  i4 v = {(int)(b & 255), (int)(b >> 8 & 255), (int)(b >> 16 & 255), (int)(b >> 24)};
  return m * __builtin_convertvector(v, f4);
}
/* 3: bytes to floats */
f4 bytes_u(f4 m, const u8x4 *p) { return m * __builtin_convertvector(*p, f4); }
f4 bytes_s(f4 m, const s8x4 *p) { return m * __builtin_convertvector(*p, f4); }
/* 4: lanes of a VU0 vector stored as floats */
void lanes(float *o, f4 r) { o[0] = r[0], o[1] = r[1], o[2] = r[2]; }
/* 5: float vector to bytes */
void tobytes(s8x4 *o, f4 r) { *o = __builtin_convertvector(__builtin_convertvector(r, i4), s8x4); }
