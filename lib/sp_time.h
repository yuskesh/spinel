/* sp_time.h -- Time value type + libc-backed accessors / formatters.
 *
 * The bulk of spinel's Time implementation lives in lib/sp_time.c
 * (compiled into libspinel_rt.a). sp_runtime.h carries only the
 * GC-aware wrappers (sp_box_time, sp_time_strftime_gc) — the small
 * sp_str_dup_external trampolines that bind the libspinel_rt format
 * helpers to the GC string heap.
 *
 * The format helpers (strftime / iso8601 / zone / inspect) write into
 * a caller-provided buffer; the caller is responsible for copying
 * into the GC string heap. This keeps libspinel_rt zero-dependency on
 * the runtime's static-inline helpers.
 *
 * Signatures use int64_t / double directly so this header stays
 * decoupled from the runtime's typedefs.
 */
#ifndef SP_TIME_H
#define SP_TIME_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct { int64_t tv_sec; int32_t tv_nsec; int8_t is_utc; } sp_Time;

/* Constructors */
sp_Time sp_time_now(void);
sp_Time sp_time_at_int(int64_t sec);
sp_Time sp_time_at_float(double epoch);
sp_Time sp_time_new(int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi, int64_t s);
sp_Time sp_time_new_utc(int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi, int64_t s);
sp_Time sp_time_utc(sp_Time t);
sp_Time sp_time_localtime(sp_Time t);

/* Broken-down resolver: is_utc selects gmtime vs localtime, off is the
   UTC offset in seconds, zbuf is the timezone abbreviation (8 bytes). */
void sp_time_vtm(sp_Time t, struct tm *bd, int32_t *off, char *zbuf);

/* Field accessors */
int64_t sp_time_year(sp_Time t);
int64_t sp_time_mon(sp_Time t);
int64_t sp_time_mday(sp_Time t);
int64_t sp_time_hour(sp_Time t);
int64_t sp_time_min(sp_Time t);
int64_t sp_time_sec(sp_Time t);
int64_t sp_time_wday(sp_Time t);
int64_t sp_time_yday(sp_Time t);
int64_t sp_time_isdst(sp_Time t);
int64_t sp_time_utc_offset(sp_Time t);

/* Time + Numeric / Time - Numeric. secs may be fractional. */
sp_Time sp_time_add(sp_Time t, double secs);

/* Comparison + integer/float shifts + Time-Time difference (cold value ops). */
int sp_time_cmp(sp_Time a, sp_Time b);
sp_Time sp_time_add_f(sp_Time t, double secs);
sp_Time sp_time_add_i(sp_Time t, int64_t secs);
sp_Time sp_time_sub_i(sp_Time t, int64_t secs);
double sp_time_sub_t(sp_Time a, sp_Time b);

/* Format helpers: write into `buf` (cap bytes incl NUL), return the
   number of bytes written excluding NUL. Return 0 on failure. */
size_t sp_time_strftime_to(sp_Time t, const char *fmt, char *buf, size_t cap);
size_t sp_time_iso8601_to(sp_Time t, char *buf, size_t cap);
size_t sp_time_zone_to(sp_Time t, char *buf, size_t cap);
size_t sp_time_inspect_to(sp_Time t, char *buf, size_t cap);

#endif
