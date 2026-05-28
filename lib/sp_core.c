/* sp_core.c -- runtime helpers split out of sp_runtime.h into
 * libspinel_rt.a. See sp_core.h for the rationale. */
#include "sp_core.h"
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

typedef int64_t mrb_int;
typedef double  mrb_float;

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
    } else if (*p == '_' && any && p[1] >= '0' && p[1] <= '9') {
      p++;
    } else {
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
    } else {
      base = 10;
    }
  } else if (*p == '0' && p[1] != 0) {
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
  return (mrb_int)v;
}

/* `Integer(s, base)` with explicit base. Bases 2..36, MRI-compatible
   prefix recognition (0x / 0b / 0o when the base matches). Raises
   ArgumentError on invalid input or unsupported base. Issue #887. */
mrb_int sp_str_to_i_strict_base(const char *s, mrb_int base) {
  if (!s) sp_raise_cls("ArgumentError", "invalid value for Integer(): nil");
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
