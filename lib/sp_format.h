#ifndef SP_FORMAT_H
#define SP_FORMAT_H
/* sp_format.h -- cold value-type display helpers, split out of sp_runtime.h
   into libspinel_rt.a.

   Each formats a small value type (Complex / Rational / Range) into a freshly
   GC-allocated string. They need only the shared types (sp_types.h) and the
   shared string allocator (sp_alloc.h), and are only ever reached on a cold
   inspect/to_s path, so they compile once in the archive instead of being
   re-parsed and re-codegen'd in every generated translation unit. */
#include "sp_types.h"   /* sp_Complex, sp_Rational, sp_Range */

const char *sp_complex_inspect(sp_Complex c);
const char *sp_complex_to_s(sp_Complex c);
const char *sp_rational_inspect(sp_Rational r);
const char *sp_rational_to_s(sp_Rational r);
const char *sp_Range_inspect(sp_Range *r);
#endif /* SP_FORMAT_H */
