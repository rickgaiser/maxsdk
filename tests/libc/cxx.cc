// Phase 8: C++ on maxsdk. Static constructors (in order), virtual calls,
// new/delete, a guarded local static, templates, and a static destructor
// at exit, which prints the end marker.

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c)                                                                           \
  do {                                                                                     \
    if (!(c)) {                                                                            \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                  \
      fails++;                                                                             \
    }                                                                                      \
  } while (0)

static int order[4], norder;

struct Rec {
  int id;
  Rec(int i) : id(i) { order[norder++] = i; }
};
static Rec r1(1), r2(2);

struct Shape {
  virtual ~Shape() {}
  virtual int area() const = 0;
};
struct Sq : Shape {
  int s;
  explicit Sq(int s) : s(s) {}
  int area() const override { return s * s; }
};
struct Rect : Shape {
  int w, h;
  Rect(int w, int h) : w(w), h(h) {}
  int area() const override { return w * h; }
};

template <typename T, int N> struct Ring {
  T v[N];
  int n = 0;
  void push(T x) { v[n++ % N] = x; }
  T sum() const {
    T s = 0;
    for (int i = 0; i < (n < N ? n : N); i++)
      s += v[i];
    return s;
  }
};

static int made;
static int &counter() {
  static int c = ++made; // constructed once
  return c;
}

struct AtEnd {
  ~AtEnd() {
    printf("cxx: static destructor ran, %s\n", fails ? "FAILED" : "passed");
    printf("@@PS2TEST_END@@ status=%d\n", fails ? 1 : 0);
  }
};
static AtEnd at_end;

int main() {
  CHECK(norder == 2 && order[0] == 1 && order[1] == 2);
  Shape *s[3] = {new Sq(3), new Rect(2, 5), new Sq(4)};
  int total = 0;
  for (Shape *x : s)
    total += x->area();
  CHECK(total == 9 + 10 + 16);
  for (Shape *x : s)
    delete x;
  int *a = new int[1000];
  for (int i = 0; i < 1000; i++)
    a[i] = i;
  CHECK(a[999] == 999);
  delete[] a;
  Ring<float, 4> ring;
  for (int i = 1; i <= 6; i++)
    ring.push((float)i);
  CHECK(ring.sum() == 5 + 6 + 3 + 4);
  counter();
  counter();
  CHECK(made == 1);
  printf("cxx: total area %d, ring %g\n", total, (double)ring.sum());
  return 0;
}
