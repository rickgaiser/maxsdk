/* Small printf: %d %i %u %x %X %o %p %s %c %f %e %g %%, with flags - 0 + space,
   width, precision (also *), and the length modifiers hh h l ll z. It uses no
   floating-point arithmetic and no compiler runtime. */

#include <mx/debug.h>
#include <mx/string.h>

struct out {
  char *buf;
  u32 size, len;
};

static void put(struct out *o, char c) {
  if (o->len + 1 < o->size)
    o->buf[o->len] = c;
  o->len++;
}

static void pad(struct out *o, char c, int n) {
  while (n-- > 0)
    put(o, c);
}

#define F_LEFT 1
#define F_ZERO 2
#define F_PLUS 4
#define F_SPACE 8

static void emit(struct out *o, const char *pre, const char *s, int n, int width, int flags) {
  int np = (int)strlen(pre);
  int fill = width - n - np;
  if (!(flags & (F_LEFT | F_ZERO)))
    pad(o, ' ', fill);
  while (*pre)
    put(o, *pre++);
  if ((flags & (F_LEFT | F_ZERO)) == F_ZERO)
    pad(o, '0', fill);
  for (int i = 0; i < n; i++)
    put(o, s[i]);
  if (flags & F_LEFT)
    pad(o, ' ', fill);
}

static int utoa(char *end, u64 v, int base, int upper) {
  const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  int n = 0;
  do {
    *--end = dig[v % (u64)base];
    v /= (u64)base;
    n++;
  } while (v);
  return n;
}

static const char *sign_prefix(int neg, int flags) {
  return neg ? "-" : (flags & F_PLUS) ? "+" : (flags & F_SPACE) ? " " : "";
}

/* Floating point without floating-point arithmetic: the double is taken
   apart into m * 2^e (m kept in [2^59, 2^60)) and scaled by 10 with 64-bit
   integer operations. About 17 significant digits are exact. */

struct fp {
  u64 m;
  int e;
};

static void fp_norm(struct fp *f) {
  while (f->m >= (1ull << 60))
    f->m >>= 1, f->e++;
  while (f->m < (1ull << 59))
    f->m <<= 1, f->e--;
}

static int fp_ge(struct fp a, struct fp b) {
  return a.e != b.e ? a.e > b.e : a.m >= b.m;
}

/* n significant digits of f (f > 0) into d, returns the decimal exponent of
   the first digit. */
static int fp_digits(struct fp f, char *d, int n) {
  const struct fp ten = {10ull << 56, -56}, one = {1ull << 59, -59};
  int x = 0;
  while (fp_ge(f, ten)) {
    f.m = (f.m << 4) / 10;
    f.e -= 4;
    fp_norm(&f);
    x++;
  }
  while (!fp_ge(f, one)) {
    f.m *= 10;
    fp_norm(&f);
    x--;
  }
  int sh = -f.e;
  u64 mask = (1ull << sh) - 1;
  u64 frac = f.m & mask;
  d[0] = (char)('0' + (f.m >> sh));
  for (int i = 1; i < n; i++) {
    frac *= 10;
    d[i] = (char)('0' + (frac >> sh));
    frac &= mask;
  }
  if (((frac * 10) >> sh) >= 5) { /* round half up */
    int i = n - 1;
    while (i >= 0 && d[i] == '9')
      d[i--] = '0';
    if (i >= 0) {
      d[i]++;
    } else {
      d[0] = '1';
      x++;
    }
  }
  return x;
}

#define MAXD 40

static int fmt_e(char *buf, struct fp f, int zero, int prec, int upper) {
  char d[MAXD];
  int n = prec + 1 > MAXD ? MAXD : prec + 1;
  int x = 0;
  if (zero)
    memset(d, '0', (size_t)n);
  else
    x = fp_digits(f, d, n);
  int len = 0;
  buf[len++] = d[0];
  if (prec) {
    buf[len++] = '.';
    for (int i = 1; i <= prec; i++)
      buf[len++] = i < n ? d[i] : '0';
  }
  buf[len++] = upper ? 'E' : 'e';
  buf[len++] = x < 0 ? '-' : '+';
  if (x < 0)
    x = -x;
  if (x >= 100)
    buf[len++] = (char)('0' + x / 100);
  buf[len++] = (char)('0' + x / 10 % 10);
  buf[len++] = (char)('0' + x % 10);
  return len;
}

static int fmt_f(char *buf, struct fp f, int zero, int prec) {
  char d[MAXD];
  int x = -1, n = 0;
  if (!zero) {
    /* digits up to the last one after the point; the exponent decides how
       many that is, and rounding can move it by one */
    x = fp_digits(f, d, 1);
    n = x + 1 + prec;
    if (n > MAXD)
      return -1;
    if (n > 0) {
      x = fp_digits(f, d, n);
      n = x + 1 + prec;
      if (n > MAXD)
        return -1;
    } else if (n == 0 && d[0] >= '5') { /* rounds up to the last place */
      d[0] = '1';
      x = -prec;
      n = 1;
    }
  }
  int len = 0;
  if (x < 0)
    buf[len++] = '0';
  for (int i = 0; i <= x; i++)
    buf[len++] = i < n ? d[i] : '0';
  if (prec) {
    buf[len++] = '.';
    for (int i = 1; i <= prec; i++) {
      int k = x + i; /* index of the digit for 10^-i */
      buf[len++] = k >= 0 && k < n ? d[k] : '0';
    }
  }
  return len;
}

static void fmt_float(struct out *o, double v, char conv, int prec, int width, int flags) {
  char buf[96];
  union {
    double d;
    u64 u;
  } b = {v};
  int neg = (int)(b.u >> 63);
  int expo = (int)(b.u >> 52) & 0x7FF;
  u64 man = b.u & ((1ull << 52) - 1);
  const char *pre = sign_prefix(neg, flags);
  if (expo == 0x7FF) {
    emit(o, pre, man ? "nan" : "inf", 3, width, flags & ~F_ZERO);
    return;
  }
  int zero = expo == 0 && man == 0;
  struct fp f = {expo ? man | (1ull << 52) : man, (expo ? expo : 1) - 1075};
  if (!zero)
    fp_norm(&f);
  if (prec < 0)
    prec = 6;
  if (prec > MAXD - 2)
    prec = MAXD - 2;
  int len;
  int upper = conv == 'E' || conv == 'G';
  if (conv == 'g' || conv == 'G') {
    if (prec == 0)
      prec = 1;
    char d[MAXD];
    int x = zero ? 0 : fp_digits(f, d, prec);
    if (x < -4 || x >= prec)
      len = fmt_e(buf, f, zero, prec - 1, upper);
    else
      len = fmt_f(buf, f, zero, prec - 1 - x);
    /* strip trailing zeros of the fraction */
    int epos = len;
    for (int i = 0; i < len; i++)
      if (buf[i] == 'e' || buf[i] == 'E')
        epos = i;
    int dot = -1;
    for (int i = 0; i < epos; i++)
      if (buf[i] == '.')
        dot = i;
    if (dot >= 0) {
      int k = epos;
      while (k > dot + 1 && buf[k - 1] == '0')
        k--;
      if (k == dot + 1)
        k = dot;
      memmove(buf + k, buf + epos, (size_t)(len - epos));
      len -= epos - k;
    }
  } else if (conv == 'e' || conv == 'E') {
    len = fmt_e(buf, f, zero, prec, upper);
  } else {
    len = fmt_f(buf, f, zero, prec);
    if (len < 0)
      len = fmt_e(buf, f, zero, prec, 0);
  }
  emit(o, pre, buf, len, width, flags);
}

static void format(struct out *o, const char *fmt, va_list ap) {
  for (; *fmt; fmt++) {
    if (*fmt != '%') {
      put(o, *fmt);
      continue;
    }
    fmt++;
    int flags = 0;
    for (;; fmt++) {
      if (*fmt == '-')
        flags |= F_LEFT;
      else if (*fmt == '0')
        flags |= F_ZERO;
      else if (*fmt == '+')
        flags |= F_PLUS;
      else if (*fmt == ' ')
        flags |= F_SPACE;
      else
        break;
    }
    int width = 0;
    if (*fmt == '*') {
      width = va_arg(ap, int);
      if (width < 0)
        flags |= F_LEFT, width = -width;
      fmt++;
    } else {
      while (*fmt >= '0' && *fmt <= '9')
        width = width * 10 + (*fmt++ - '0');
    }
    int prec = -1;
    if (*fmt == '.') {
      fmt++;
      prec = 0;
      if (*fmt == '*') {
        prec = va_arg(ap, int);
        fmt++;
      } else {
        while (*fmt >= '0' && *fmt <= '9')
          prec = prec * 10 + (*fmt++ - '0');
      }
    }
    int lng = 0; /* 0 int, 1 long, 2 long long, -1 short, -2 char */
    for (;; fmt++) {
      if (*fmt == 'l')
        lng = lng == 1 ? 2 : 1;
      else if (*fmt == 'h')
        lng = lng == -1 ? -2 : -1;
      else if (*fmt == 'z')
        lng = sizeof(size_t) == 8 ? 2 : 0;
      else
        break;
    }
    char conv = *fmt;
    char num[32];
    char *end = num + sizeof num;
    switch (conv) {
    case 'd':
    case 'i': {
      s64 v = lng == 2   ? va_arg(ap, long long)
              : lng == 1 ? va_arg(ap, long)
                         : va_arg(ap, int);
      if (lng == -1)
        v = (short)v;
      else if (lng == -2)
        v = (signed char)v;
      int neg = v < 0;
      int n = utoa(end, neg ? (u64)-v : (u64)v, 10, 0);
      if (prec >= 0)
        flags &= ~F_ZERO;
      while (n < prec)
        *(end - ++n) = '0';
      emit(o, sign_prefix(neg, flags), end - n, n, width, flags);
      break;
    }
    case 'u':
    case 'x':
    case 'X':
    case 'o':
    case 'p': {
      u64 v;
      const char *pre = "";
      if (conv == 'p') {
        v = (uintptr_t)va_arg(ap, void *);
        pre = "0x";
      } else {
        v = lng == 2   ? va_arg(ap, unsigned long long)
            : lng == 1 ? va_arg(ap, unsigned long)
                       : va_arg(ap, unsigned);
        if (lng == -1)
          v = (unsigned short)v;
        else if (lng == -2)
          v = (unsigned char)v;
      }
      int base = conv == 'u' ? 10 : conv == 'o' ? 8 : 16;
      int n = utoa(end, v, base, conv == 'X');
      if (prec >= 0)
        flags &= ~F_ZERO;
      while (n < prec)
        *(end - ++n) = '0';
      emit(o, pre, end - n, n, width, flags);
      break;
    }
    case 'c':
      num[0] = (char)va_arg(ap, int);
      emit(o, "", num, 1, width, flags & ~F_ZERO);
      break;
    case 's': {
      const char *s = va_arg(ap, const char *);
      if (!s)
        s = "(null)";
      int n = 0;
      while (s[n] && (prec < 0 || n < prec))
        n++;
      emit(o, "", s, n, width, flags & ~F_ZERO);
      break;
    }
    case 'f':
    case 'F':
    case 'e':
    case 'E':
    case 'g':
    case 'G':
      fmt_float(o, va_arg(ap, double), conv == 'F' ? 'f' : conv, prec, width, flags);
      break;
    case '%':
      put(o, '%');
      break;
    case 0:
      return;
    default:
      put(o, '%');
      put(o, conv);
      break;
    }
  }
}

int mx_vsnprintf(char *buf, u32 size, const char *fmt, va_list ap) {
  struct out o = {buf, size, 0};
  format(&o, fmt, ap);
  if (size)
    buf[o.len < size ? o.len : size - 1] = 0;
  return (int)o.len;
}

int mx_snprintf(char *buf, u32 size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = mx_vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return n;
}

int mx_vprintf(const char *fmt, va_list ap) {
  char buf[512];
  int n = mx_vsnprintf(buf, sizeof buf, fmt, ap);
  mx_debug_write(buf, (u32)(n < (int)sizeof buf ? n : (int)sizeof buf - 1));
  return n;
}

int mx_printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = mx_vprintf(fmt, ap);
  va_end(ap);
  return n;
}
