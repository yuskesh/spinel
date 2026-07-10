/* sp_format.c -- cold value-type display helpers (see sp_format.h).
   Self-contained: the shared value types (sp_types.h) + string allocator
   (sp_alloc.h) + libc formatting only. */
#include "sp_format.h"
#include "sp_alloc.h"   /* sp_str_alloc_raw, sp_raise_cls, <math.h> for cos/sin/sqrt */
#include <stdio.h>
#include <string.h>
#include <time.h>       /* gmtime / strftime for sp_Time_inspect */

const char *sp_complex_inspect(sp_Complex c) {
  char buf[128];
  int n = 0;
  /* Real part: keep integer-looking values short. */
  if (c.re == (mrb_int)c.re) n += snprintf(buf + n, sizeof(buf) - n, "(%lld", (long long)c.re);
  else n += snprintf(buf + n, sizeof(buf) - n, "(%g", c.re);
  /* Imaginary sign + value. */
  if (c.im < 0) {
    if (c.im == (mrb_int)c.im) n += snprintf(buf + n, sizeof(buf) - n, "-%lldi)", -(long long)c.im);
    else n += snprintf(buf + n, sizeof(buf) - n, "%gi)", c.im);
  }
  else {
    if (c.im == (mrb_int)c.im) n += snprintf(buf + n, sizeof(buf) - n, "+%lldi)", (long long)c.im);
    else n += snprintf(buf + n, sizeof(buf) - n, "+%gi)", c.im);
  }
  if (n < 0) n = 0;
  char *r = sp_str_alloc_raw(n + 1);
  memcpy(r, buf, n);
  r[n] = 0;
  return r;
}
/* Complex#to_s: bare `re+imi` (no surrounding parens, unlike #inspect). */
const char *sp_complex_to_s(sp_Complex c) {
  char buf[128];
  int n = 0;
  if (c.re == (mrb_int)c.re) n += snprintf(buf + n, sizeof(buf) - n, "%lld", (long long)c.re);
  else n += snprintf(buf + n, sizeof(buf) - n, "%g", c.re);
  if (c.im < 0) {
    if (c.im == (mrb_int)c.im) n += snprintf(buf + n, sizeof(buf) - n, "-%lldi", -(long long)c.im);
    else n += snprintf(buf + n, sizeof(buf) - n, "%gi", c.im);
  }
  else {
    if (c.im == (mrb_int)c.im) n += snprintf(buf + n, sizeof(buf) - n, "+%lldi", (long long)c.im);
    else n += snprintf(buf + n, sizeof(buf) - n, "+%gi", c.im);
  }
  if (n < 0) n = 0;
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
/* No-arg rationalize: the simplest rational whose nearest double is exactly f.
   The continued-fraction convergents of f are, by construction, the simplest
   rationals approximating it in increasing complexity, so the FIRST convergent
   that round-trips to f is the answer. This is robust where a double-precision
   half-ulp interval search is not: computing the interval bounds in `double`
   rounds them back to f for an exactly-representable value (collapsing the
   interval) and amplifies rounding at the ulp scale for others. */
sp_Rational sp_float_rationalize0(mrb_float f) {
  if (isnan(f) || isinf(f)) sp_raise_cls("FloatDomainError", isnan(f) ? "NaN" : "Infinity");
  if (f == 0.0) { sp_Rational z; z.num = 0; z.den = 1; return z; }
  int neg = f < 0.0;
  double x = neg ? -(double)f : (double)f;
  double v = x;
  sp_rat_wide h0 = 1, h1 = 0, k0 = 0, k1 = 1;  /* convergent num/den recurrences */
  sp_rat_wide num = 0, den = 1;
  int found = 0;
  for (int i = 0; i < 64 && !found; i++) {
    /* floor(v) out of sp_rat_wide range would make the cast below UB; any such
       convergent exceeds INTPTR_MAX anyway, so bail to the exact-ratio fallback. */
    if (v > (double)INTPTR_MAX) break;
    sp_rat_wide a = (sp_rat_wide)floor(v);
    /* guard the convergent against mrb_int overflow before forming it (all in
       integer arithmetic: a*h0 + h1 <= INTPTR_MAX). */
    if (a > 0 && (h0 > ((sp_rat_wide)INTPTR_MAX - h1) / a ||
                  k0 > ((sp_rat_wide)INTPTR_MAX - k1) / a))
      break;
    sp_rat_wide h = a * h0 + h1;
    sp_rat_wide k = a * k0 + k1;
    num = h; den = k;
    if (k != 0 && (double)h / (double)k == x) { found = 1; break; }
    h1 = h0; h0 = h; k1 = k0; k0 = k;
    double frac = v - a;
    if (frac == 0.0) break;
    v = 1.0 / frac;
  }
  if (!found) return sp_float_to_rational(f);  /* fallback: exact bit ratio */
  return sp_rational_new_wide(neg ? -num : num, den);
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
