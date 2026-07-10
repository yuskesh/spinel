/* sp_format.c -- cold value-type display helpers (see sp_format.h).
   Self-contained: the shared value types (sp_types.h) + string allocator
   (sp_alloc.h) + libc formatting only. */
#include "sp_format.h"
#include "sp_alloc.h"   /* sp_str_alloc_raw, sp_raise_cls, <math.h> for cos/sin/sqrt */
#include <stdio.h>
#include <string.h>
#include <time.h>       /* gmtime / strftime for sp_Time_inspect */

/* Format a non-negative Complex-component magnitude the way MRI does: infinite
   and NaN values become the Ruby names Infinity/NaN (not C's inf/nan), a whole
   value stays integer-looking, else %g. Returns the written length. */
static int sp_complex_mag(char *out, size_t sz, mrb_float v) {
  if (isinf(v)) return snprintf(out, sz, "Infinity");
  if (isnan(v)) return snprintf(out, sz, "NaN");
  /* a float outside mrb_int's range makes (mrb_int)v undefined behavior; only
     integer-print a whole value that fits, else fall through to %g. */
  if (v >= -(mrb_float)INTPTR_MAX && v <= (mrb_float)INTPTR_MAX && v == (mrb_int)v)
    return snprintf(out, sz, "%lld", (long long)v);
  return snprintf(out, sz, "%g", v);
}
/* Append the imaginary part ("+<mag>i" / "-<mag>i") to buf. MRI inserts a `*`
   before a non-numeric magnitude (Infinity/NaN) so `Infinity*i` stays readable,
   while a plain number is written as `10i`. */
static int sp_complex_imag(char *buf, int n, size_t sz, mrb_float im) {
  if (n < 0 || (size_t)n >= sz) return 0;
  char mag[64];
  sp_complex_mag(mag, sizeof mag, im < 0 ? -im : im);
  const char *sep = (mag[0] >= '0' && mag[0] <= '9') ? "" : "*";
  return snprintf(buf + n, sz - (size_t)n, "%c%s%si", im < 0 ? '-' : '+', mag, sep);
}
const char *sp_complex_inspect(sp_Complex c) {
  char buf[128], re[64];
  sp_complex_mag(re, sizeof re, c.re < 0 ? -c.re : c.re);
  int n = snprintf(buf, sizeof buf, "(%s%s", c.re < 0 ? "-" : "", re);
  if (n < 0) n = 0; else if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
  n += sp_complex_imag(buf, n, sizeof buf, c.im);
  if (n < (int)sizeof buf) n += snprintf(buf + n, sizeof(buf) - (size_t)n, ")");
  if (n < 0) n = 0; else if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
  char *r = sp_str_alloc_raw(n + 1);
  memcpy(r, buf, n);
  r[n] = 0;
  return r;
}
/* Complex#to_s: bare `re+imi` (no surrounding parens, unlike #inspect). */
const char *sp_complex_to_s(sp_Complex c) {
  char buf[128], re[64];
  sp_complex_mag(re, sizeof re, c.re < 0 ? -c.re : c.re);
  int n = snprintf(buf, sizeof buf, "%s%s", c.re < 0 ? "-" : "", re);
  if (n < 0) n = 0; else if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
  n += sp_complex_imag(buf, n, sizeof buf, c.im);
  if (n < 0) n = 0; else if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
  char *r = sp_str_alloc_raw(n + 1);
  memcpy(r, buf, n);
  r[n] = 0;
  return r;
}

const char *sp_rational_inspect(sp_Rational r) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "(%lld/%lld)", (long long)r.num, (long long)r.den);
  if (n < 0) n = 0;
  char *o = sp_str_alloc_raw(n + 1);
  memcpy(o, buf, n);
  o[n] = 0;
  return o;
}
/* Rational#to_s: bare `num/den` (no parens, unlike #inspect). */
const char *sp_rational_to_s(sp_Rational r) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "%lld/%lld", (long long)r.num, (long long)r.den);
  if (n < 0) n = 0;
  char *o = sp_str_alloc_raw(n + 1);
  memcpy(o, buf, n);
  o[n] = 0;
  return o;
}

const char *sp_Range_inspect(sp_Range *r) {
  /* "first..last" / "first...last" form. Buffer sized for two int64s + dots. */
  char *buf = sp_str_alloc_raw(48);
  snprintf(buf, 48, r->excl ? "%lld...%lld" : "%lld..%lld", (long long)r->first, (long long)r->last);
  return buf;
}

/* "YYYY-MM-DD HH:MM:SS UTC" via gmtime: the spinel runtime keeps Time
   timezone-naive, so UTC is the unambiguous choice that needs no tzdata. */
const char *sp_Time_inspect(sp_Time *t) {
  char *buf = sp_str_alloc_raw(40);
  time_t sec = (time_t)t->tv_sec;
  struct tm *tm_ = gmtime(&sec);
  if (tm_) {
    strftime(buf, 40, "%Y-%m-%d %H:%M:%S UTC", tm_);
  }
  else {
    snprintf(buf, 40, "Time(%lld)", (long long)t->tv_sec);
  }
  return buf;
}

/* ---- Complex arithmetic ---- */
sp_Complex sp_complex_polar(mrb_float m, mrb_float a) { sp_Complex c; c.re = m * cos(a); c.im = m * sin(a); return c; }
sp_Complex sp_complex_add(sp_Complex a, sp_Complex b) { sp_Complex c; c.re = a.re + b.re; c.im = a.im + b.im; return c; }
sp_Complex sp_complex_mul(sp_Complex a, sp_Complex b) { sp_Complex c; c.re = (a.re * b.re) - (a.im * b.im); c.im = (a.re * b.im) + (a.im * b.re); return c; }
sp_Complex sp_complex_conjugate(sp_Complex a) { sp_Complex c; c.re = a.re; c.im = -a.im; return c; }
sp_Complex sp_complex_sub(sp_Complex a, sp_Complex b) { sp_Complex c; c.re = a.re - b.re; c.im = a.im - b.im; return c; }
sp_Complex sp_complex_div(sp_Complex a, sp_Complex b) {
  mrb_float d = (b.re * b.re) + (b.im * b.im); sp_Complex c;
  c.re = ((a.re * b.re) + (a.im * b.im)) / d; c.im = ((a.im * b.re) - (a.re * b.im)) / d; return c;
}
/* Complex divided by a real scalar divides each component -- unlike the
   conjugate formula, this yields Infinity (not NaN) when the divisor is 0.0,
   matching MRI's Complex#/ against a Float. */
sp_Complex sp_complex_div_real(sp_Complex a, mrb_float b) {
  sp_Complex c; c.re = a.re / b; c.im = a.im / b; return c;
}
/* Complex divided by an Integer follows integer zero-division rules: dividing
   by 0 raises ZeroDivisionError, as in MRI (a Float divisor gives Infinity). */
sp_Complex sp_complex_div_int(sp_Complex a, mrb_int b) {
  if (b == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  sp_Complex c; c.re = a.re / (mrb_float)b; c.im = a.im / (mrb_float)b; return c;
}
sp_Complex sp_complex_neg(sp_Complex a) { sp_Complex c; c.re = -a.re; c.im = -a.im; return c; }
mrb_float sp_complex_abs2(sp_Complex a) { return (a.re * a.re) + (a.im * a.im); }
mrb_float sp_complex_abs(sp_Complex a) { return sqrt((a.re * a.re) + (a.im * a.im)); }
mrb_bool sp_complex_eq(sp_Complex a, sp_Complex b) { return a.re == b.re && a.im == b.im; }
sp_Complex sp_complex_pow(sp_Complex a, mrb_int e) {
  sp_Complex r; r.re = 1; r.im = 0;
  mrb_int k = e < 0 ? -e : e;
  for (mrb_int i = 0; i < k; i++) r = sp_complex_mul(r, a);
  if (e < 0) { sp_Complex one; one.re = 1; one.im = 0; r = sp_complex_div(one, r); }
  return r;
}

/* ---- Rational arithmetic ----
   Intermediate products use a wider type; a result that does not fit back into
   mrb_int raises RangeError (mruby promotes to Bigint -- a later phase can too).
   __int128 covers the 64-bit build; int64 covers two int32 operands losslessly. */
static mrb_int sp_rational_gcd_i(mrb_int a, mrb_int b) {
  if (a < 0) a = -a;
  if (b < 0) b = -b;
  while (b) { mrb_int t = b; b = a % b; a = t; }
  return a;
}
sp_Rational sp_rational_new(mrb_int n, mrb_int d) {
  sp_Rational r;
  if (d == 0) { r.num = n; r.den = 0; return r; }
  if (d < 0) { n = -n; d = -d; }
  mrb_int g = sp_rational_gcd_i(n, d);
  if (g <= 0) g = 1;
  r.num = n / g;
  r.den = d / g;
  return r;
}
/* String#to_r: parse a leading numeric of the form [ws][sign]digits[.digits][/digits],
   stopping at the first non-numeric byte; an unparseable string is 0/1 (MRI). */
sp_Rational sp_str_to_r(const char *s) {
  if (!s) return sp_rational_new(0, 1);
  const char *p = s;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') p++;
  mrb_int sign = 1;
  if (*p == '+') p++;
  else if (*p == '-') { sign = -1; p++; }
  mrb_int num = 0, den = 1;
  int any = 0;
  while (*p >= '0' && *p <= '9') { num = num * 10 + (*p - '0'); p++; any = 1; }
  if (*p == '.') {
    p++;
    while (*p >= '0' && *p <= '9') { num = num * 10 + (*p - '0'); den *= 10; p++; any = 1; }
  }
  if (*p == '/') {
    p++;
    mrb_int d2 = 0; int anyd = 0;
    while (*p >= '0' && *p <= '9') { d2 = d2 * 10 + (*p - '0'); p++; anyd = 1; }
    if (anyd && d2 != 0) den *= d2;
  }
  if (!any) return sp_rational_new(0, 1);
  return sp_rational_new(sign * num, den);
}
#if INTPTR_MAX > 0x7fffffff
typedef __int128 sp_rat_wide;
#else
typedef long long sp_rat_wide;
#endif
static mrb_int sp_rat_fit(sp_rat_wide v) {
  if (v > (sp_rat_wide)INTPTR_MAX || v < (sp_rat_wide)(-INTPTR_MAX))
    sp_raise_cls("RangeError", "Rational out of mrb_int range");
  return (mrb_int)v;
}
static sp_Rational sp_rational_new_wide(sp_rat_wide n, sp_rat_wide d) {
  if (d == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  if (d < 0) { n = -n; d = -d; }
  sp_rat_wide a = n < 0 ? -n : n, b = d;
  while (b) { sp_rat_wide t = b; b = a % b; a = t; }
  if (a <= 0) a = 1;
  sp_Rational r;
  r.num = sp_rat_fit(n / a);
  r.den = sp_rat_fit(d / a);
  return r;
}
sp_Rational sp_rational_add(sp_Rational a, sp_Rational b) {
  return sp_rational_new_wide(((sp_rat_wide)a.num * b.den) + ((sp_rat_wide)b.num * a.den),
                              (sp_rat_wide)a.den * b.den);
}
sp_Rational sp_rational_sub(sp_Rational a, sp_Rational b) {
  return sp_rational_new_wide(((sp_rat_wide)a.num * b.den) - ((sp_rat_wide)b.num * a.den),
                              (sp_rat_wide)a.den * b.den);
}
sp_Rational sp_rational_mul(sp_Rational a, sp_Rational b) {
  return sp_rational_new_wide((sp_rat_wide)a.num * b.num, (sp_rat_wide)a.den * b.den);
}
sp_Rational sp_rational_div(sp_Rational a, sp_Rational b) {
  if (b.num == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  return sp_rational_new_wide((sp_rat_wide)a.num * b.den, (sp_rat_wide)a.den * b.num);
}
sp_Rational sp_rational_neg(sp_Rational a) { a.num = -a.num; return a; }
sp_Rational sp_rational_abs(sp_Rational a) { if (a.num < 0) a.num = -a.num; return a; }
mrb_int sp_rational_cmp(sp_Rational a, sp_Rational b) {
  sp_rat_wide l = (sp_rat_wide)a.num * b.den, r = (sp_rat_wide)b.num * a.den;
  return l < r ? -1 : (l > r ? 1 : 0);
}
mrb_bool sp_rational_eq(sp_Rational a, sp_Rational b) {
  return a.num == b.num && a.den == b.den;
}
mrb_float sp_rational_to_f(sp_Rational a) {
  return (mrb_float)a.num / (mrb_float)a.den;
}
/* 2^e as a wide integer, raising RangeError past the mrb_int range. */
static sp_rat_wide sp_rat_pow2(int e) {
  sp_rat_wide r = 1;
  for (int i = 0; i < e; i++) {
    r <<= 1;
    if (r > (sp_rat_wide)INTPTR_MAX) sp_raise_cls("RangeError", "Rational out of mrb_int range");
  }
  return r;
}
/* Float#to_r : the exact rational value of the double. A finite double equals
   m * 2^e for a 53-bit integer mantissa m, so the conversion is exact. When the
   exact numerator/denominator does not fit in mrb_int (huge magnitude or a tiny
   subnormal), it raises RangeError -- spinel's Rational is int64/int64 and does
   not promote to a Bignum (matching the existing Rational-overflow behavior). */
sp_Rational sp_float_to_rational(mrb_float f) {
  if (isnan(f) || isinf(f)) sp_raise_cls("FloatDomainError", isnan(f) ? "NaN" : "Infinity");
  if (f == 0.0) { sp_Rational z; z.num = 0; z.den = 1; return z; }
  int e;
  double m = frexp((double)f, &e);          /* f = m * 2^e, 0.5 <= |m| < 1 */
  for (int i = 0; i < 53 && m != floor(m); i++) { m *= 2.0; e--; }
  sp_rat_wide num = (sp_rat_wide)m;          /* integer mantissa (fits in 54 bits) */
  if (e >= 0) {
    /* num * 2^e can exceed sp_rat_wide on a 32-bit build (long long); guard the
       product first. p > INTPTR_MAX/|num| is exactly "num*p won't fit mrb_int",
       so this raises rather than overflowing. */
    sp_rat_wide p = sp_rat_pow2(e), an = num < 0 ? -num : num;
    if (an != 0 && p > (sp_rat_wide)INTPTR_MAX / an)
      sp_raise_cls("RangeError", "Rational out of mrb_int range");
    return sp_rational_new_wide(num * p, 1);
  }
  return sp_rational_new_wide(num, sp_rat_pow2(-e));
}
/* Simplest p/q with lo <= p/q <= hi, for 0 < lo <= hi. Classic continued-fraction
   "simplest rational in an interval" recursion; the convergent depth is bounded by
   the interval, so q stays small for sane epsilons. */
static void sp_simplest_pos(double lo, double hi, sp_rat_wide *np, sp_rat_wide *dp) {
  double fl = floor(lo);
  if (fl == lo) { *np = (sp_rat_wide)fl; *dp = 1; return; }   /* lo is an integer */
  if (fl == floor(hi)) {                                       /* no integer in (lo,hi) */
    sp_rat_wide n, d;
    sp_simplest_pos(1.0 / (hi - fl), 1.0 / (lo - fl), &n, &d);
    *np = ((sp_rat_wide)fl * n) + d;
    *dp = n;
    return;
  }
  *np = (sp_rat_wide)fl + 1; *dp = 1;                          /* fl+1 lies in [lo,hi] */
}
static sp_Rational sp_rationalize_interval(double lo, double hi) {
  if (lo > hi) { double t = lo; lo = hi; hi = t; }
  if (lo <= 0.0 && hi >= 0.0) { sp_Rational z; z.num = 0; z.den = 1; return z; }
  int neg = 0;
  if (hi < 0.0) { double t = -lo; lo = -hi; hi = t; neg = 1; } /* fold to positives */
  /* Any p/q in [lo,hi] has p >= lo*q >= lo, so lo past mrb_int can't fit and
     floor(lo) would also overflow the (double->sp_rat_wide) cast below. */
  if (lo > (double)INTPTR_MAX) sp_raise_cls("RangeError", "Rational out of mrb_int range");
  sp_rat_wide n, d;
  sp_simplest_pos(lo, hi, &n, &d);
  if (neg) n = -n;
  return sp_rational_new_wide(n, d);
}
sp_Rational sp_float_rationalize(mrb_float f, mrb_float eps) {
  if (isnan(f) || isinf(f)) sp_raise_cls("FloatDomainError", isnan(f) ? "NaN" : "Infinity");
  double e = eps < 0 ? -eps : eps;
  return sp_rationalize_interval((double)f - e, (double)f + e);
}
/* No-arg rationalize: simplest rational that round-trips to this exact double,
   i.e. lying in the half-ulp interval around f. */
sp_Rational sp_float_rationalize0(mrb_float f) {
  if (isnan(f) || isinf(f)) sp_raise_cls("FloatDomainError", isnan(f) ? "NaN" : "Infinity");
  if (f == 0.0) { sp_Rational z; z.num = 0; z.den = 1; return z; }
  double lo = ((double)f + nextafter((double)f, -INFINITY)) / 2.0;
  double hi = ((double)f + nextafter((double)f,  INFINITY)) / 2.0;
  return sp_rationalize_interval(lo, hi);
}
static sp_rat_wide sp_rat_ipow(sp_rat_wide base, mrb_int e) {
  sp_rat_wide r = 1;
  for (mrb_int i = 0; i < e; i++) {
    r *= base;
    if (r > (sp_rat_wide)INTPTR_MAX || r < (sp_rat_wide)(-INTPTR_MAX))
      sp_raise_cls("RangeError", "Rational out of mrb_int range");
  }
  return r;
}
sp_Rational sp_rational_pow(sp_Rational a, mrb_int e) {
  if (e >= 0) return sp_rational_new_wide(sp_rat_ipow(a.num, e), sp_rat_ipow(a.den, e));
  if (a.num == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  return sp_rational_new_wide(sp_rat_ipow(a.den, -e), sp_rat_ipow(a.num, -e));
}
