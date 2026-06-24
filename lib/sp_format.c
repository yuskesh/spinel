/* sp_format.c -- cold value-type display helpers (see sp_format.h).
   Self-contained: the shared value types (sp_types.h) + string allocator
   (sp_alloc.h) + libc formatting only. */
#include "sp_format.h"
#include "sp_alloc.h"   /* sp_str_alloc_raw */
#include <stdio.h>
#include <string.h>

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
