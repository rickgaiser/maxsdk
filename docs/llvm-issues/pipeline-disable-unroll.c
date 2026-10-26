void a(float *x, int n) {
#pragma clang loop unroll_count(2) pipeline(disable)
  for (int i = 0; i < n; i++) x[i] *= 2;
}
void b(float *x, int n) {
#pragma clang loop pipeline(disable)
#pragma clang loop unroll_count(2)
  for (int i = 0; i < n; i++) x[i] *= 2;
}
#define L _Pragma("clang loop unroll_count(2) pipeline(disable)")
void c(float *x, int n) {
  L
  for (int i = 0; i < n; i++) x[i] *= 2;
}
