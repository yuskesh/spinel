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
