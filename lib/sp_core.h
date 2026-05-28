/* sp_core.h -- runtime helpers split out of sp_runtime.h into
 * libspinel_rt.a so they are compiled once rather than re-parsed and
 * re-codegen'd in every generated translation unit.
 *
 * Signatures use int64_t / double directly (== mrb_int / mrb_float)
 * to stay decoupled from the runtime's typedefs and the mrb_bool
 * conflict between sp_runtime.h and mruby_shim.h -- same convention as
 * sp_time.h. These helpers call sp_raise_cls / sp_sprintf, which live
 * in the generated TU (sp_runtime.h) and resolve at link time.
 */
#ifndef SP_CORE_H
#define SP_CORE_H

#include <stdint.h>

/* String -> number parsers (cold, I/O-boundary). */
int64_t sp_str_to_i_cruby(const char *s);
int64_t sp_str_to_i_base(const char *s, int64_t base);
int64_t sp_str_to_i_strict(const char *s);
int64_t sp_str_to_i_strict_base(const char *s, int64_t base);
double  sp_str_to_f_strict(const char *s);

#endif
