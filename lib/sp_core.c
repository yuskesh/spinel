/* sp_core.c -- runtime helpers split out of sp_runtime.h into
 * libspinel_rt.a. See sp_core.h for the rationale. */
#include "sp_core.h"
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

/* Must match sp_types.h: mrb_int is pointer-width (int64 on 64-bit,
   int32 on 32-bit) so this TU's helper ABI agrees with the generated TU. */
typedef intptr_t mrb_int;
typedef double  mrb_float;
/* 10^p fits mrb_int only up to this exponent (p>= it collapses to 0):
   10^19 > INT64_MAX, 10^10 > INT32_MAX. */
#if INTPTR_MAX == INT32_MAX
#define SP_INT_POW10_LIMIT 10
#else
#define SP_INT_POW10_LIMIT 19
#endif

/* Defined in the generated translation unit (sp_runtime.h); referenced
   here and resolved at link time. */
void sp_raise_cls(const char *cls, const char *msg);
const char *sp_sprintf(const char *fmt, ...);

/* CRuby's `String#to_i` accepts a leading sign, then digits with
   `_` between consecutive digits, and stops at the first non-digit
   (returning what it has so far rather than raising). `"1_2_3asdf"`
   -> 123. spinel previously emitted `(mrb_int)atoll(s)` which stops
   at the first `_`, returning 1 instead. Issue #619. */
mrb_int sp_str_to_i_cruby(const char *s) {
  if (!s) return 0;
  const char *p = s;
  while (isspace((unsigned char)*p)) p++;
  int neg = 0;
  if (*p == '+') p++;
  else if (*p == '-') { neg = 1; p++; }
  mrb_int v = 0;
  int any = 0;
  while (*p) {
    if (*p >= '0' && *p <= '9') {
      /* Signed-overflow on `v * 10 + digit` is undefined behavior;
         detect via __builtin_*_overflow. CRuby promotes to Bignum
         on overflow but spinel's int model is int64-only -- raise
         RangeError instead of silently saturating, so a user-side
         `rescue` can react. */
      mrb_int t;
      if (__builtin_mul_overflow(v, 10, &t) ||
          __builtin_add_overflow(t, (mrb_int)(*p - '0'), &v)) {
        sp_raise_cls("RangeError", sp_sprintf("integer overflow parsing \"%s\"", s));
      }
      any = 1;
      p++;
    }
else if (*p == '_' && any && p[1] >= '0' && p[1] <= '9') {
      p++;
    }
else {
      break;
    }
  }
  if (!any) return 0;
  return neg ? -v : v;
}

/* `String#to_i(base)` with a non-decimal base. Accepts bases 2..36
   like MRI; `_` is allowed between digits the same way as base 10.
   Stops at the first invalid digit and returns what's parsed so
   far. Issue #883. */
mrb_int sp_str_to_i_base(const char *s, mrb_int base) {
  if (!s) return 0;
  /* base 0 = auto-detect from prefix (0x -> 16, 0b -> 2, 0/0o -> 8,
     otherwise 10). Per CRuby, only base 0 enables prefix-based
     dispatch -- explicit bases just *accept* the matching prefix. */
  if (base != 0 && (base < 2 || base > 36)) base = 10;
  const char *p = s;
  while (isspace((unsigned char)*p)) p++;
  int neg = 0;
  if (*p == '+') p++;
  else if (*p == '-') { neg = 1; p++; }
  if (base == 0) {
    if (*p == '0') {
      if (p[1] == 'x' || p[1] == 'X') { base = 16; p += 2; }
      else if (p[1] == 'b' || p[1] == 'B') { base = 2; p += 2; }
      else if (p[1] == 'o' || p[1] == 'O') { base = 8; p += 2; }
      else if (p[1] == 'd' || p[1] == 'D') { base = 10; p += 2; }
      else if (p[1] >= '0' && p[1] <= '7') { base = 8; p++; }
      else { base = 10; }
    }
else {
      base = 10;
    }
  }
else if (*p == '0' && p[1] != 0) {
    /* Explicit base accepts the matching prefix. */
    if ((base == 16) && (p[1] == 'x' || p[1] == 'X')) p += 2;
    else if ((base == 2) && (p[1] == 'b' || p[1] == 'B')) p += 2;
    else if ((base == 8) && (p[1] == 'o' || p[1] == 'O')) p += 2;
  }
  mrb_int v = 0;
  int any = 0;
  while (*p) {
    int d = -1;
    if (*p >= '0' && *p <= '9') d = *p - '0';
    else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
    else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
    if (d < 0 || d >= (int)base) {
      if (*p == '_' && any) {
        /* Lookahead: only consume `_` between digits. */
        int n = -1;
        char c = p[1];
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'a' && c <= 'z') n = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') n = c - 'A' + 10;
        if (n >= 0 && n < (int)base) { p++; continue; }
      }
      break;
    }
    mrb_int t;
    if (__builtin_mul_overflow(v, base, &t) ||
        __builtin_add_overflow(t, (mrb_int)d, &v)) {
      sp_raise_cls("RangeError", sp_sprintf("integer overflow parsing \"%s\"", s));
    }
    any = 1;
    p++;
  }
  if (!any) return 0;
  return neg ? -v : v;
}

/* CRuby's `Integer(s)` raises ArgumentError for unparseable input
   (empty string, leading/trailing junk, all-whitespace). The bare
   `(mrb_int)strtoll(s, NULL, 10)` spinel previously emitted silently
   returned 0 instead, which made `Integer(s) rescue 0` always take
   the main branch. This helper matches CRuby semantics: skips
   leading/trailing whitespace, requires at least one valid digit,
   rejects trailing junk. Accepts an optional leading `+` / `-`. */
mrb_int sp_str_to_i_strict(const char *s) {
  if (!s) sp_raise_cls("ArgumentError", "invalid value for Integer(): nil");
  const char *p = s;
  while (isspace((unsigned char)*p)) p++;
  if (*p == '\0') sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Integer(): \"%s\"", s));
  char *endptr;
  errno = 0;
  long long v = strtoll(p, &endptr, 10);
  if (endptr == p) sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Integer(): \"%s\"", s));
  /* strtoll signals overflow via errno=ERANGE and clamps to
     LLONG_MAX/MIN -- raise rather than silently saturate so the
     caller can distinguish "fits in int64" from "too big". */
  if (errno == ERANGE) sp_raise_cls("RangeError", sp_sprintf("integer overflow parsing \"%s\"", s));
  while (isspace((unsigned char)*endptr)) endptr++;
  if (*endptr != '\0') sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Integer(): \"%s\"", s));
  /* On 32-bit mrb_int, a value that fit int64 (strtoll) may not fit the
     Ruby Integer; raise rather than silently truncate. No-op on 64-bit. */
#if INTPTR_MAX != INT64_MAX
  if (v < (long long)INTPTR_MIN || v > (long long)INTPTR_MAX)
    sp_raise_cls("RangeError", sp_sprintf("integer %lld out of range for 32-bit Integer", v));
#endif
  /* INTPTR_MIN == SP_INT_NIL is the int-nil sentinel; that value cannot
     be represented as a Ruby Integer in Spinel. */
  if ((mrb_int)v == (mrb_int)INTPTR_MIN)
    sp_raise_cls("RangeError", sp_sprintf("integer %lld out of Spinel representable range", v));
  return (mrb_int)v;
}

/* `Integer(s, base)` with explicit base. Bases 2..36, MRI-compatible
   prefix recognition (0x / 0b / 0o when the base matches). Raises
   ArgumentError on invalid input or unsupported base. Issue #887. */
mrb_int sp_str_to_i_strict_base(const char *s, mrb_int base) {
  if (!s) sp_raise_cls("ArgumentError", "invalid value for Integer(): nil");
  if (base == 0) {
    /* auto-detect the base from the literal's prefix */
    const char *q = s;
    while (isspace((unsigned char)*q)) q++;
    if (*q == '+' || *q == '-') q++;
    if (*q == '0') {
      char n = q[1];
      if (n == 'x' || n == 'X') base = 16;
      else if (n == 'b' || n == 'B') base = 2;
      else if (n == 'o' || n == 'O') base = 8;
      else if (n >= '0' && n <= '7') base = 8;
      else base = 10;
    }
    else base = 10;
  }
  if (base < 2 || base > 36) sp_raise_cls("ArgumentError", sp_sprintf("invalid radix %lld", (long long)base));
  const char *p = s;
  while (isspace((unsigned char)*p)) p++;
  int neg = 0;
  if (*p == '+') p++;
  else if (*p == '-') { neg = 1; p++; }
  if (*p == '0' && p[1] != 0) {
    if ((base == 16) && (p[1] == 'x' || p[1] == 'X')) p += 2;
    else if ((base == 2) && (p[1] == 'b' || p[1] == 'B')) p += 2;
    else if ((base == 8) && (p[1] == 'o' || p[1] == 'O')) p += 2;
  }
  if (*p == '\0') sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Integer(): \"%s\"", s));
  mrb_int v = 0;
  int any = 0;
  while (*p) {
    int d = -1;
    if (*p >= '0' && *p <= '9') d = *p - '0';
    else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
    else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
    if (d < 0 || d >= (int)base) {
      if (*p == '_' && any) {
        int n = -1;
        char c = p[1];
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'a' && c <= 'z') n = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') n = c - 'A' + 10;
        if (n >= 0 && n < (int)base) { p++; continue; }
      }
      break;
    }
    {
      mrb_int t;
      if (__builtin_mul_overflow(v, base, &t) ||
          __builtin_add_overflow(t, (mrb_int)d, &v)) {
        sp_raise_cls("RangeError", sp_sprintf("integer overflow parsing \"%s\"", s));
      }
    }
    any = 1;
    p++;
  }
  if (!any) sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Integer(): \"%s\"", s));
  while (isspace((unsigned char)*p)) p++;
  if (*p != '\0') sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Integer(): \"%s\"", s));
  return neg ? -v : v;
}

/* Kernel#Float() raises ArgumentError on unparseable input. strtod
   on its own would silently return 0.0 for "abc" or empty input;
   match MRI semantics by validating at-least-one-digit + no-trailing-
   junk. Whitespace flanking is fine. Issue #888. */
mrb_float sp_str_to_f_strict(const char *s) {
  if (!s) sp_raise_cls("ArgumentError", "invalid value for Float(): nil");
  const char *p = s;
  while (isspace((unsigned char)*p)) p++;
  if (*p == '\0') sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Float(): \"%s\"", s));
  char *endptr;
  double v = strtod(p, &endptr);
  if (endptr == p) sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Float(): \"%s\"", s));
  while (isspace((unsigned char)*endptr)) endptr++;
  if (*endptr != '\0') sp_raise_cls("ArgumentError", sp_sprintf("invalid value for Float(): \"%s\"", s));
  return (mrb_float)v;
}

/* Cold integer-math and String#oct helpers, moved out of sp_runtime.h
 * so they're compiled once into libspinel_rt.a rather than re-parsed
 * in every generated translation unit. Leaf functions: arithmetic +
 * libc + sp_raise_cls only. */
mrb_int sp_gcd(mrb_int a,mrb_int b){if(a<0)a=-a;if(b<0)b=-b;while(b){mrb_int t=b;b=a%b;a=t;}return a;}
mrb_int sp_lcm(mrb_int a,mrb_int b){if(a==0||b==0)return 0;mrb_int g=sp_gcd(a,b);if(a<0)a=-a;if(b<0)b=-b;return (a/g)*b;}
mrb_int sp_powmod(mrb_int base,mrb_int exp,mrb_int mod){if(mod==0)sp_raise_cls("ZeroDivisionError","divided by 0");mrb_int r=1;mrb_int m=mod<0?-mod:mod;if(m==1){r=0;}else{base=base%m;if(base<0)base+=m;while(exp>0){if(exp%2==1)r=r*base%m;exp=exp/2;base=base*base%m;}}if(mod<0&&r>0)r-=m;return r;}
mrb_int sp_ceildiv(mrb_int a,mrb_int b){if(b==0)sp_raise_cls("ZeroDivisionError","divided by 0");if(b==-1)return -a;mrb_int q=a/b;if(a%b!=0&&((a^b)>=0))q++;return q;}
mrb_int sp_int_clamp(mrb_int v,mrb_int lo,mrb_int hi){return v<lo?lo:v>hi?hi:v;}
/* Integer square root via Newton's method -- exact for the full
   mrb_int range. CRuby raises Math::DomainError on negative input
   (flattened runtime name "Math_DomainError"). The seed is n/2, not
   (n+1)/2: at n == MRB_INT_MAX the latter overflows (signed UB), and
   n/2 is a valid Newton seed for all n >= 2. */
mrb_int sp_int_sqrt(mrb_int n){if(n<0)sp_raise_cls("Math_DomainError","Numerical argument is out of domain - \"isqrt\"");if(n<2)return n;mrb_int x=n,y=n/2;while(y<x){x=y;y=(x+n/x)/2;}return x;}
/* Integer#round/ceil/floor/truncate at 10^(-ndigits). Pure integer
   arithmetic (no double precision loss above 2^53). 10^p fits mrb_int
   only for p<=18; p>=19 collapses to 0. Round-up multiply is overflow-
   guarded and falls back to the truncated value. */
mrb_int sp_ipow10(mrb_int p){mrb_int f=1;mrb_int i=0;while(i<p){f*=10;i++;}return f;}
mrb_int sp_int_round(mrb_int v,mrb_int nd){if(nd>=0)return v;mrb_int p=-nd;if(p>=SP_INT_POW10_LIMIT)return 0;mrb_int f=sp_ipow10(p);mrb_int q=v/f,r=v%f,half=f/2;if(v>=0){if(r>=half&&q<INTPTR_MAX/f)return(q+1)*f;return q*f;}if(-r>=half&&q>INTPTR_MIN/f)return(q-1)*f;return q*f;}
mrb_int sp_int_ceil(mrb_int v,mrb_int nd){if(nd>=0)return v;mrb_int p=-nd;if(p>=SP_INT_POW10_LIMIT)return 0;mrb_int f=sp_ipow10(p);mrb_int q=v/f,r=v%f;if(r!=0&&v>0&&q<INTPTR_MAX/f)return(q+1)*f;return q*f;}
mrb_int sp_int_floor(mrb_int v,mrb_int nd){if(nd>=0)return v;mrb_int p=-nd;if(p>=SP_INT_POW10_LIMIT)return 0;mrb_int f=sp_ipow10(p);mrb_int q=v/f,r=v%f;if(r!=0&&v<0&&q>INTPTR_MIN/f)return(q-1)*f;return q*f;}
mrb_int sp_int_truncate(mrb_int v,mrb_int nd){if(nd>=0)return v;mrb_int p=-nd;if(p>=SP_INT_POW10_LIMIT)return 0;mrb_int f=sp_ipow10(p);return(v/f)*f;}
/* String#oct: prefix auto-detection (0x=hex, 0b=bin, 0o/0=oct, else
   base-8). Matches CRuby. */
mrb_int sp_str_oct(const char*s){if(!s)return 0;const char*p=s;while(*p==' '||*p=='\t')p++;if(p[0]=='0'){if(p[1]=='x'||p[1]=='X')return(mrb_int)strtoll(p,NULL,16);if(p[1]=='b'||p[1]=='B')return(mrb_int)strtoll(p+2,NULL,2);if(p[1]=='o'||p[1]=='O')return(mrb_int)strtoll(p+2,NULL,8);return(mrb_int)strtoll(p,NULL,8);}return(mrb_int)strtoll(p,NULL,8);}
