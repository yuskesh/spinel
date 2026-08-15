/* sp_core.h -- runtime helpers split out of spinel_rt.h into
 * libspinel_rt.a so they are compiled once rather than re-parsed and
 * re-codegen'd in every generated translation unit.
 *
 * Signatures use intptr_t / double directly (== sp_int / sp_float)
 * to stay decoupled from the runtime's typedefs. These helpers call
 * sp_raise_cls / sp_sprintf, which live in the generated TU
 * (spinel_rt.h) and resolve at link time.
 */
#ifndef SP_CORE_H
#define SP_CORE_H

#include <stdint.h>
#include <stddef.h>   /* size_t (sp_snprintf_c_float) */

/* spinel-llvm Patch 4 (E010): SP_INT64 companion -- see lib/sp_types.h.
   The prototypes below spell sp_int as `intptr_t` (file comment above:
   "== sp_int"). Under SP_INT64 sp_int is int64_t regardless of pointer
   width, so the spelling has to follow: otherwise the generated TU passes
   int64 arguments to int32 prototypes and every libspinel_rt.a helper
   (sp_gcd / sp_str_to_i_* / sp_int_round / ...) truncates at the call.
   sp_core.c's own `typedef intptr_t sp_int;` and its
   `#if INTPTR_MAX == INT32_MAX` (SP_INT_POW10_LIMIT) follow from the same
   two definitions. Default OFF: with the flag unset nothing below changes. */
#if defined(SP_INT64)
#include "sp_types.h"
#define intptr_t sp_int
#endif

/* String -> number parsers (cold, I/O-boundary). */
intptr_t sp_str_to_i_cruby(const char *s);
double sp_str_to_f_cruby(const char *s);
intptr_t sp_str_to_i_base(const char *s, intptr_t base);
intptr_t sp_str_to_i_strict(const char *s);
intptr_t sp_str_to_i_strict_base(const char *s, intptr_t base);
intptr_t sp_str_to_i_lenient_base(const char *s, intptr_t base);
double  sp_str_to_f_strict(const char *s);
double sp_str_to_f_lenient(const char *s);
int     sp_snprintf_c_float(char *buf, size_t size, const char *fmt, double v);
int     sp_snprintf_ruby_float(char *buf, size_t size, const char *fmt, double v);

/* Cold integer-math + String#oct helpers. */
intptr_t sp_gcd(intptr_t a, intptr_t b);
intptr_t sp_lcm(intptr_t a, intptr_t b);
intptr_t sp_powmod(intptr_t base, intptr_t exp, intptr_t mod);
intptr_t sp_ceildiv(intptr_t a, intptr_t b);
intptr_t sp_int_clamp(intptr_t v, intptr_t lo, intptr_t hi);
double sp_float_clamp(double v, double lo, double hi);
intptr_t sp_int_sqrt(intptr_t n);
intptr_t sp_ipow10(intptr_t p);
intptr_t sp_int_round(intptr_t v, intptr_t nd);
intptr_t sp_int_ceil(intptr_t v, intptr_t nd);
intptr_t sp_int_floor(intptr_t v, intptr_t nd);
intptr_t sp_int_truncate(intptr_t v, intptr_t nd);
intptr_t sp_str_oct(const char *s);

/* Float#round / #floor / #ceil / #truncate at a POSITIVE digit count, and the
   `half:` variants of round. `op` selects which. */
#define SP_PREC_ROUND      0   /* round, default half-up tie-break */
#define SP_PREC_FLOOR      1
#define SP_PREC_CEIL       2
#define SP_PREC_TRUNC      3
#define SP_PREC_HALF_EVEN  4
#define SP_PREC_HALF_DOWN  5
double sp_float_prec_op(double x, intptr_t nd, int op);

#endif
