/* Spinel Runtime Library */
#ifndef SP_RUNTIME_H
#define SP_RUNTIME_H

/* Platform feature-test macros (_DARWIN_C_SOURCE for MAP_ANON) live at the top
   of sp_types.h so every translation unit that includes it defines them before
   the first system header. Must precede <stdio.h>. */
#include "sp_types.h"
#include "sp_alloc.h"   /* shared string-heap state + allocators (extern; see sp_alloc.c) */
#include "sp_marshal.h" /* Marshal.dump/load (lib/sp_marshal.c) + the sp_marshal_v vtable */
#include "sp_format.h"  /* cold value-type display helpers (lib/sp_format.c) */
#include "sp_string.h"  /* sp_String builder (hot core inline; cold mutators in lib/sp_string.c) */
#include "sp_inspect.h" /* generic container #inspect (lib/sp_inspect.c) */
#include "sp_array.h"   /* typed arrays: hot core inline + cold ops in lib/sp_array.c */
#include "sp_re.h"      /* regexp wrappers + MatchData (lib/sp_re.c); engine = build/regexp/*.o */
#include "sp_str.h"     /* cold string transforms (lib/sp_str.c); hot/utf8 core stays here */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#include <setjmp.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

/* Opt-in native backtrace (spinel --debug). In a -g, non-inlined build the
   sp_<method> symbols are present, so sp_raise_cls can snapshot the live C
   stack at raise time and Exception#backtrace / caller format it into a
   Ruby-style backtrace — no per-method shadow frames needed. Off unless the
   generated main() sets sp_bt_enabled (debug builds), so non-debug behaviour
   and cost are unchanged. execinfo is POSIX-ish; absent on Windows. */
#include <execinfo.h>
#define SP_BT_AVAILABLE 1
static int sp_bt_enabled = 0;          /* set to 1 by debug-build main() */
static const char *sp_bt_srcfile = ""; /* toplevel .rb path, set by debug main() */
#if SP_BT_AVAILABLE
static void *sp_bt_buf[256];       /* frames captured at the last raise */
static int sp_bt_n = 0;
#endif
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#if !defined(__APPLE__) && !defined(__FreeBSD__)
#include <malloc.h>
#else
/* Darwin's libc has no malloc_trim; make it a no-op so call sites stay portable. */
#define malloc_trim(x) ((void)0)
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

/* Core value-type definitions (primitives, leaf structs, GC headers,
   typed arrays, non-poly hashes) live in sp_types.h so libspinel_rt.a
   sources can share them without the function bodies below. */
#include "sp_types.h"
/* sp_RbVal, the collector globals/entry points, and the hot inline mark
   helpers; the collector body lives in libspinel_rt.a (lib/sp_gc.c). */
#include "sp_gc.h"
/* sp_Fiber + the Fiber API; the bodies live in libspinel_rt.a (lib/sp_fiber.c). */
#include "sp_fiber.h"
/* sp_thread + the cooperative scheduler (Phase 0); bodies in lib/sp_sched.c. */
#include "sp_sched.h"
static const char *sp_sym_to_s(sp_sym id);
/* Capacity of the runtime symbol-intern pool the generated TU declares
   (sp_dyn_syms). 8 bytes/entry, so the default is a 64 KB static buffer holding
   symbols minted at runtime (String#to_sym, :"#{interp}"). Embedded targets that
   intern few or no symbols at runtime can shrink it with -DSP_DYN_SYMS_MAX=<n>. */
#ifndef SP_DYN_SYMS_MAX
#define SP_DYN_SYMS_MAX 8192
#endif

/* sp_raise_cls forward decl — defined later in this header (line ~1017).
   Used by the integer-division helpers below to match CRuby semantics:
   `a / 0`, `a % 0`, `a.divmod(0)`, `a.ceildiv(0)`, and `a.pow(e, 0)` all
   raise ZeroDivisionError instead of triggering C undefined behaviour
   (SIGFPE on x86) or silently returning 0. */
SP_NORETURN SP_COLD void sp_raise_cls(const char *cls, const char *msg);

/* The unresolved-call gate (codegen_call.c) raises NoMethodError through this
   single recognizable token under SPINEL_GATE_RAISE, so coercion sites can
   detect and coerce it (sp_poly_to_i(sp_raise_nomethod(...)) etc.) rather than
   parse a comma-expression. Returns sp_RbVal so it composes in a poly slot;
   NORETURN, so the value is never produced. Unused when the gate stays silent. */
/* Deliberately an extern, NOT declared noreturn (lib/sp_core.c): gate arms
   sit inside hot dispatch functions (PPU update_scroll_address_line), and a
   noreturn call there restructures the CFG -- blocks reorder and optcarrot
   pays ~5-7% fps. As a plain value-returning extern call the arm keeps the
   exact shape of the sp_box_nil() it replaced; the raise still never
   returns at runtime (sp_raise_cls longjmps). */
sp_RbVal sp_raise_nomethod(const char *msg);

static inline mrb_int sp_idiv(mrb_int a, mrb_int b) {
  if (b == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  mrb_int q = a / b; mrb_int r = a % b;
  if ((r != 0) && ((r ^ b) < 0)) q--;
  return q;
}
static inline mrb_int sp_imod(mrb_int a, mrb_int b) {
  if (b == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  mrb_int r = a % b;
  if ((r != 0) && ((r ^ b) < 0)) r += b;
  return r;
}
/* Float#% (and Integer % Float): floored modulo whose result takes the sign of
   the divisor, unlike C fmod which follows the dividend (-5.5 % 2 == 0.5). */
/* float %% with an INTEGER zero divisor raises in CRuby (5.0 %% 0), while a
   float zero divisor yields NaN (5.0 %% 0.0) -- the int-divisor emit routes
   here so the check costs nothing on the float-divisor path. */
static inline double sp_fmod_intdiv(double a, mrb_int b) {
  if (b == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  double r = fmod(a, (double)b);
  if (r != 0 && ((r < 0) != (b < 0))) r += (double)b;
  return r;
}
static inline double sp_fmod(double a, double b) {
  if (b == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");  /* 5.0 % 0.0 raises in CRuby */
  double r = fmod(a, b);
  if (r != 0.0 && ((r < 0.0) != (b < 0.0))) r += b;
  return r;
}
/* Integer#remainder: truncated remainder (sign follows the dividend, i.e. plain
   C `%`), unlike the floored sp_imod. Zero divisor raises like sp_imod/sp_idiv. */
static inline mrb_int sp_iremainder(mrb_int a, mrb_int b) {
  if (b == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  return a % b;
}
/* Overflow-checked integer arithmetic (BIGINT.md option β: raise on
   overflow, keep locals at native mrb_int width).

   Build modes (preprocessor toggles):
     default                 : overflow raises RangeError
     SP_NO_OVERFLOW_CHECK    : bare `+ - *`, silent wrap (UB on signed
                               overflow, but matches spinel's prior
                               behaviour where the user asks for
                               max speed and accepts the risk)

   The actual overflow check is wrapped through
   `sp_int_*_overflow_p` helpers so non-gcc/clang compilers fall back
   to a portable unsigned-arithmetic implementation. The shape
   mirrors mruby's mrb_int_*_overflow in include/mruby/numeric.h.

   The outer sp_int_add/sub/mul/neg are GCC statement-expression
   macros (`({ ... })`) rather than functions: an earlier inline-
   function variant produced wrong optcarrot output (checksum
   diverged 59662→4096) even with __attribute__((always_inline)),
   apparently from a UB-assumption interaction between the
   optimizer and the function-call-shaped expression that the
   macro form bypasses by keeping every operand in its surrounding
   expression context. The macros also let the compiler fold
   constant-operand cases at every call site. */
#ifndef __has_builtin
#  define __has_builtin(x) 0
#endif
#if (defined(__GNUC__) && __GNUC__ >= 5) || \
    (__has_builtin(__builtin_add_overflow) && \
     __has_builtin(__builtin_sub_overflow) && \
     __has_builtin(__builtin_mul_overflow))
#  define SP_HAVE_OVERFLOW_BUILTINS 1
#endif

#ifdef SP_HAVE_OVERFLOW_BUILTINS
static inline mrb_bool sp_int_add_overflow_p(mrb_int a, mrb_int b, mrb_int *r) {
  return __builtin_add_overflow(a, b, r);
}
static inline mrb_bool sp_int_sub_overflow_p(mrb_int a, mrb_int b, mrb_int *r) {
  return __builtin_sub_overflow(a, b, r);
}
static inline mrb_bool sp_int_mul_overflow_p(mrb_int a, mrb_int b, mrb_int *r) {
  return __builtin_mul_overflow(a, b, r);
}
#else
/* Portable fallback for compilers lacking __builtin_*_overflow.
   mrb_int is pointer-width (intptr_t), so compute in uintptr_t --
   unsigned overflow is well-defined wrap-around in C -- and detect
   signed overflow via the sign-bit XOR trick at the *correct* width
   (the sign bit is mrb_int's top bit: 63 on 64-bit, 31 on 32-bit).
   Bounds use INTPTR_MAX/MIN, not the int64 MRB_INT_* macros, so this
   path is self-contained and width-correct. Mul checks bounds before
   multiplying because a 2x-width intermediate isn't portable. */
#define SP_INT_OVF_SIGN ((uintptr_t)1 << (sizeof(mrb_int) * 8 - 1))
static inline mrb_bool sp_int_add_overflow_p(mrb_int a, mrb_int b, mrb_int *r) {
  uintptr_t x = (uintptr_t)a, y = (uintptr_t)b, z = x + y;
  *r = (mrb_int)z;
  return !!(((x ^ z) & (y ^ z)) & SP_INT_OVF_SIGN);
}
static inline mrb_bool sp_int_sub_overflow_p(mrb_int a, mrb_int b, mrb_int *r) {
  uintptr_t x = (uintptr_t)a, y = (uintptr_t)b, z = x - y;
  *r = (mrb_int)z;
  return !!(((x ^ z) & (~y ^ z)) & SP_INT_OVF_SIGN);
}
static inline mrb_bool sp_int_mul_overflow_p(mrb_int a, mrb_int b, mrb_int *r) {
  if (a > 0 && b > 0 && a > INTPTR_MAX / b) { *r = a * b; return TRUE; }
  if (a < 0 && b > 0 && a < INTPTR_MIN / b) { *r = a * b; return TRUE; }
  if (a > 0 && b < 0 && b < INTPTR_MIN / a) { *r = a * b; return TRUE; }
  if (a < 0 && b < 0 && (a <= INTPTR_MIN || b <= INTPTR_MIN || -a > INTPTR_MAX / -b)) {
    *r = a * b; return TRUE;
  }
  *r = a * b;
  return FALSE;
}
#undef SP_INT_OVF_SIGN
#endif

/* Three modes selected by `--int-overflow=raise|wrap|promote` on the
   spinel wrapper, which passes -DSP_INT_OVERFLOW_MODE_{RAISE,WRAP,
   PROMOTE}. WRAP skips the check entirely. PROMOTE still raises at
   this layer -- the actual promotion semantics is implemented in the
   analyzer by rewriting every int local as bigint, so the helpers
   below are only reached on the few residual sites that stay int
   even in promote mode (FFI argument coercion, etc.). */
#ifdef SP_INT_OVERFLOW_MODE_WRAP
#  define sp_int_add(a, b) ((a) + (b))
#  define sp_int_sub(a, b) ((a) - (b))
#  define sp_int_mul(a, b) ((a) * (b))
#  define sp_int_neg(a)    (-(a))
#else
#  define sp_int_add(a, b) ({ mrb_int _sp_a = (a), _sp_b = (b), _sp_r; \
    if (sp_int_add_overflow_p(_sp_a, _sp_b, &_sp_r)) sp_raise_cls("RangeError", "integer overflow in +"); \
    _sp_r; })
#  define sp_int_sub(a, b) ({ mrb_int _sp_a = (a), _sp_b = (b), _sp_r; \
    if (sp_int_sub_overflow_p(_sp_a, _sp_b, &_sp_r)) sp_raise_cls("RangeError", "integer overflow in -"); \
    _sp_r; })
#  define sp_int_mul(a, b) ({ mrb_int _sp_a = (a), _sp_b = (b), _sp_r; \
    if (sp_int_mul_overflow_p(_sp_a, _sp_b, &_sp_r)) sp_raise_cls("RangeError", "integer overflow in *"); \
    _sp_r; })
#  define sp_int_neg(a)    ({ mrb_int _sp_a = (a), _sp_r; \
    if (sp_int_sub_overflow_p((mrb_int)0, _sp_a, &_sp_r)) sp_raise_cls("RangeError", "integer overflow in -@"); \
    _sp_r; })
#endif

/* sp_gcd / sp_lcm / sp_powmod / sp_ceildiv / sp_int_clamp / sp_int_sqrt
   now live in libspinel_rt.a (lib/sp_core.c); declared via sp_core.h. */
static inline char *sp_str_alloc_raw(size_t total_with_null);  /* fwd decl */
static const char*sp_int_chr(mrb_int n){char*s=sp_str_alloc_raw(2);s[0]=(char)n;s[1]=0;sp_str_set_len(s,1);return s;}
/* sp_ipow10 / sp_int_round / sp_int_ceil / sp_int_floor /
   sp_int_truncate / sp_str_oct now live in libspinel_rt.a
   (lib/sp_core.c); declared via sp_core.h. */
/* Narrow a foreign 64-bit value (time_t / off_t, etc.) to a Ruby
   Integer. On 64-bit mrb_int this is the identity. On 32-bit, a value
   outside the mrb_int range can't be represented; rather than silently
   truncating a clock/size value the program never computed (which
   `int-overflow=wrap` is NOT meant to license -- wrap is about the
   user's own arithmetic), raise RangeError. promote-mode bigint
   promotion of these boundary values is a follow-up. */
static inline mrb_int sp_i64_to_int(int64_t v){
#if INTPTR_MAX == INT64_MAX
  return (mrb_int)v;
#else
  if(v < (int64_t)INTPTR_MIN || v > (int64_t)INTPTR_MAX)
    sp_raise_cls("RangeError","value out of range for 32-bit Integer");
  return (mrb_int)v;
#endif
}

/* Forward decls for helpers used across this header (and by the
   string->number parsers that now live in libspinel_rt.a). */
SP_NORETURN SP_COLD void sp_raise_cls(const char *cls, const char *msg);
const char *sp_sprintf(const char *fmt, ...);

/* String -> number parsers now live in libspinel_rt.a (lib/sp_core.c). */
#include "sp_core.h"
/* system()/backtick ($?) support now lives in libspinel_rt.a (lib/sp_system.c). */
#include "sp_system.h"

/* Math.* wrappers that raise Math::DomainError on out-of-domain input, per
   CRuby. The runtime exception name is the flattened "Math_DomainError"
   (the codegen maps a `rescue Math::DomainError` path to that form, matching
   the StringScanner::Error -> StringScanner_Error precedent). Only the
   domain-restricted methods get wrappers; cos/sin/tan/atan/sinh/cosh/tanh/
   asinh/exp/cbrt/erf/erfc/atan2/hypot accept all reals and call libc
   directly from codegen. log(0) is -Infinity in CRuby (no raise); only a
   negative argument raises. atanh's endpoints (|x| == 1) yield ±Infinity and
   do NOT raise in CRuby -- only |x| > 1 is out of domain. CRuby's Math.*
   message names the function WITHOUT quotes (e.g. `... out of domain - sqrt`),
   unlike Integer.sqrt which quotes "isqrt". */
static inline mrb_float sp_math_sqrt(mrb_float x){if(x<0.0)sp_raise_cls("Math_DomainError","Numerical argument is out of domain - sqrt");return sqrt(x);}
static inline mrb_float sp_math_log(mrb_float x){if(x<0.0)sp_raise_cls("Math_DomainError","Numerical argument is out of domain - log");return log(x);}
static inline mrb_float sp_math_log2(mrb_float x){if(x<0.0)sp_raise_cls("Math_DomainError","Numerical argument is out of domain - log2");return log2(x);}
static inline mrb_float sp_math_log10(mrb_float x){if(x<0.0)sp_raise_cls("Math_DomainError","Numerical argument is out of domain - log10");return log10(x);}
static inline mrb_float sp_math_acos(mrb_float x){if(x<-1.0||x>1.0)sp_raise_cls("Math_DomainError","Numerical argument is out of domain - acos");return acos(x);}
static inline mrb_float sp_math_asin(mrb_float x){if(x<-1.0||x>1.0)sp_raise_cls("Math_DomainError","Numerical argument is out of domain - asin");return asin(x);}
static inline mrb_float sp_math_acosh(mrb_float x){if(x<1.0)sp_raise_cls("Math_DomainError","Numerical argument is out of domain - acosh");return acosh(x);}
static inline mrb_float sp_math_atanh(mrb_float x){if(x<-1.0||x>1.0)sp_raise_cls("Math_DomainError","Numerical argument is out of domain - atanh");return atanh(x);}
/* gamma has poles at the non-positive integers, but CRuby only raises at the
   NEGATIVE integers (gamma(0.0) is +Infinity, gamma(-0.0) -Infinity, which
   tgamma already yields); negative non-integers are in-domain. */
static inline mrb_float sp_math_gamma(mrb_float x){if(x<0.0&&x==floor(x))sp_raise_cls("Math_DomainError","Numerical argument is out of domain - gamma");return tgamma(x);}

static sp_Range sp_range_new(mrb_int f,mrb_int l,mrb_int e){sp_Range r;r.first=f;r.last=l;r.excl=e;r.step=0;return r;}
static sp_Range sp_range_new_step(mrb_int f,mrb_int l,mrb_int e,mrb_int s){sp_Range r;r.first=f;r.last=l;r.excl=e;r.step=s;return r;}
/* The effective stride: a literal `a..b` range stores 0, which iterates by +1. */
static inline mrb_int sp_range_step(sp_Range r){return r.step==0?1:r.step;}
/* Number of elements the range enumerates (0 for an empty one), honoring step. */
static inline mrb_int sp_range_count(sp_Range r){
  mrb_int s=sp_range_step(r);
  mrb_int lastv=r.excl?(r.last-(s>0?1:-1)):r.last;
  mrb_int n=(lastv-r.first)/s+1;
  return n<0?0:n;
}
/* Materialize the range into an int array (ascending or descending per step).
   The +1 stride (every literal `a..b` range) keeps the tight from_range loop; a
   real step only appears for downto / explicit step, so it pays the general
   path only then. */
static inline sp_IntArray *sp_range_to_ia(sp_Range r){
  mrb_int s=sp_range_step(r);
  if(s==1)return sp_IntArray_from_range(r.first,r.last-r.excl);
  return sp_IntArray_from_range_step(r.first,r.last,s,r.excl);
}
/* Last enumerated element (== first for an empty range), and the min/max of the
   enumerated set -- direction-aware, so a descending range reports them right. */
static inline mrb_int sp_range_last_elem(sp_Range r){
  mrb_int n=sp_range_count(r);
  return n<=0?r.first:r.first+(n-1)*sp_range_step(r);
}
static inline mrb_int sp_range_min_v(sp_Range r){ mrb_int a=r.first,b=sp_range_last_elem(r); return a<b?a:b; }
static inline mrb_int sp_range_max_v(sp_Range r){ mrb_int a=r.first,b=sp_range_last_elem(r); return a>b?a:b; }
static mrb_bool sp_range_eq(sp_Range a,sp_Range b){return a.first==b.first&&a.last==b.last&&a.excl==b.excl;}
/* `Range#include?`/`#cover?` on the boxed (SP_TAG_OBJ cls_id
   SP_BUILTIN_RANGE) Range value. The direct sp_Range typed path
   inlines this same check via compile_range_method_expr; poly-recv
   dispatch needs the wrapper so the cls_id arm in
   emit_poly_builtin_dispatch can land on a single C expression. An
   exclusive range stops one short of `last`, so the upper bound is
   `last - excl` (excl is 0 or 1). */
static mrb_bool sp_range_include(sp_Range *r, mrb_int x){mrb_int lo=sp_range_min_v(*r),hi=sp_range_max_v(*r);return sp_range_count(*r)>0 && lo<=x && x<=hi;}

/* ---- Class object ----
   Value-type Class reference: a single class id that indexes into
   the per-program sp_class_names[] table emitted by codegen. Lets
   `c = Foo` produce a runtime value (`(sp_Class){<id>}`) instead of
   a bare C identifier, and `c.to_s` lower to a names-table lookup.
   Other Class methods (`.name`, `.inspect`, `.==`, `.!=`,
   `.superclass`, `.ancestors`, dynamic `is_a?(c)` against a
   variable, etc.) are not yet supported. */

/* ---- Complex runtime ---- */
/* Value-type Cartesian Complex: 16 bytes, passed by value. Used by
   optcarrot's nestopia palette generator; the palette is precomputed
   in the default code path so this is exercised only with
   `--nestopia-palette`. */
/* Complex arithmetic + inspect/to_s moved to lib/sp_format.c (cold; optcarrot
   touches Complex only under --nestopia-palette). */

/* ---- Rational runtime ---- */
/* Value-type Rational: 16 bytes (two mrb_ints), passed by value.
   Stored in reduced form -- the parser hands us the already-reduced
   numerator/denominator from the literal; Integer#quo / arithmetic
   normalizes via sp_rational_reduce. Issue #841. */
/* Rational construction + arithmetic + inspect/to_s moved to lib/sp_format.c
   (cold; only reached when a program actually uses Rational). */

/* ---- Time runtime ---- */
/* sp_Time and the libc-backed accessors / formatters live in
   lib/sp_time.{c,h} (compiled into libspinel_rt.a). What stays here
   are the GC-aware wrappers — sp_box_time copies the value onto the
   GC heap, and the *_gc string forwarders allocate a small stack buf,
   call the libspinel_rt format helper, then sp_str_dup_external the
   result into the GC string heap. is_utc distinguishes UTC-coerced
   times from local-zone times; the underlying epoch is the same. */
#include "sp_time.h"


/* `recycle`: optional sweep hook. If non-NULL, sp_gc_collect calls
   recycle(h) on the unmarked object instead of finalize+free. The
   hook is responsible for deciding whether to keep the storage
   (pool push) or free it. Used by class-instance free-list pools. */
/* sp_gc_heap / sp_gc_bytes are defined in lib/sp_gc.c (extern via sp_gc.h). */
/* sp_gc_threshold moved to sp_alloc.c (extern, shared) */

/* ---- GC verify: opt-in mark-path validation (release-neutral) ------------
 * SPINEL_GC_VERIFY=1  before the collector invokes an object's scan hook,
 *   check that the object is a currently-registered heap allocation. If a
 *   raw/aliased pointer (e.g. into a string or builder buffer) has been put
 *   on the mark path, abort with a diagnostic at that point instead of
 *   calling through a bogus scan-function pointer (which otherwise faults at
 *   a garbage address and is near-impossible to attribute).
 * Default OFF; with it unset, behavior and codegen are unchanged. */
/* GC verify state + helpers live in lib/sp_gc.c. */

/* ---- String GC ---- */
/* The string heap state (sp_str_heap, sp_str_heap_bytes, sp_str_threshold...),
   the allocators (sp_str_alloc / _raw / byte_len / set_len / from_bytes /
   dup_external), sp_str_empty, and the UTF-8 length cache now live in
   sp_alloc.h / sp_alloc.c, shared (extern) so standalone lib C files can
   allocate onto the same heap. sp_str_sweep moved to sp_alloc.c. */
#define SPL(s) (&("\xff" s)[1])

/* RUBY_PLATFORM string -- host arch + OS. Detected at C compile time
   so cross-builds report the target platform. Issue #890. */
#if defined(__x86_64__) || defined(_M_X64)
#  define SP_RUBY_ARCH "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define SP_RUBY_ARCH "aarch64"
#elif defined(__i386__) || defined(_M_IX86)
#  define SP_RUBY_ARCH "i686"
#elif defined(__arm__)
#  define SP_RUBY_ARCH "arm"
#else
#  define SP_RUBY_ARCH "unknown"
#endif
#if defined(__linux__)
#  define SP_RUBY_OS "linux"
#elif defined(__APPLE__)
#  define SP_RUBY_OS "darwin"
#elif defined(__FreeBSD__)
#  define SP_RUBY_OS "freebsd"
#else
#  define SP_RUBY_OS "unknown"
#endif
static const char sp_ruby_platform_data[] = "\xff" SP_RUBY_ARCH "-" SP_RUBY_OS;
static inline const char *sp_ruby_platform_str(void) { return sp_ruby_platform_data + 1; }

/* Process.ppid wrapper. */
static inline mrb_int sp_process_ppid(void) {
  return (mrb_int)getppid();
}

static inline double sp_process_clock_gettime(void) {
#if defined(CLOCK_MONOTONIC)
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ((double)ts.tv_nsec * 1e-9);
#else
  return 0.0;
#endif
}

/* ---- Encoding runtime ----
   Spinel currently assumes UTF-8 source and string data. Keep Encoding
   as a tiny value type so `__ENCODING__` and `String#encoding` can
   answer Ruby's Encoding-shaped protocol without carrying full
   transcoding state. */
/* The name is a spinel rodata-literal string (0xff marker prefix), not a bare
   C literal: it flows into sp_str_hash / sp_str_byte_len etc. as a hash key
   and String value, all of which read the marker byte at s[-1]. A bare literal
   has no controlled preceding byte, so that read is a global-buffer-overflow
   (ASAN-caught) and could spuriously hit the heap-header cache path (#282). */
static inline sp_Encoding sp_encoding_utf8(void){return(sp_Encoding){&("\xff" "UTF-8")[1]};}
static inline sp_Encoding sp_encoding_us_ascii(void){return(sp_Encoding){&("\xff" "US-ASCII")[1]};}
static inline const char*sp_encoding_name(sp_Encoding e){return e.name?e.name:sp_str_empty;}
static inline const char*sp_encoding_inspect(sp_Encoding e){return sp_sprintf("#<Encoding:%s>",sp_encoding_name(e));}
static inline mrb_bool sp_encoding_eq(sp_Encoding a,sp_Encoding b){const char*an=sp_encoding_name(a);const char*bn=sp_encoding_name(b);return strcmp(an,bn)==0;}

/* sp_str_alloc / _raw / byte_len / set_len / from_bytes / dup_external moved to
   sp_alloc.h (shared inline over extern heap state). */

/* ---- UTF-8 helpers (used throughout the string runtime below) ---- */
/* Bytes to advance past the codepoint at p (caller guarantees *p != 0).
   Caps at NUL and validates the continuation-byte pattern, so malformed or
   truncated UTF-8 never advances past the terminator. */
/* Issue #882: `"hello" << 33` should append the character with
   that codepoint, not the decimal digits. UTF-8 encode (1..4 bytes)
   and return a NUL-terminated string. */
static const char *sp_int_codepoint_to_str(mrb_int n) {
  char *s = sp_str_alloc_raw(5);
  if (n < 0 || n > 0x10FFFF) { s[0] = 0; sp_str_set_len(s, 0); return s; }
  int len = sp_utf8_encode((uint32_t)n, s);
  s[len] = 0;
  sp_str_set_len(s, (size_t)len);  /* byte_len must be the encoded length, not the alloc */
  return s;
}
/* Direct-mapped pointer-keyed cache for (byte_len, char_len). Populated lazily
   by sp_str_length / sp_utf8_byte_offset; the same entries unlock both calls
   so iterating a single string with `s.length` + `s[i]` walks UTF-8 once
   instead of per access. ASCII strings (char_len == byte_len) take an O(1)
   byte_offset fast path. Cleared by sp_str_sweep so freed-and-reused heap
   addresses can't leak stale entries. Skipped for sp_String wrappers (marker
   0xfd) whose buffers can move on realloc. */
/* SP_STR_LCACHE_*, sp_str_lcache, and sp_str_lcache_clear live in sp_alloc.h /
   sp_alloc.c (shared so the archive-side sweep flushes this TU's cache). */

/* Count UTF-8 code points in s[0..bl). The 8-byte ASCII-detect prologue skips
   bytes 8 at a time while the high bit stays clear, so pure-ASCII strings (the
   common case in the JSON / CSV / template benchmarks) run vastly faster than
   the per-byte advance loop they used to fall through. */

/* True when `s` carries one of spinel's own string markers in the
   preceding byte (0xfe / 0xfc heap, 0xff rodata literal). FFI returns
   a bare `const char *` whose preceding byte is whatever C variable
   sits before the buffer in memory — using the pointer as a cache
   key without this gate aliased subsequent FFI calls into the prior
   call's cached length (#611). 0xfd (sp_String wrapper) is excluded
   too because its buffer can move on append. */

/* NULL-safe string equality. ENV[] returns NULL for unset vars
   (the dispatch is `sp_str_dup_external(getenv(...))`, which propagates
   NULL), so emitted strcmp(...) on the result of `ENV["X"] == "1"` would
   dereference NULL on either side. nil-vs-string equality is false in
   Ruby; nil == nil is true, so falling back to pointer equality on the
   NULL path covers both. */
static inline int sp_str_eq(const char*a,const char*b){
  if(a==b)return 1;
  if(!a||!b)return 0;
  if(strcmp(a,b)!=0)return 0;
  /* strcmp equality is only prefix equality when a length header records
     an embedded NUL ("a\0b" vs "a"): confirm byte-exact equality. The
     miss path above stays a single strcmp. */
  size_t la=sp_str_byte_len(a);
  return la==sp_str_byte_len(b)&&memcmp(a,b,la)==0;
}
/* Issue #762: check malloc/realloc returns. On OOM, return an empty
   array rather than dereferencing NULL. */

/* Issue #858: expand `a-z` range notation in a String#delete /
   String#tr / String#count character set. `^abc` negation is
   NOT handled (separate v1 scope). Result is a malloc'd flat
   codepoint array — caller frees. */

/* sp_mark_string is an inline helper in sp_gc.h. sp_str_sweep moved to
   sp_alloc.c (single definition, registered with the GC there). */

/* Time formatters (strftime / iso8601 / zone / inspect_v) and the cold
   value ops (cmp / add_f / add_i / sub_i / sub_t) live in lib/sp_time.c;
   the formatters now return GC-heap strings directly, so no trampoline
   is needed here. */

/* SP_GC_STACK_MAX, sp_gc_roots, sp_gc_nroots come from sp_gc.h / lib/sp_gc.c. */
/* Cooperative-fiber GC root storage (issue #636).
   sp_gc_roots[] holds the CURRENT fiber's active roots. When a fiber
   yields, its roots get copied out to the fiber's saved_roots buffer
   and the resuming fiber's saved_roots are copied back in — so the
   per-fiber stacks never clobber each other through interleaved
   pushes the way they did when a single global stack was shared by
   every fiber. sp_gc_mark_all calls the hook below (installed once
   the Fiber section's setup runs) to walk every live fiber's
   saved_roots in addition to the current view. */
/* SP_GC_ROOT / SP_GC_SAVE / SP_GC_RESTORE and the _sp_gc_root_push/pop helpers
   moved to sp_gc.h (shared so lib/sp_marshal.c can root its in-flight objects).
   sp_re_mark_globals is defined below (with the regex globals it marks) and
   carries external linkage so the collector body can reach it. */
#define SP_GC_MARK_STACK_MAX (1024*64)
#define SP_GC_NBUCKETS 32
static sp_gc_hdr*sp_gc_buckets[SP_GC_NBUCKETS];
static inline int sp_gc_bucket(size_t sz){int b=(int)(sz/16);return b<SP_GC_NBUCKETS?b:SP_GC_NBUCKETS-1;}
/* sp_gc_cycle / sp_gc_old_bytes are in lib/sp_gc.c (extern via sp_gc.h);
   sp_gc_old_heap is collector-private to lib/sp_gc.c. */

/* GC verify support + sp_gc_collect live in lib/sp_gc.c. */
/* sp_gc_threshold_init moved to sp_alloc.c */
/* sp_oom_die + the SPINEL_MAX_HEAP_MB governor (sp_gc_enforce_mem_limit)
   live in lib/sp_gc.c. */
/* SPINEL_GC_STRESS=1: shrink the collection threshold to a few KB so a
   cycle runs at almost every allocation. A rooting hole that normal
   thresholds hide (the GC rarely lands inside the vulnerable window)
   becomes a deterministic failure; pair with SPINEL_GC_VERIFY=1. */
/* sp_gc_stress_checked moved to sp_alloc.c */
/* sp_gc_alloc / sp_gc_alloc_nogc moved to sp_alloc.h (shared inline over the
   extern heap + threshold state). */
/* GC-header frozen bit — used for containers whose mutators are NOT
   on a hot path (hashes), so the extra cache line vs. a struct field
   doesn't matter. Arrays co-locate `frozen` in the struct instead
   (see sp_IntArray); strings use the 0xff marker / wrapper bit. */
static inline mrb_bool sp_gc_is_frozen(void *p) { if (!p) return FALSE; return ((sp_gc_hdr *)((char *)p - sizeof(sp_gc_hdr)))->frozen; }
static inline void *sp_gc_freeze(void *p) { if (p) ((sp_gc_hdr *)((char *)p - sizeof(sp_gc_hdr)))->frozen = 1; return p; }
/* marker-prefixed message: see sp_raise_frozen_array (lib/sp_alloc.h) */
static void __attribute__((noinline,cold)) sp_raise_frozen_hash(void){sp_raise_cls("FrozenError",(&("\xff" "can't modify frozen Hash")[1]));}
/* Pool-aware alloc. The recycle hook is stored in the gc_hdr; sweep
   calls it on unmarked objects instead of finalize+free. The hook
   decides whether to push the storage onto a per-class free-list or
   actually free it. */
static void *sp_gc_alloc_pool(size_t sz, void(*scn)(void*), void(*recycle)(sp_gc_hdr*)) {
  void *p = sp_gc_alloc(sz, NULL, scn);
  sp_gc_hdr *h = (sp_gc_hdr *)((char *)p - sizeof(sp_gc_hdr));
  h->recycle = recycle;
  return p;
}
/* Re-link a previously-pooled slot back into sp_gc_heap so the next
   GC cycle visits it. Called by class _new functions when reusing
   a pooled instance. The storage was kept alive by the pool
   free-list since the last sweep unlinked it from sp_gc_heap. */
/* The heap list is shared across workers; a pooled re-link used to push onto
   it bare, racing sp_gc_alloc's push at N>1 (a lost link is a leaked-then-UAF
   header). SP_GC_HEAP_PUSH is a lock-free CAS push in the threaded build --
   the pool hit stays off the heap mutex -- and the exact plain push in the
   single-threaded one (see sp_gc.h). */
static void sp_gc_pool_relink(sp_gc_hdr *h) {
  h->marked = 0;
  SP_GC_HEAP_PUSH(h);
  SP_GC_CTR_ADD(sp_gc_bytes, h->size);
}

/* Per-class free-list pool boilerplate. SP_POOL_DEFINE(CLS) goes at
   file scope, near the class _new function. SP_POOL_NEW(CLS, scan)
   replaces the body of an `sp_gc_alloc(sizeof(sp_CLS), NULL, scan)`
   call, popping from the per-class free-list if non-empty.
   Default cap can be overridden at runtime via SP_POOL_MAX envvar
   (uniform across classes). SP_POOL_REPORT=1 dumps per-class stats
   at exit. */
#define SP_POOL_DEFAULT_MAX 1048576L
/* Pool concurrency (threaded build). The free lists stay SHARED across
   workers -- pushes (recycle) happen only during the stop-the-world sweep,
   where every mutator is parked, so they stay plain stores; pops run
   concurrently on any worker and go through a lock-free CAS (below). A
   concurrent-pop-only Treiber stack has no ABA window: a popped node can
   only reappear on the list via the sweep, and no sweep can run while a
   mutator is mid-pop (it is not at a safepoint). A shared list also keeps
   recycled storage visible to every worker -- per-worker (TLS) lists would
   strand the whole recycle crop on whichever worker ran the collection.
   pool_count/pops are adjusted by concurrent poppers, so they use relaxed
   atomics; pushes/freed/hwm are sweep-only (exclusive), so they stay plain.
   Single-threaded: the macros expand to the exact plain code this had. */
#ifdef SP_THREADS
static inline sp_gc_hdr *sp_pool_try_pop(sp_gc_hdr **head) {
  sp_gc_hdr *old = __atomic_load_n(head, __ATOMIC_ACQUIRE);
  while (old) {
    /* A stale `old` may already belong to another worker, which is
       concurrently rewriting old->next (its relink push). The atomic load
       keeps that defined; the CAS below then fails and retries with a
       fresh head, discarding the stale next. */
    sp_gc_hdr *nxt = __atomic_load_n(&old->next, __ATOMIC_RELAXED);
    if (__atomic_compare_exchange_n(head, &old, nxt, 1,
                                    __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) break;
  }
  return old;
}
#define SP_POOL_CTR_INC(c) __atomic_fetch_add(&(c), 1, __ATOMIC_RELAXED)
#define SP_POOL_CTR_DEC(c) __atomic_fetch_sub(&(c), 1, __ATOMIC_RELAXED)
#else
static inline sp_gc_hdr *sp_pool_try_pop(sp_gc_hdr **head) {
  sp_gc_hdr *old = *head;
  if (old) *head = old->next;
  return old;
}
#define SP_POOL_CTR_INC(c) ((c)++)
#define SP_POOL_CTR_DEC(c) ((c)--)
#endif
#define SP_POOL_DEFINE(CLS) \
  static sp_gc_hdr *sp_##CLS##_pool_head = NULL; \
  static long sp_##CLS##_pool_count = 0; \
  static long sp_##CLS##_pool_max = SP_POOL_DEFAULT_MAX; \
  static long sp_##CLS##_pool_pushes = 0; \
  static long sp_##CLS##_pool_pops = 0; \
  static long sp_##CLS##_pool_freed = 0; \
  static long sp_##CLS##_pool_hwm = 0; \
  __attribute__((constructor)) static void sp_##CLS##_pool_init(void) { \
    const char *m = getenv("SP_POOL_MAX"); \
    if (m && *m) { long v = atol(m); if (v >= 0) sp_##CLS##_pool_max = v; } \
  } \
  /* Runs only from the sweep (stop-the-world in the threaded build), so the
     list mutation needs no CAS -- no pop can run concurrently. */ \
  static void sp_##CLS##_pool_recycle(sp_gc_hdr *h) { \
    if (sp_##CLS##_pool_count >= sp_##CLS##_pool_max) { \
      free(h); sp_##CLS##_pool_freed++; return; \
    } \
    h->next = sp_##CLS##_pool_head; \
    sp_##CLS##_pool_head = h; \
    sp_##CLS##_pool_count++; \
    sp_##CLS##_pool_pushes++; \
    if (sp_##CLS##_pool_count > sp_##CLS##_pool_hwm) sp_##CLS##_pool_hwm = sp_##CLS##_pool_count; \
  } \
  __attribute__((destructor)) static void sp_##CLS##_pool_report(void) { \
    const char *e = getenv("SP_POOL_REPORT"); \
    if (!e || !e[0] || e[0] == '0') return; \
    fprintf(stderr, #CLS " pool: pops=%ld pushes=%ld over_cap_freed=%ld hwm=%ld retained=%ld cap=%ld\n", \
      sp_##CLS##_pool_pops, sp_##CLS##_pool_pushes, sp_##CLS##_pool_freed, \
      sp_##CLS##_pool_hwm, sp_##CLS##_pool_count, sp_##CLS##_pool_max); \
  }

#define SP_POOL_NEW(CLS, SCAN) (__extension__ ({ \
  sp_##CLS *_p; \
  sp_gc_hdr *_h = sp_pool_try_pop(&sp_##CLS##_pool_head); \
  if (_h) { \
    SP_POOL_CTR_DEC(sp_##CLS##_pool_count); \
    SP_POOL_CTR_INC(sp_##CLS##_pool_pops); \
    sp_gc_pool_relink(_h); \
    _h->recycle = sp_##CLS##_pool_recycle; \
    _p = (sp_##CLS *)((char *)_h + sizeof(sp_gc_hdr)); \
  } \
  else { \
    _p = (sp_##CLS *)sp_gc_alloc_pool(sizeof(sp_##CLS), SCAN, sp_##CLS##_pool_recycle); \
  } \
  _p; \
}))

/* `Object.new` — a sentinel object whose only meaningful property is
   identity. Each call returns a fresh GC-managed allocation, so two
   `Object.new` results compare as `!=` via their pointer addresses. */
typedef struct sp_Object_s { uint8_t _pad; } sp_Object;
static sp_Object *sp_Object_new(void){return(sp_Object*)sp_gc_alloc(sizeof(sp_Object),NULL,NULL);}

/* sp_IntArray lives in sp_array.h (hot core inline) + lib/sp_array.c
   (cold ops). The Integer methods that happen to build an IntArray stay
   here; they call the inline sp_IntArray_new / _push from sp_array.h. */
static sp_IntArray*sp_int_digits(mrb_int n,mrb_int base){sp_IntArray*a=sp_IntArray_new();if(base<2)base=10;if(n==0){sp_IntArray_push(a,0);return a;}if(n<0)n=-n;while(n>0){sp_IntArray_push(a,n%base);n/=base;}return a;}
/* Integer#bit_length: bits in the two's-complement representation excluding
   the sign bit (a negative n counts the bits of ~n). */
static mrb_int sp_int_bit_length(mrb_int n){unsigned long long x=(n<0)?(unsigned long long)(~n):(unsigned long long)n;mrb_int b=0;if(x>=1ULL<<32){b+=32;x>>=32;}if(x>=1ULL<<16){b+=16;x>>=16;}if(x>=1ULL<<8){b+=8;x>>=8;}if(x>=1ULL<<4){b+=4;x>>=4;}if(x>=1ULL<<2){b+=2;x>>=2;}if(x>=1ULL<<1){b+=1;x>>=1;}return b+(mrb_int)x;}
/* Integer#[start, len]: the len-bit field starting at bit `start`, i.e.
   (n >> start) & ((1 << len) - 1) with Ruby's shift semantics (a negative
   count shifts the other way), clamped to the 64-bit word so an out-of-range
   start/len can't trigger an undefined shift. The receiver shift is arithmetic
   so a negative `n`'s high bits read as 1. */
static mrb_int sp_int_bit_range(mrb_int n, mrb_int start, mrb_int len) {
  mrb_int shifted;
  if (start >= 0) shifted = (start >= 64) ? (n < 0 ? -1 : 0) : (n >> start);
  else { mrb_int s = -start; shifted = (s >= 64) ? 0 : (mrb_int)((uint64_t)n << s); }
  uint64_t mask = (len <= 0) ? (len == 0 ? (uint64_t)0 : ~(uint64_t)0)
                             : (len >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << len) - 1));
  return (mrb_int)((uint64_t)shifted & mask);
}
/* sp_FloatArray lives in sp_array.h (hot core inline) + lib/sp_array.c
   (cold ops). */

/* sp_PtrArray lives in sp_array.h (hot core inline) + lib/sp_array.c
   (cold ops). */

/* sp_StrArray lives in sp_array.h (hot core inline) + lib/sp_array.c
   (cold ops). sp_StrArray_from_string_range stays here because it needs
   sp_str_succ (a string-batch helper defined further down). */
/* Forward decl — sp_str_succ is defined further down (it uses
   sp_utf8_decode); the StrArray_from_string_range loop below needs
   it visible early. */
/* String range to_a — single-char and multi-char ASCII ranges via
   sp_str_succ. The 4096-iteration cap stops a pathological prepend-
   style infinite loop before it eats memory. */
/* Case-insensitive string compare. Portable across glibc / MinGW
   (avoids strcasecmp which lives in strings.h on POSIX and is named
   stricmp on Windows). Returns -1 / 0 / 1 like CRuby's String#casecmp. */
/* String#valid_encoding? — walks the buffer and accepts pure ASCII
   or well-formed UTF-8 (RFC 3629 byte sequences with no overlong
   forms, no surrogate halves, code points <= U+10FFFF). */

static inline uint64_t sp_str_hash_compute(const char*s){uint64_t h=14695981039346656037ULL;while(*s){h^=(unsigned char)*s++;h*=1099511628211ULL;}return h;}
/* Cold path: compute (and, for a heap/heap-frozen string, cache) the FNV
   hash. Kept out-of-line so sp_str_hash's inline fast path -- a cached-hash
   read -- stays tiny and doesn't bloat every call site's code layout. */
static SP_NOINLINE uint64_t sp_str_hash_miss(const char*s,unsigned char m){
  if(m==0xfe||m==0xfc||m==0xf1){
    sp_str_hdr*hd=((sp_str_hdr*)(s-1))-1;
    uint64_t h=sp_str_hash_compute(s);
    hd->hash=h?h:1;
    return hd->hash;
  }
  return sp_str_hash_compute(s);
}
static inline uint64_t sp_str_hash(const char*s){
  unsigned char m=((const unsigned char*)s)[-1];
  if(m==0xfe||m==0xfc||m==0xf1){
    uint64_t cached=(((sp_str_hdr*)(s-1))-1)->hash;
    if(cached)return cached;
  }
  return sp_str_hash_miss(s,m);
}
static void sp_StrIntHash_fin(void*p){sp_StrIntHash*h=(sp_StrIntHash*)p;free(h->keys);free(h->vals);free(h->order);}
static void sp_StrIntHash_scan(void*p){sp_StrIntHash*h=(sp_StrIntHash*)p;for(mrb_int i=0;i<h->cap;i++){if(h->keys[i])sp_mark_string(h->keys[i]);}}
/* default_v is SP_INT_NIL for a hash with no explicit default ({} / {k=>v}),
   so a missing-key `[]` read surfaces Ruby nil (#801). Hash.new(N) sets it to
   N via _new_with_default. Proven-present internal reads use _get on present
   keys, so this only governs the miss path. */
static sp_StrIntHash*sp_StrIntHash_new(void){sp_StrIntHash*h=(sp_StrIntHash*)sp_gc_alloc(sizeof(sp_StrIntHash),sp_StrIntHash_fin,sp_StrIntHash_scan);h->cap=16;h->mask=15;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->order=(const char**)malloc(sizeof(const char*)*h->cap);h->len=0;h->default_v=SP_INT_NIL;return h;}
static sp_StrIntHash*sp_StrIntHash_new_with_default(mrb_int d){sp_StrIntHash*h=sp_StrIntHash_new();h->default_v=d;return h;}
static void sp_StrIntHash_grow(sp_StrIntHash*h){mrb_int oc=h->cap;const char**ok=h->keys;mrb_int*ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->order=(const char**)realloc(h->order,sizeof(const char*)*h->cap);h->len=0;for(mrb_int i=0;i<oc;i++){if(ok[i]){mrb_int idx=(mrb_int)(sp_str_hash(ok[i])&h->mask);while(h->keys[idx])idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}
static mrb_int sp_StrIntHash_get(sp_StrIntHash*h,const char*k){if(!h)return 0;mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k))return h->vals[idx];idx=(idx+1)&h->mask;}return h->default_v;}
/* Issue #801: maybe-missing public `[]` read. Returns default_v on a miss,
   which is SP_INT_NIL (Ruby nil at the value level) for a no-default hash and
   the explicit default for Hash.new(N). Proven-present reads keep using _get. */
static mrb_int sp_StrIntHash_get_opt(sp_StrIntHash*h,const char*k){if(!h)return SP_INT_NIL;mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k))return h->vals[idx];idx=(idx+1)&h->mask;}return h->default_v;}
static void sp_StrIntHash_set(sp_StrIntHash*h,const char*k,mrb_int v){if(h->len*2>=h->cap)sp_StrIntHash_grow(h);mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k)){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}
static mrb_bool sp_StrIntHash_has_key(sp_StrIntHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k))return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
/* Hash#value? -- scan values in insertion order. Issue #738. */
static mrb_bool sp_StrIntHash_has_value(sp_StrIntHash*h,mrb_int v){if(!h)return FALSE;for(mrb_int i=0;i<h->len;i++)if(sp_StrIntHash_get(h,h->order[i])==v)return TRUE;return FALSE;}
static mrb_int sp_StrIntHash_length(sp_StrIntHash*h){return h->len;}
static void sp_StrIntHash_delete(sp_StrIntHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k)){h->keys[idx]=NULL;h->vals[idx]=0;h->len--;mrb_int j=(idx+1)&h->mask;while(h->keys[j]){mrb_int nj=(mrb_int)(sp_str_hash(h->keys[j])&h->mask);if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];h->keys[j]=NULL;h->vals[j]=0;idx=j;}j=(j+1)&h->mask;}{mrb_int oi=0;while(oi<=h->len){if(strcmp(h->order[oi],k)==0){while(oi<h->len){h->order[oi]=h->order[oi+1];oi++;}break;}oi++;}}return;}idx=(idx+1)&h->mask;}}
static sp_StrArray*sp_StrIntHash_keys(sp_StrIntHash*h){SP_GC_ROOT(h);sp_StrArray*a=sp_StrArray_new();SP_GC_ROOT(a);for(mrb_int i=0;i<h->len;i++)sp_StrArray_push(a,h->order[i]);return a;}
static sp_IntArray*sp_StrIntHash_values(sp_StrIntHash*h){SP_GC_ROOT(h);sp_IntArray*a=sp_IntArray_new();SP_GC_ROOT(a);for(mrb_int i=0;i<h->len;i++)sp_IntArray_push(a,sp_StrIntHash_get(h,h->order[i]));return a;}
static sp_StrIntHash*sp_StrArray_tally(sp_StrArray*a){sp_StrIntHash*h=sp_StrIntHash_new();for(mrb_int i=0;i<a->len;i++){const char*k=a->data[i];mrb_int c=sp_StrIntHash_has_key(h,k)?sp_StrIntHash_get(h,k):0;sp_StrIntHash_set(h,k,c+1);}return h;}
static sp_StrIntHash*sp_StrIntHash_merge(sp_StrIntHash*a,sp_StrIntHash*b){sp_StrIntHash*r=sp_StrIntHash_new();r->default_v=a->default_v;for(mrb_int i=0;i<a->len;i++)sp_StrIntHash_set(r,a->order[i],sp_StrIntHash_get(a,a->order[i]));for(mrb_int i=0;i<b->len;i++)sp_StrIntHash_set(r,b->order[i],sp_StrIntHash_get(b,b->order[i]));return r;}
static void sp_StrIntHash_update(sp_StrIntHash*a,sp_StrIntHash*b){for(mrb_int i=0;i<b->len;i++)sp_StrIntHash_set(a,b->order[i],sp_StrIntHash_get(b,b->order[i]));}
static sp_StrIntHash*sp_StrIntHash_dup(sp_StrIntHash*h){sp_StrIntHash*r=sp_StrIntHash_new();r->default_v=h->default_v;for(mrb_int i=0;i<h->len;i++)sp_StrIntHash_set(r,h->order[i],sp_StrIntHash_get(h,h->order[i]));return r;}
static sp_StrIntHash*sp_StrIntHash_replace(sp_StrIntHash*h,sp_StrIntHash*o){if(!h)return h;for(mrb_int i=0;i<h->cap;i++)h->keys[i]=NULL;h->len=0;if(o)for(mrb_int i=0;i<o->len;i++)sp_StrIntHash_set(h,o->order[i],sp_StrIntHash_get(o,o->order[i]));return h;}
static void sp_StrIntHash_clear(sp_StrIntHash*h){if(!h)return;for(mrb_int i=0;i<h->cap;i++)h->keys[i]=NULL;h->len=0;}
static mrb_bool sp_StrIntHash_eq(sp_StrIntHash*a,sp_StrIntHash*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++){const char*k=a->order[i];if(!sp_StrIntHash_has_key(b,k))return FALSE;if(sp_StrIntHash_get(a,k)!=sp_StrIntHash_get(b,k))return FALSE;}return TRUE;}

/* GC.stat snapshot: String=>Integer hash over the collector globals.
   full_runs derives from sp_gc_cycle / SP_GC_FULL_INTERVAL (the major
   collection cadence). */
/* Keys are spinel rodata literals (SPL: 0xff marker prefix) so the str-hash
   header cache's s[-1] read is in-bounds -- a bare C literal here would
   overread (and could alias a heap marker on some rodata layouts). */
static sp_StrIntHash*sp_gc_stat(void){
  /* The string heap (sp_str_heap) is malloc'd separately and deliberately
     excluded from sp_gc_bytes (see sp_str_alloc). Surface its footprint so
     GC.stat can explain "RSS huge but bytes tiny" for string-heavy workloads.
     Prototype: O(n) walk; a production version maintains a running counter.
     The walk holds the heap lock: sp_str_alloc pushes onto this list under
     it from any worker, and an unlocked traversal could read a half-linked
     node. Unlock before building the hash -- sp_gc_alloc takes the same
     (non-recursive) lock. */
  size_t str_bytes=0; mrb_int str_count=0;
  SP_HEAP_LOCK();
  for(sp_str_hdr*sh=sp_str_heap; sh; sh=sh->next){ str_bytes+=sh->size; str_count++; }
  SP_HEAP_UNLOCK();
  sp_StrIntHash*h=sp_StrIntHash_new();sp_StrIntHash_set(h,SPL("bytes"),(mrb_int)SP_GC_CTR_GET(sp_gc_bytes));sp_StrIntHash_set(h,SPL("old_bytes"),(mrb_int)sp_gc_old_bytes);sp_StrIntHash_set(h,SPL("threshold"),(mrb_int)sp_gc_threshold);sp_StrIntHash_set(h,SPL("cycle"),(mrb_int)sp_gc_cycle);sp_StrIntHash_set(h,SPL("full_runs"),(mrb_int)(sp_gc_cycle/SP_GC_FULL_INTERVAL));sp_StrIntHash_set(h,SPL("str_bytes"),(mrb_int)str_bytes);sp_StrIntHash_set(h,SPL("str_count"),str_count);return h;}

static void sp_StrStrHash_fin(void*p){sp_StrStrHash*h=(sp_StrStrHash*)p;free(h->keys);free(h->vals);free(h->order);}
static void sp_StrStrHash_scan(void*p){sp_StrStrHash*h=(sp_StrStrHash*)p;for(mrb_int i=0;i<h->cap;i++){if(h->keys[i]){sp_mark_string(h->keys[i]);sp_mark_string(h->vals[i]);}}if(h->default_v)sp_mark_string(h->default_v);}
static sp_StrStrHash*sp_StrStrHash_new(void){sp_StrStrHash*h=(sp_StrStrHash*)sp_gc_alloc(sizeof(sp_StrStrHash),sp_StrStrHash_fin,sp_StrStrHash_scan);h->cap=16;h->mask=15;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(const char**)calloc(h->cap,sizeof(const char*));h->order=(const char**)malloc(sizeof(const char*)*h->cap);h->len=0;h->default_v=NULL;return h;}
static sp_StrStrHash*sp_StrStrHash_new_with_default(const char*d){sp_StrStrHash*h=sp_StrStrHash_new();h->default_v=d;return h;}
static void sp_StrStrHash_grow(sp_StrStrHash*h){mrb_int oc=h->cap;const char**ok=h->keys;const char**ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(const char**)calloc(h->cap,sizeof(const char*));h->order=(const char**)realloc(h->order,sizeof(const char*)*h->cap);h->len=0;for(mrb_int i=0;i<oc;i++){if(ok[i]){mrb_int idx=(mrb_int)(sp_str_hash(ok[i])&h->mask);while(h->keys[idx])idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}
static const char*sp_StrStrHash_get(sp_StrStrHash*h,const char*k){if(!h)return NULL;mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k))return h->vals[idx];idx=(idx+1)&h->mask;}return h->default_v;}
static void sp_StrStrHash_set(sp_StrStrHash*h,const char*k,const char*v){if(h->len*2>=h->cap)sp_StrStrHash_grow(h);mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k)){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}
static mrb_bool sp_StrStrHash_has_key(sp_StrStrHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k))return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static mrb_bool sp_StrStrHash_has_value(sp_StrStrHash*h,const char*v){if(!h||!v)return FALSE;for(mrb_int i=0;i<h->len;i++){const char*x=sp_StrStrHash_get(h,h->order[i]);if(x&&strcmp(x,v)==0)return TRUE;}return FALSE;}
static mrb_int sp_StrStrHash_length(sp_StrStrHash*h){return h->len;}
static void sp_StrStrHash_delete(sp_StrStrHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k)){h->keys[idx]=NULL;h->vals[idx]=NULL;h->len--;mrb_int j=(idx+1)&h->mask;while(h->keys[j]){mrb_int nj=(mrb_int)(sp_str_hash(h->keys[j])&h->mask);if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];h->keys[j]=NULL;h->vals[j]=NULL;idx=j;}j=(j+1)&h->mask;}{mrb_int oi=0;while(oi<=h->len){if(strcmp(h->order[oi],k)==0){while(oi<h->len){h->order[oi]=h->order[oi+1];oi++;}break;}oi++;}}return;}idx=(idx+1)&h->mask;}}
static sp_StrArray*sp_StrStrHash_keys(sp_StrStrHash*h){SP_GC_ROOT(h);sp_StrArray*a=sp_StrArray_new();SP_GC_ROOT(a);for(mrb_int i=0;i<h->len;i++)sp_StrArray_push(a,h->order[i]);return a;}
static sp_StrArray*sp_StrStrHash_values(sp_StrStrHash*h){SP_GC_ROOT(h);sp_StrArray*a=sp_StrArray_new();SP_GC_ROOT(a);for(mrb_int i=0;i<h->len;i++)sp_StrArray_push(a,sp_StrStrHash_get(h,h->order[i]));return a;}
static sp_StrStrHash*sp_StrStrHash_invert(sp_StrStrHash*h){sp_StrStrHash*r=sp_StrStrHash_new();for(mrb_int i=0;i<h->len;i++){const char*k=h->order[i];sp_StrStrHash_set(r,sp_StrStrHash_get(h,k),k);}return r;}
static void sp_StrStrHash_update(sp_StrStrHash*a,sp_StrStrHash*b){for(mrb_int i=0;i<b->len;i++)sp_StrStrHash_set(a,b->order[i],sp_StrStrHash_get(b,b->order[i]));}
static sp_StrStrHash*sp_StrStrHash_dup(sp_StrStrHash*h){sp_StrStrHash*r=sp_StrStrHash_new();r->default_v=h->default_v;for(mrb_int i=0;i<h->len;i++)sp_StrStrHash_set(r,h->order[i],sp_StrStrHash_get(h,h->order[i]));return r;}
static sp_StrStrHash*sp_StrStrHash_replace(sp_StrStrHash*h,sp_StrStrHash*o){if(!h)return h;for(mrb_int i=0;i<h->cap;i++)h->keys[i]=NULL;h->len=0;if(o)for(mrb_int i=0;i<o->len;i++)sp_StrStrHash_set(h,o->order[i],sp_StrStrHash_get(o,o->order[i]));return h;}
static void sp_StrStrHash_clear(sp_StrStrHash*h){if(!h)return;for(mrb_int i=0;i<h->cap;i++)h->keys[i]=NULL;h->len=0;}
static mrb_bool sp_StrStrHash_eq(sp_StrStrHash*a,sp_StrStrHash*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++){const char*k=a->order[i];if(!sp_StrStrHash_has_key(b,k))return FALSE;if(!sp_str_eq(sp_StrStrHash_get(a,k),sp_StrStrHash_get(b,k)))return FALSE;}return TRUE;}

static void sp_IntStrHash_fin(void*p){sp_IntStrHash*h=(sp_IntStrHash*)p;free(h->keys);free(h->vals);free(h->order);free(h->used);}
static void sp_IntStrHash_scan(void*p){sp_IntStrHash*h=(sp_IntStrHash*)p;for(mrb_int i=0;i<h->cap;i++)if(h->used[i])sp_mark_string(h->vals[i]);if(h->default_v)sp_mark_string(h->default_v);}
static sp_IntStrHash*sp_IntStrHash_new(void){sp_IntStrHash*h=(sp_IntStrHash*)sp_gc_alloc(sizeof(sp_IntStrHash),sp_IntStrHash_fin,sp_IntStrHash_scan);h->cap=16;h->mask=15;h->keys=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->vals=(const char**)calloc(h->cap,sizeof(const char*));h->order=(mrb_int*)malloc(sizeof(mrb_int)*h->cap);h->used=(mrb_bool*)calloc(h->cap,sizeof(mrb_bool));h->len=0;h->default_v=NULL;return h;}
static sp_IntStrHash*sp_IntStrHash_new_with_default(const char*d){sp_IntStrHash*h=sp_IntStrHash_new();h->default_v=d;return h;}
static inline mrb_int _sp_istr_idx(mrb_int mask,mrb_int k){return(mrb_int)(((uint64_t)(unsigned long long)k*11400714819323198485ULL)&(uint64_t)mask);}
static void sp_IntStrHash_grow(sp_IntStrHash*h){mrb_int oc=h->cap,ol=h->len;mrb_int*ok=h->keys;const char**ov=h->vals;mrb_bool*ou=h->used;mrb_int*oo=h->order;h->cap*=2;h->mask=h->cap-1;h->keys=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->vals=(const char**)calloc(h->cap,sizeof(const char*));h->order=(mrb_int*)malloc(sizeof(mrb_int)*h->cap);h->used=(mrb_bool*)calloc(h->cap,sizeof(mrb_bool));h->len=ol;for(mrb_int i=0;i<oc;i++){if(!ou[i])continue;mrb_int k=ok[i];const char*v=ov[i];mrb_int di=_sp_istr_idx(h->mask,k);while(h->used[di])di=(di+1)&h->mask;h->used[di]=TRUE;h->keys[di]=k;h->vals[di]=v;}for(mrb_int i=0;i<ol;i++)h->order[i]=oo[i];free(ok);free(ov);free(ou);free(oo);}
static void sp_IntStrHash_set(sp_IntStrHash*h,mrb_int k,const char*v){if(h->len*2>=h->cap)sp_IntStrHash_grow(h);mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->used[idx]=TRUE;h->keys[idx]=k;h->vals[idx]=v;h->order[h->len++]=k;}
static const char*sp_IntStrHash_get(sp_IntStrHash*h,mrb_int k){if(!h)return NULL;mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return h->vals[idx];idx=(idx+1)&h->mask;}return h->default_v;}
static sp_IntStrHash*sp_IntStrHash_merge(sp_IntStrHash*a,sp_IntStrHash*b){sp_IntStrHash*r=sp_IntStrHash_new();if(a){r->default_v=a->default_v;for(mrb_int i=0;i<a->len;i++)sp_IntStrHash_set(r,a->order[i],sp_IntStrHash_get(a,a->order[i]));}if(b){for(mrb_int i=0;i<b->len;i++)sp_IntStrHash_set(r,b->order[i],sp_IntStrHash_get(b,b->order[i]));}return r;}
static mrb_bool sp_IntStrHash_has_key(sp_IntStrHash*h,mrb_int k){mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static mrb_bool sp_IntStrHash_has_value(sp_IntStrHash*h,const char*v){if(!h||!v)return FALSE;for(mrb_int i=0;i<h->len;i++){const char*x=sp_IntStrHash_get(h,h->order[i]);if(x&&strcmp(x,v)==0)return TRUE;}return FALSE;}
static mrb_int sp_IntStrHash_length(sp_IntStrHash*h){return h->len;}
static sp_IntArray*sp_IntStrHash_keys(sp_IntStrHash*h){SP_GC_ROOT(h);sp_IntArray*a=sp_IntArray_new();SP_GC_ROOT(a);for(mrb_int i=0;i<h->len;i++)sp_IntArray_push(a,h->order[i]);return a;}
static sp_StrArray*sp_IntStrHash_values(sp_IntStrHash*h){SP_GC_ROOT(h);sp_StrArray*a=sp_StrArray_new();SP_GC_ROOT(a);for(mrb_int i=0;i<h->len;i++)sp_StrArray_push(a,sp_IntStrHash_get(h,h->order[i]));return a;}
static sp_IntStrHash*sp_IntStrHash_dup(sp_IntStrHash*h){sp_IntStrHash*r=sp_IntStrHash_new();r->default_v=h->default_v;for(mrb_int i=0;i<h->len;i++)sp_IntStrHash_set(r,h->order[i],sp_IntStrHash_get(h,h->order[i]));return r;}
static sp_IntStrHash*sp_IntStrHash_replace(sp_IntStrHash*h,sp_IntStrHash*o){if(!h)return h;for(mrb_int i=0;i<h->cap;i++)h->used[i]=0;h->len=0;if(o)for(mrb_int i=0;i<o->len;i++)sp_IntStrHash_set(h,o->order[i],sp_IntStrHash_get(o,o->order[i]));return h;}
static void sp_IntStrHash_clear(sp_IntStrHash*h){if(!h)return;for(mrb_int i=0;i<h->cap;i++)h->used[i]=0;h->len=0;}
static mrb_bool sp_IntStrHash_eq(sp_IntStrHash*a,sp_IntStrHash*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++){mrb_int k=a->order[i];if(!sp_IntStrHash_has_key(b,k))return FALSE;if(!sp_str_eq(sp_IntStrHash_get(a,k),sp_IntStrHash_get(b,k)))return FALSE;}return TRUE;}

/* Int → Int typed hash. Mirrors sp_IntStrHash's open-addressing
   layout (used[] bitmap so 0/-1 keys are distinguishable from
   empty), with int-valued slots. Used by Array#tally on int
   arrays — see #865. */
static void sp_IntIntHash_fin(void*p){sp_IntIntHash*h=(sp_IntIntHash*)p;free(h->keys);free(h->vals);free(h->order);free(h->used);}
/* default_v is SP_INT_NIL for a hash with no explicit default, so a
   missing-key `[]` read surfaces Ruby nil (#801). Hash.new(N) sets it via
   _new_with_default. */
static sp_IntIntHash*sp_IntIntHash_new(void){sp_IntIntHash*h=(sp_IntIntHash*)sp_gc_alloc(sizeof(sp_IntIntHash),sp_IntIntHash_fin,NULL);h->cap=16;h->mask=15;h->keys=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->vals=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->order=(mrb_int*)malloc(sizeof(mrb_int)*h->cap);h->used=(mrb_bool*)calloc(h->cap,sizeof(mrb_bool));h->len=0;h->default_v=SP_INT_NIL;return h;}
static sp_IntIntHash*sp_IntIntHash_new_with_default(mrb_int d){sp_IntIntHash*h=sp_IntIntHash_new();h->default_v=d;return h;}
static void sp_IntIntHash_grow(sp_IntIntHash*h){mrb_int oc=h->cap,ol=h->len;mrb_int*ok=h->keys;mrb_int*ov=h->vals;mrb_bool*ou=h->used;mrb_int*oo=h->order;h->cap*=2;h->mask=h->cap-1;h->keys=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->vals=(mrb_int*)calloc(h->cap,sizeof(mrb_int));h->order=(mrb_int*)malloc(sizeof(mrb_int)*h->cap);h->used=(mrb_bool*)calloc(h->cap,sizeof(mrb_bool));h->len=ol;for(mrb_int i=0;i<oc;i++){if(!ou[i])continue;mrb_int k=ok[i];mrb_int v=ov[i];mrb_int di=_sp_istr_idx(h->mask,k);while(h->used[di])di=(di+1)&h->mask;h->used[di]=TRUE;h->keys[di]=k;h->vals[di]=v;}for(mrb_int i=0;i<ol;i++)h->order[i]=oo[i];free(ok);free(ov);free(ou);free(oo);}
static void sp_IntIntHash_set(sp_IntIntHash*h,mrb_int k,mrb_int v){if(h->len*2>=h->cap)sp_IntIntHash_grow(h);mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->used[idx]=TRUE;h->keys[idx]=k;h->vals[idx]=v;h->order[h->len++]=k;}
static mrb_int sp_IntIntHash_get(sp_IntIntHash*h,mrb_int k){if(!h)return 0;mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return h->vals[idx];idx=(idx+1)&h->mask;}return h->default_v;}
static sp_IntIntHash*sp_IntIntHash_merge(sp_IntIntHash*a,sp_IntIntHash*b){sp_IntIntHash*r=sp_IntIntHash_new();if(a){r->default_v=a->default_v;for(mrb_int i=0;i<a->len;i++)sp_IntIntHash_set(r,a->order[i],sp_IntIntHash_get(a,a->order[i]));}if(b){for(mrb_int i=0;i<b->len;i++)sp_IntIntHash_set(r,b->order[i],sp_IntIntHash_get(b,b->order[i]));}return r;}
/* Issue #801: maybe-missing public `[]` read. Returns default_v on a miss
   (SP_INT_NIL for a no-default hash = Ruby nil; the explicit default for
   Hash.new(N)). Proven-present reads keep using _get. */
static mrb_int sp_IntIntHash_get_opt(sp_IntIntHash*h,mrb_int k){if(!h)return SP_INT_NIL;mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return h->vals[idx];idx=(idx+1)&h->mask;}return h->default_v;}
static mrb_bool sp_IntIntHash_has_key(sp_IntIntHash*h,mrb_int k){mrb_int idx=_sp_istr_idx(h->mask,k);while(h->used[idx]){if(h->keys[idx]==k)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static mrb_int sp_IntIntHash_length(sp_IntIntHash*h){return h?h->len:0;}
static sp_IntArray*sp_IntIntHash_keys(sp_IntIntHash*h){SP_GC_ROOT(h);sp_IntArray*a=sp_IntArray_new();SP_GC_ROOT(a);if(h)for(mrb_int i=0;i<h->len;i++)sp_IntArray_push(a,h->order[i]);return a;}
static sp_IntArray*sp_IntIntHash_values(sp_IntIntHash*h){SP_GC_ROOT(h);sp_IntArray*a=sp_IntArray_new();SP_GC_ROOT(a);if(h)for(mrb_int i=0;i<h->len;i++)sp_IntArray_push(a,sp_IntIntHash_get(h,h->order[i]));return a;}
static mrb_bool sp_IntIntHash_has_value(sp_IntIntHash*h,mrb_int v){if(!h)return FALSE;for(mrb_int i=0;i<h->len;i++)if(sp_IntIntHash_get(h,h->order[i])==v)return TRUE;return FALSE;}
static mrb_bool sp_IntIntHash_eq(sp_IntIntHash*a,sp_IntIntHash*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++){mrb_int k=a->order[i];if(!sp_IntIntHash_has_key(b,k))return FALSE;if(sp_IntIntHash_get(a,k)!=sp_IntIntHash_get(b,k))return FALSE;}return TRUE;}
static sp_IntIntHash*sp_IntIntHash_dup(sp_IntIntHash*h){sp_IntIntHash*r=sp_IntIntHash_new();r->default_v=h->default_v;for(mrb_int i=0;i<h->len;i++)sp_IntIntHash_set(r,h->order[i],sp_IntIntHash_get(h,h->order[i]));return r;}
static sp_IntIntHash*sp_IntIntHash_replace(sp_IntIntHash*h,sp_IntIntHash*o){if(!h)return h;for(mrb_int i=0;i<h->cap;i++)h->used[i]=0;h->len=0;if(o)for(mrb_int i=0;i<o->len;i++)sp_IntIntHash_set(h,o->order[i],sp_IntIntHash_get(o,o->order[i]));return h;}
static void sp_IntIntHash_clear(sp_IntIntHash*h){if(!h)return;for(mrb_int i=0;i<h->cap;i++)h->used[i]=0;h->len=0;}
/* Array#tally on int_array. CRuby returns an Integer-keyed Hash
   mapping each distinct element to its occurrence count. */
static sp_IntIntHash*sp_IntArray_tally_int(sp_IntArray*a){sp_IntIntHash*h=sp_IntIntHash_new();if(!a)return h;for(mrb_int i=0;i<a->len;i++){mrb_int k=a->data[a->start+i];mrb_int c=sp_IntIntHash_has_key(h,k)?sp_IntIntHash_get(h,k):0;sp_IntIntHash_set(h,k,c+1);}return h;}

/* sp_int_to_s / sp_float_to_s moved to sp_alloc.h (shared so lib/sp_json.c can
   format numbers). String-interpolation of an int slot: a nil sentinel renders
   as the empty string (CRuby interpolates nil as ""), every other value as its
   decimal. */
static const char*sp_int_interp(mrb_int n){return n==SP_INT_NIL?sp_str_empty:sp_int_to_s(n);}
static const char*sp_int_to_s_base(mrb_int n,mrb_int base){if(base<2||base>36)base=10;char*b=sp_str_alloc_raw(72);char tmp[72];int i=0;int neg=0;uint64_t u;if(n<0){neg=1;u=(uint64_t)(-(n+1))+1;}else{u=(uint64_t)n;}if(u==0){tmp[i++]='0';}else{while(u>0){mrb_int d=u%base;tmp[i++]=d<10?'0'+d:'a'+d-10;u/=base;}}int j=0;if(neg)b[j++]='-';while(i>0)b[j++]=tmp[--i];b[j]=0;sp_str_set_len(b,(size_t)j);return b;}
/* sp_float_to_s now lives in sp_alloc.h (shared). Float#inspect is aliased. */
#define sp_float_inspect sp_float_to_s

/* Bound-checked clamp. The arithmetic lives in the compiled-once leaf
   helpers (sp_core.c); the validation rides here because it needs the
   error-message machinery (sp_sprintf) and Ruby float inspect, neither of
   which the decoupled sp_core.c TU links against. CRuby's Comparable#clamp
   compares min<=>max first (so a NaN bound names max), then self against
   each bound (so a NaN receiver names min); a non-NaN min>max is the
   ordinary ArgumentError. */
static inline mrb_int sp_int_clamp_ck(mrb_int v,mrb_int lo,mrb_int hi){
  if(lo>hi)sp_raise_cls("ArgumentError","min argument must be less than or equal to max argument");
  return sp_int_clamp(v,lo,hi);
}
static inline mrb_float sp_float_clamp_ck(mrb_float v,mrb_float lo,mrb_float hi){
  if(lo!=lo||hi!=hi)sp_raise_cls("ArgumentError",sp_sprintf("comparison of Float with %s failed",sp_float_to_s(hi)));
  if(lo>hi)sp_raise_cls("ArgumentError","min argument must be less than or equal to max argument");
  if(v!=v)sp_raise_cls("ArgumentError",sp_sprintf("comparison of Float with %s failed",sp_float_to_s(lo)));
  return sp_float_clamp(v,lo,hi);
}
/* clamp(range): an exclusive range with a real end cannot clamp (CRuby); an
   exclusive ENDLESS range (`1...`, last is the INTPTR_MAX sentinel) can. The
   beginless/endless sentinels satisfy sp_int_clamp_ck's bounds naturally. */
static inline mrb_int sp_int_clamp_range_ck(mrb_int v, sp_Range r) {
  if (r.excl && r.last != INTPTR_MAX)
    sp_raise_cls("ArgumentError", "cannot clamp with an exclusive range");
  /* Reaching here with excl means r.last is the INTPTR_MAX endless sentinel
     (a real exclusive end raised above); pass it through unchanged -- it means
     "no upper bound", which sp_int_clamp_ck handles, not INTPTR_MAX-1. */
  return sp_int_clamp_ck(v, r.first, r.last);
}
/* `:name`, or `:"name"` when the name needs quoting -- shares the
   name-string predicates in lib/sp_str.c with the hash-key short form. */
static const char *sp_sym_inspect(sp_sym id) { if (id == (sp_sym)-1) return "nil"; /* nilable-symbol sentinel */ return sp_sym_inspect_name(sp_sym_to_s(id)); }
static const char*sp_gets(void){char buf[4096];if(!fgets(buf,sizeof(buf),stdin))return NULL;size_t l=strlen(buf);char*r=sp_str_alloc_raw(l+1);memcpy(r,buf,l+1);return r;}
static sp_StrArray*sp_readlines(void){sp_StrArray*a=sp_StrArray_new();char buf[4096];while(fgets(buf,sizeof(buf),stdin)){size_t l=strlen(buf);char*r=sp_str_alloc_raw(l+1);memcpy(r,buf,l+1);sp_StrArray_push(a,r);}return a;}
const char*sp_sprintf(const char*fmt,...){char _sp_tmp[4096];va_list ap;va_start(ap,fmt);int _sp_n=vsnprintf(_sp_tmp,sizeof(_sp_tmp),fmt,ap);va_end(ap);if(_sp_n<0)_sp_n=0;char*b=sp_str_alloc((size_t)_sp_n);if(_sp_n<(int)sizeof(_sp_tmp)){memcpy(b,_sp_tmp,(size_t)_sp_n);}else{/* result didn't fit the stack temp; re-render at full width (sp_str_alloc gives _sp_n bytes + NUL) so long string interpolations aren't truncated. re-arm the va_list rather than va_copy so the common fast path pays nothing */va_start(ap,fmt);vsnprintf(b,(size_t)_sp_n+1,fmt,ap);va_end(ap);}return b;}
/* Use a temp pointer for realloc so the original buffer is not leaked
   on allocation failure. Match the perror+exit pattern used elsewhere
   (see sp_IntArray_replace) instead of returning a partial result. */

/* String#count with multiple args: intersection of charsets.
   Each additional arg further restricts which chars to count.
   sp_str_count_n(s, args[], n) computes the intersection. */
/* Issue #800: clamp l*n so a malicious input can't allocate a tiny
   buffer through size_t overflow. */
/* Issue #836: bound the multiplier so a wildly oversized request
   raises ArgumentError rather than segfaulting when malloc returns
   NULL and memcpy walks it. 1 GiB cap covers realistic use. */
/* Issue #903: String#codepoints -- one IntArray entry per UTF-8
   codepoint (not byte). Replacement-character behaviour mirrors
   sp_utf8_decode (returns the leading byte for malformed seqs). */
/* Issue #902: String#tr_s -- translate AND squeeze consecutive
   identical results into one. Walks codepoint-by-codepoint and
   collapses adjacent duplicates only among the translated bytes
   (untranslated runs keep their original characters). */
/* Build into a malloc temp and read all of `s` BEFORE the result sp_str_alloc:
   that allocation can now trigger a string-heap collection, which would sweep
   an unrooted `s` mid-copy (the read-first pattern sp_str_tr/sp_str_format use). */
/* Issue #921: shrink the heap-string header length to match the
   squeezed payload — the alloc gives bl+1 bytes, the squeezed
   write fills n<=bl, leaving the header's stored length stale.
   `bytes` / `length` consult the header (not strlen), so callers
   would see the alloc size and trailing NULs. */
/* String#squeeze(chars) — only squeeze chars listed in the charset
   (same charset syntax as tr: a-z, ^x, etc.). Consecutive runs of
   non-listed chars pass through untouched. */
/* Multi-arg delete/squeeze: delete (or squeeze runs of) characters that
   are in the INTERSECTION of all n charset args, mirroring
   sp_str_count_n. Each arg is a charset spec (^negation, a-z ranges). */

/* Forward decl from sp_crypto.h (libspinel_rt.a). Used by
   sp_str_crypt below to provide a deterministic crypt-like
   hash without dragging in libc crypt(3). */
const char *sp_crypto_hmac_sha256_b64url(const char *key, const char *msg);

/* String#crypt — Spinel's crypt is NOT the libc DES crypt. It
   returns `salt[0..1] || hmac_sha256(salt, password)[0..10]` as
   a 13-char string (same length as DES crypt, deterministic,
   stronger primitive). CRuby's spec says String#crypt is impl-
   defined and "should not be used for security"; this matches
   that contract while keeping outputs reproducible across
   spinel builds. Short salts get padded with '.' so the result
   still has the canonical first-2-chars-are-salt shape. */

/* String#scrub — walk the bytes; for each valid UTF-8 lead +
   continuation sequence, copy through. For invalid bytes (lone
   continuation, truncated multi-byte, overlong, etc.), emit the
   replacement string and skip one byte. NULL replacement uses
   U+FFFD (3 UTF-8 bytes: EF BF BD), matching CRuby. */

/* String#setbyte: mutate s[i] = v in place. Spinel adopts
   `# frozen_string_literal: true` semantics globally — all
   string literals are frozen, mutation requires a heap-allocated
   buffer (e.g. via .dup or string concatenation). The runtime
   marker byte at s[-1] tells us the provenance:
     0xfe / 0xfc -> sp_str_alloc heap (writable, GC unmarked / marked)
     0xfd        -> sp_String wrapper buffer (writable)
     0xff        -> rodata literal (frozen -> FrozenError)
     other       -> FFI / unknown provenance, treated as frozen
   Returns the byte value (CRuby setbyte return). */
static inline mrb_int sp_str_getbyte(const char *s, mrb_int i) {
  if (!s) sp_nil_recv("getbyte");
  mrb_int bl = (mrb_int)sp_str_byte_len(s);
  if (i < 0) i += bl;
  if (i < 0 || i >= bl) return 0;
  return (mrb_int)(unsigned char)s[i];
}
/* String#getbyte: a negative index counts from the end, and an out-of-range
   index is nil (SP_INT_NIL, a nullable int) -- not 0 or an adjacent byte. */
static inline mrb_int sp_str_getbyte_opt(const char *s, mrb_int i) {
  if (!s) sp_nil_recv("getbyte");
  mrb_int bl = (mrb_int)sp_str_byte_len(s);
  if (i < 0) i += bl;
  if (i < 0 || i >= bl) return SP_INT_NIL;
  return (mrb_int)(unsigned char)s[i];
}

static inline mrb_int sp_str_setbyte(const char *s, mrb_int i, mrb_int v) {
  if (!s) sp_nil_recv("setbyte");
  unsigned char m = ((const unsigned char *)s)[-1];
  if (m == 0xfe || m == 0xfc) {
    (((sp_str_hdr *)(s - 1)) - 1)->hash = 0;  /* invalidate cached key hash */
    ((char *)s)[i] = (char)v;
    return v;
  }
  if (m == 0xfd) {
    ((char *)s)[i] = (char)v;
    return v;
  }
  sp_raise_frozen_str(s);
  return v;
}

/* 0xf1 = heap string frozen by an explicit .freeze call.
   Unlike 0xff (rodata literal) this marker lives in a malloc'd buffer
   so sp_str_freeze_val can set it.  The frozen? predicate and mutation
   guards check for 0xf1; plain rodata 0xff literals are NOT reported
   as frozen (they behave as immutable value-semantics objects). */
static inline mrb_bool sp_str_is_frozen_val(const char *s) {
  if (!s) return TRUE;
  return ((const unsigned char *)s)[-1] == 0xf1;
}
static inline const char *sp_str_freeze_val(const char *s) {
  if (!s) return s;
  unsigned char m = ((const unsigned char *)s)[-1];
  if (m == 0xfe || m == 0xfc) {
    ((unsigned char *)s)[-1] = 0xf1;
    return s;
  }
  if (m == 0xff || m == 0xf1 || m != 0xfd) {
    /* rodata literal or already frozen: copy to heap and freeze */
    if (m == 0xf1) return s;  /* already heap-frozen */
    size_t n = strlen(s);
    char *r = sp_str_alloc(n);
    memcpy(r, s, n);
    ((unsigned char *)r)[-1] = 0xf1;
    return r;
  }
  return s;
}
/* String#clone: a copy that preserves the frozen state, unlike #dup which always
   returns an unfrozen copy (CRuby semantics). Carries the 0xf1 heap-frozen marker
   across to the fresh buffer. */
static inline const char *sp_str_clone_val(const char *s) {
  if (!s) return NULL;
  const char *r = sp_str_dup(s);  /* byte_len-aware: clone carries embedded NULs */
  if (r && sp_str_is_frozen_val(s)) ((unsigned char *)r)[-1] = 0xf1;
  return r;
}
static inline void sp_str_check_mutable(const char *s) {
  if (sp_str_is_frozen_val(s)) sp_raise_frozen_str(s);
}

/* sp_String (mutable-String builder) moved to sp_string.h / lib/sp_string.c:
   the hot construction/append core is inline in the header, the cold in-place
   mutators (prepend/insert/replace/dup) compile once in the archive. */

/* `File.open(path, mode)` without a block returns an sp_File * — a
   GC-managed wrapper around `FILE *fp`. The finalizer fclose()s any
   still-open fp so a dropped file handle doesn't leak. `path` and
   `mode` are kept live for `f.path` / `f.mode` introspection; they
   come from the caller's already-live string slot so no extra mark
   is needed. */
/* File / IO handle ops live in libspinel_rt.a (lib/sp_io.c); the sp_File
   struct and the allocation-free op prototypes come from sp_io.h. The
   string-returning readers below (gets / read / read_n / path) stay
   inline here because they allocate via the hot static sp_str_alloc,
   whose per-TU sp_str_heap can't be shared across translation units. */
#include "sp_io.h"
static inline const char *sp_File_gets(sp_File *f) {
  if (!f || !f->fp) return NULL;
  char buf[65536];
  if (!fgets(buf, (int)sizeof(buf), f->fp)) return NULL;
  size_t n = strlen(buf);
  char *r = sp_str_alloc(n);
  memcpy(r, buf, n);
  return r;
}
/* Read a line into a caller-provided buffer without allocating. The returned
   pointer is `buf` (or NULL at EOF); valid only until the next call. Used by
   each_line loops where the line does not escape the loop body. */
static inline const char *sp_File_gets_buf(sp_File *f, char *buf, int size) {
  if (!f || !f->fp) return NULL;
  if (!fgets(buf, size, f->fp)) return NULL;
  return buf;
}
/* Like gets_buf, but into a reusable HEAP line string: the buffer carries a
   real string header (marker + length), so it is a first-class spinel string
   -- runtime helpers may read its marker or root it safely. Every string
   crossing the runtime API must be spinel-marked (or a 0xff literal); a raw
   stack buffer whose [-1] byte is arbitrary memory breaks the GC mark. */
static inline const char *sp_File_gets_into(sp_File *f, char *s, int cap) {
  if (!f || !f->fp) return NULL;
  if (!fgets(s, cap, f->fp)) return NULL;
  sp_str_set_len(s, strlen(s));
  return s;
}
static inline const char *sp_File_read(sp_File *f) {
  if (!f || !f->fp) return sp_str_empty;
  long pos = ftell(f->fp);
  if (pos >= 0 && fseek(f->fp, 0, SEEK_END) == 0) {
    long end = ftell(f->fp);
    fseek(f->fp, pos, SEEK_SET);
    long n = (end > pos) ? (end - pos) : 0;
    if (n <= 0) return sp_str_empty;
    char *r = sp_str_alloc((size_t)n);
    size_t got = fread(r, 1, (size_t)n, f->fp);
    r[got] = 0;
    return r;
  }
  /* Non-seekable stream (IO.pipe end, socket): read to EOF in chunks. */
  size_t cap = 256, len = 0;
  char *buf = (char *)malloc(cap);
  if (!buf) return sp_str_empty;
  for (;;) {
    if (len + 4096 + 1 > cap) {
      cap = (len + 4096 + 1) * 2;
      char *nb = (char *)realloc(buf, cap);
      if (!nb) { free(buf); return sp_str_empty; }
      buf = nb;
    }
    size_t got = fread(buf + len, 1, 4096, f->fp);
    len += got;
    if (got < 4096) break;
  }
  char *r = sp_str_alloc(len);
  memcpy(r, buf, len);
  r[len] = 0;
  free(buf);
  return r;
}
/* IO#read(n): read up to n bytes from the current position. Returns NULL
   (nil) at EOF for a positive n, "" for n == 0, and the whole rest for a
   negative n (treated as the no-count read). A short read produces a
   string of the bytes actually read. */
static inline const char *sp_File_read_n(sp_File *f, mrb_int n) {
  if (!f || !f->fp) return NULL;
  if (n < 0) return sp_File_read(f);
  if (n == 0) return sp_str_empty;
  char *r = sp_str_alloc((size_t)n);
  size_t got = fread(r, 1, (size_t)n, f->fp);
  if (got == 0) return NULL;
  if ((mrb_int)got == n) { r[got] = 0; return r; }
  char *s = sp_str_alloc(got);
  memcpy(s, r, got);
  s[got] = 0;
  return s;
}
static inline const char *sp_File_path(sp_File *f) { return f && f->path ? f->path : sp_str_empty; }
static const char *sp_file_join(const char **parts, int n) {
  size_t total = 0;
  for (int i = 0; i < n; i++) {
    if (parts[i]) total += strlen(parts[i]);
    if (i < n - 1) total++;
  }
  char *r = sp_str_alloc((mrb_int)total);
  size_t off = 0;
  for (int i = 0; i < n; i++) {
    if (parts[i]) { size_t l = strlen(parts[i]); memcpy(r + off, parts[i], l); off += l; }
    if (i < n - 1) r[off++] = '/';
  }
  r[off] = 0;
  return r;
}
static inline sp_StrArray *sp_File_readlines(sp_File *f) {
  sp_StrArray *a = sp_StrArray_new();
  const char *line;
  while ((line = sp_File_gets(f)) != NULL) sp_StrArray_push(a, line);
  return a;
}
static sp_StrArray *sp_file_readlines(const char *path) {
  sp_StrArray *a = sp_StrArray_new();
  FILE *_fp = fopen(path ? path : "", "r");
  if (!_fp) return a;
  char _buf[4096];
  while (fgets(_buf, (int)sizeof(_buf), _fp)) {
    size_t _l = strlen(_buf);
    char *_r = sp_str_alloc_raw(_l + 1);
    memcpy(_r, _buf, _l + 1);
    sp_StrArray_push(a, _r);
  }
  fclose(_fp);
  return a;
}
static sp_StrArray *sp_file_readlines_chomp(const char *path) {
  sp_StrArray *a = sp_StrArray_new();
  FILE *_fp = fopen(path ? path : "", "r");
  if (!_fp) return a;
  char _buf[4096];
  while (fgets(_buf, (int)sizeof(_buf), _fp)) {
    size_t _l = strlen(_buf);
    if (_l > 0 && _buf[_l-1] == '\n') { _buf[--_l] = '\0'; }
    if (_l > 0 && _buf[_l-1] == '\r') { _buf[--_l] = '\0'; }
    char *_r = sp_str_alloc_raw(_l + 1);
    memcpy(_r, _buf, _l + 1);
    sp_StrArray_push(a, _r);
  }
  fclose(_fp);
  return a;
}

/* Array#inspect for each typed array: `[elem1, elem2, ...]` with each
   element rendered via its own primitive inspect. Matches CRuby's
   Array#inspect output byte-for-byte. Returns a GC-managed C string. */
/* The inspect helpers NULL-guard their receiver: a NULL array can
   reach here when an unresolved call (e.g. an unsupported Array method)
   emits a 0 placeholder that flows into `.inspect`, and dereferencing
   a->len would segfault. Rendering "[]" stops the crash and degrades to
   the same shape as the empty-array case. */
/* Array#join for float arrays -- each element via the Ruby-faithful
   sp_float_to_s ("1.0", not "1"). Mirrors sp_IntArray_join exactly: build in a
   malloc buffer, return an sp_str_alloc'd copy. (Not sp_String#data, whose owner
   isn't GC-rooted across the return.) sp_float_to_s's result is copied
   immediately, before the next call can reuse its buffer. */
static mrb_bool sp_StrArray_eq(sp_StrArray*a,sp_StrArray*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++)if(!sp_str_eq(a->data[i],b->data[i]))return FALSE;return TRUE;}
/* Symbol arrays share the IntArray representation (sp_sym = mrb_int),
   but each element is rendered as ":name" via sp_sym_to_s. */
static inline const char*sp_SymArray_inspect(sp_IntArray*a){return a?sp_inspect_container(sp_box_obj(a,SP_BUILTIN_SYM_ARRAY)):"[]";}
/* PtrArray elements are object pointers without a per-element class
   tag, so we render them as `#<Object>` rather than recursing. */
/* Issue #851: Hash#inspect for typed-hash variants beyond
   sym_int_hash. Renders Ruby's `{"k" => v, ...}` (string keys),
   `{42 => "v", ...}` (int keys), or `{:k => v, ...}` (sym keys but
   non-int value, since the bare `k: v` shorthand only applies
   when values are inspectable as one-liners — match CRuby). */
static const char*sp_StrIntHash_inspect(sp_StrIntHash*h){return h?sp_inspect_container(sp_box_obj(h,SP_BUILTIN_STR_INT_HASH)):"{}";}
/* Hash#to_proc lookup fn — cap is the hash, args[0] the string key. */
static mrb_int sp_StrIntHash_proc_fn(void *cap, mrb_int argc, mrb_int *args) { if (argc < 1) return 0; return sp_StrIntHash_get((sp_StrIntHash *)cap, (const char *)(uintptr_t)args[0]); }
static const char*sp_StrStrHash_inspect(sp_StrStrHash*h){return h?sp_inspect_container(sp_box_obj(h,SP_BUILTIN_STR_STR_HASH)):"{}";}
static const char*sp_IntStrHash_inspect(sp_IntStrHash*h){return h?sp_inspect_container(sp_box_obj(h,SP_BUILTIN_INT_STR_HASH)):"{}";}
static const char*sp_IntIntHash_inspect(sp_IntIntHash*h){SP_GC_ROOT(h);sp_String*s=sp_String_new("{");SP_GC_ROOT(s);if(h){for(mrb_int i=0;i<h->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_int_to_s(h->order[i]));sp_String_append(s," => ");sp_String_append(s,sp_int_to_s(sp_IntIntHash_get(h,h->order[i])));}}sp_String_append(s,"}");return s->data;}
/* Nested-array inspect: when codegen knows the ptr_array's element
   type is one of the four built-in T_array shapes, recurse into the
   matching primitive inspect . */
static const char*sp_IntArrayPtrArray_inspect(sp_PtrArray*a){SP_GC_ROOT(a);sp_String*s=sp_String_new("[");SP_GC_ROOT(s);for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_IntArray_inspect((sp_IntArray*)a->data[i]));}sp_String_append(s,"]");return s->data;}
/* Issue #742: Array#combination(k) on int_array -- emit all
   k-element ordered combinations as a PtrArray of IntArrays. */
static void sp_int_combination_recur(sp_IntArray*src,mrb_int start,mrb_int k,sp_IntArray*acc,sp_PtrArray*out){if(k==0){sp_IntArray*cp=sp_IntArray_new();for(mrb_int i=0;i<acc->len;i++)sp_IntArray_push(cp,acc->data[acc->start+i]);sp_PtrArray_push(out,cp);return;}for(mrb_int i=start;i<=src->len-k;i++){sp_IntArray_push(acc,src->data[src->start+i]);sp_int_combination_recur(src,i+1,k-1,acc,out);acc->len--;}}
static sp_PtrArray*sp_IntArray_combination(sp_IntArray*a,mrb_int k){sp_PtrArray*out=sp_PtrArray_new();if(!a||k<0||k>a->len)return out;sp_IntArray*acc=sp_IntArray_new();sp_int_combination_recur(a,0,k,acc,out);return out;}
/* repeated_combination: like combination but an index may repeat, so the
   recursion restarts at `i` rather than `i+1`. */
static void sp_int_repeated_combination_recur(sp_IntArray*src,mrb_int start,mrb_int k,sp_IntArray*acc,sp_PtrArray*out){if(k==0){sp_IntArray*cp=sp_IntArray_new();for(mrb_int i=0;i<acc->len;i++)sp_IntArray_push(cp,acc->data[acc->start+i]);sp_PtrArray_push(out,cp);return;}for(mrb_int i=start;i<src->len;i++){sp_IntArray_push(acc,src->data[src->start+i]);sp_int_repeated_combination_recur(src,i,k-1,acc,out);acc->len--;}}
static sp_PtrArray*sp_IntArray_repeated_combination(sp_IntArray*a,mrb_int k){sp_PtrArray*out=sp_PtrArray_new();if(!a||k<0)return out;sp_IntArray*acc=sp_IntArray_new();sp_int_repeated_combination_recur(a,0,k,acc,out);return out;}

/* Cartesian product of two int arrays. Returns a PtrArray of
   2-element IntArrays. */
/* Array#permutation(k) -- ordered k-permutations. */
static void sp_int_permutation_recur(sp_IntArray*src,mrb_int k,sp_IntArray*used,sp_IntArray*acc,sp_PtrArray*out){if(k==0){sp_IntArray*cp=sp_IntArray_new();for(mrb_int i=0;i<acc->len;i++)sp_IntArray_push(cp,acc->data[acc->start+i]);sp_PtrArray_push(out,cp);return;}for(mrb_int i=0;i<src->len;i++){if(used->data[used->start+i])continue;used->data[used->start+i]=1;sp_IntArray_push(acc,src->data[src->start+i]);sp_int_permutation_recur(src,k-1,used,acc,out);acc->len--;used->data[used->start+i]=0;}}
static sp_PtrArray*sp_IntArray_permutation(sp_IntArray*a,mrb_int k){SP_GC_ROOT(a);sp_PtrArray*out=sp_PtrArray_new();SP_GC_ROOT(out);if(!a||k<0||k>a->len)return out;sp_IntArray*used=sp_IntArray_new();SP_GC_ROOT(used);for(mrb_int i=0;i<a->len;i++)sp_IntArray_push(used,0);sp_IntArray*acc=sp_IntArray_new();SP_GC_ROOT(acc);sp_int_permutation_recur(a,k,used,acc,out);return out;}
static const char*sp_FloatArrayPtrArray_inspect(sp_PtrArray*a){SP_GC_ROOT(a);sp_String*s=sp_String_new("[");SP_GC_ROOT(s);for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_FloatArray_inspect((sp_FloatArray*)a->data[i]));}sp_String_append(s,"]");return s->data;}
static const char*sp_StrArrayPtrArray_inspect(sp_PtrArray*a){SP_GC_ROOT(a);sp_String*s=sp_String_new("[");SP_GC_ROOT(s);for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_StrArray_inspect((sp_StrArray*)a->data[i]));}sp_String_append(s,"]");return s->data;}
/* sp_PolyArrayPtrArray_inspect lives below sp_PolyArray_inspect's
   forward declaration (sp_PolyArray isn't defined until ~2542). */
static const char*sp_SymArrayPtrArray_inspect(sp_PtrArray*a){SP_GC_ROOT(a);sp_String*s=sp_String_new("[");SP_GC_ROOT(s);for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_SymArray_inspect((sp_IntArray*)a->data[i]));}sp_String_append(s,"]");return s->data;}
/* issue #526: join for a sp_PtrArray of sp_String* (mutable_str_ptr_array).
   Sibling to sp_StrArray_join, but takes advantage of sp_String's known
   length: two-pass — sum the exact total, sp_str_alloc once, then memcpy
   each element by its s->len (preserves embedded NULs). Avoids the
   realloc-grow loop's leak-on-failure and long-separator overflow
   risks, and skips an intermediate malloc'd buffer. NULL entries
   contribute zero length. */

#ifdef __FreeBSD__

#define re_exec spinel_re_exec

#define MAP_NORESERVE 0

#endif

/* Regexp engine (link with libspre.a from lib/regexp/) */

/* Regexp globals: $1-$9 captures */
/* NULL (not "") so sp_mark_string's null-guard handles the unset case
   without reaching the `s[-1]` access. The rodata `""` literal would
   trigger -Wstringop-overflow under -O3 + sp_mark_string inlining at
   the call site in sp_re_mark_globals — gcc proves the `s[-1] = 0xfc`
   write would be out-of-bounds even though the runtime guard
   `s[-1] == 0xfe` (always false for rodata) prevents it from firing. */

/* Symbolic back-references populated alongside the numbered captures.
   Read by codegen's BackReferenceReadNode arm:
     $&  -> sp_re_match_str (the whole matched substring)
     $`  -> sp_re_match_pre  (substring before the match)
     $'  -> sp_re_match_post (substring after the match)
   $~ falls back to $& since Spinel has no MatchData wrapper. */

/* ARGV runtime: argv[i] strings are dup'd via sp_str_dup_external on
   main() entry, which allocates from the str-heap with mark byte
   0xfe. Without explicit marking they get reaped on the first
   sp_str_sweep, leaving sp_argv.data[i] as a dangling pointer that
   later `ARGV[i]` reads dereference. The exact length boundary
   triggering the segfault depends on malloc's reuse pattern (so the
   bug surfaces non-deterministically by string length), but the
   underlying issue is unconditional. */
typedef struct{const char**data;mrb_int len;}sp_Argv;
static sp_Argv sp_argv;
static const char *sp_program_name = "";
static sp_StrArray *sp_argv_array_cache = NULL;
static sp_StrArray *sp_get_ARGV(void) {
  if (!sp_argv_array_cache) {
    sp_argv_array_cache = sp_StrArray_new();
    for (mrb_int i = 0; i < sp_argv.len; i++) sp_StrArray_push(sp_argv_array_cache, sp_argv.data[i]);
  }
  return sp_argv_array_cache;
}

/* ARGF: a pseudo-IO that reads the files named in ARGV in sequence, or stdin
   when ARGV is empty (a `-` filename also means stdin). The state is a single
   global; the ARGF constant is a marker pointer to it. */
typedef struct { mrb_int idx; FILE *cur; int started; const char *fname; } sp_Argf;
static sp_Argf sp_argf_obj = {0, NULL, 0, NULL};
/* Ensure a current readable stream, or return 0 at total end of input. */
static int sp_argf_ensure(void) {
  if (sp_argf_obj.cur) return 1;
  if (sp_argv.len == 0) {
    if (sp_argf_obj.started) return 0;
    sp_argf_obj.started = 1; sp_argf_obj.cur = stdin; sp_argf_obj.fname = "-"; return 1;
  }
  while (sp_argf_obj.idx < sp_argv.len) {
    const char *fn = sp_argv.data[sp_argf_obj.idx++];
    sp_argf_obj.started = 1;
    if (fn && fn[0] == '-' && fn[1] == 0) { sp_argf_obj.cur = stdin; sp_argf_obj.fname = "-"; return 1; }
    FILE *f = fn ? fopen(fn, "r") : NULL;
    if (f) { sp_argf_obj.cur = f; sp_argf_obj.fname = fn; return 1; }
  }
  return 0;
}
static const char *sp_argf_gets(void) {
  for (;;) {
    if (!sp_argf_ensure()) return NULL;
    char buf[8192];
    if (fgets(buf, sizeof buf, sp_argf_obj.cur)) {
      size_t l = strlen(buf); char *r = sp_str_alloc_raw(l + 1); memcpy(r, buf, l + 1); return r;
    }
    if (sp_argf_obj.cur && sp_argf_obj.cur != stdin) fclose(sp_argf_obj.cur);
    sp_argf_obj.cur = NULL;  /* EOF on this stream; advance on next ensure */
  }
}
static const char *sp_argf_read(void) {
  sp_String *s = sp_String_new(""); SP_GC_ROOT(s);
  const char *line;
  while ((line = sp_argf_gets())) sp_String_append(s, line);
  return s->data;
}
static sp_StrArray *sp_argf_readlines(void) {
  sp_StrArray *a = sp_StrArray_new(); SP_GC_ROOT(a);
  const char *line;
  while ((line = sp_argf_gets())) sp_StrArray_push(a, line);
  return a;
}
static const char *sp_argf_filename(void) {
  if (sp_argf_obj.fname) return sp_argf_obj.fname;
  return sp_argv.len > 0 ? sp_argv.data[0] : "-";
}
static mrb_bool sp_argf_eof(void) { return !sp_argf_ensure(); }

/* Mark active in-flight exception messages. Most raises pass string
   literals (rodata, marker byte ≠ 0xfe → no-op for sp_mark_string),
   but raises that build messages dynamically via sp_sprintf (e.g.
   sp_str_to_i_strict's `"invalid value for Integer(): \"%s\""`)
   hand a heap-allocated string to sp_raise_cls. Without marking, a
   GC cycle between the raise and the rescue handler reading the
   message would sweep the message and leave sp_exc_msg[i] dangling.
   The helper itself is defined further down (after sp_exc_msg /
   sp_exc_top are declared); only the prototype lives here. */
static void sp_mark_in_flight_exceptions(void);
/* Mark the current fiber's in-flight proc-return values; defined with the
   proc-return machinery (sp_proc_home) further down. */
static void sp_mark_proc_homes(void);
/* Mark in-flight valued-break values; defined with the sp_brk machinery. */
static void sp_mark_brk_vals(void);

/* Mark the regex globals as live during GC. Each holds a pointer to a
   string allocated via sp_str_alloc_raw on the str-heap; without this
   sp_str_sweep would reap them on the next collect, leaving dangling
   pointers in $1..$9, $&, $`, $'. sp_mark_string is null-safe and
   no-ops on non-heap strings (the empty-string default of
   sp_re_last_str), so it's safe to call unconditionally. */
/* sp_fiber_root.storage needs marking here because sp_fiber_root is
   a static (not sp_gc_alloc'd), so its normal sp_Fiber_scan never
   runs via the heap walker. Without this, top-level `Fiber[:k] = v`
   writes get prematurely collected. The forward declaration is
   needed because sp_fiber_root is defined further down in the
   Fiber runtime block. */
/* External linkage: lib/sp_gc.c's sp_gc_mark_all reaches this by name. */
static void sp_re_mark_globals(void) {
  sp_mark_string(sp_re_last_str);
  for (int i = 0; i < 10; i++) sp_mark_string(sp_re_captures[i]);
  sp_mark_string(sp_re_match_str);
  sp_mark_string(sp_re_match_pre);
  sp_mark_string(sp_re_match_post);
  for (mrb_int i = 0; i < sp_argv.len; i++) sp_mark_string(sp_argv.data[i]);
  if (sp_argv_array_cache) sp_gc_mark(sp_argv_array_cache);
  sp_mark_in_flight_exceptions();
  sp_mark_proc_homes();
  sp_mark_brk_vals();
  sp_mark_fiber_root_storage();
}

/* Hand the collector (lib/sp_gc.c) this TU's root-marking and string-heap
   sweep. Runs before main, so the hooks are set before the first
   allocation can trigger a collection. */
__attribute__((constructor)) static void sp_gc_install_tu_hooks(void) {
  sp_gc_mark_globals_hook = sp_re_mark_globals;
  /* sp_gc_str_sweep_hook is installed by sp_alloc.c's constructor. */
}

/* `$+` / `$LAST_PAREN_MATCH` — contents of the highest-indexed group
   that participated in the match. Walks sp_re_captures[] from 9 down
   and returns the first non-NULL entry. NULL when no group matched
   (codegen ternary falls back to ""). Matches CRuby's behaviour:
   for /(a)(b)?/ matching "a", $+ is "a"; for /(a)(b)/ matching "ab",
   $+ is "b". */


/* `=~` returns the match position (0-indexed) or -1 on miss.
   Codegen's regex truthy check (regex_match_call_node? arm in
   compile_cond_expr) compares against -1 so match-at-position-0
   is correctly truthy. Direct value use lines up with CRuby's
   `String#=~` int semantics: "abc" =~ /b/ -> 1, not 2. */

/* `s.rindex(regex)` — last match start, in BYTE offset (matches
   the way sp_str_rindex reports indices for plain-string search;
   codepoint translation would require a UTF-8 walk and the
   handful of call sites that consume rindex don't need it).
   Walks forward through successive matches and remembers the
   latest start. Issue #504: previously the codegen routed
   `s.rindex(/re/)` to sp_str_rindex(s, 0) and SEGV'd at
   strlen(NULL). Returns -1 on no match. */

/* `s.rpartition(regex)` -> [before, last_match, after]. On no match Ruby
   returns ["", "", s] (the whole string lands in the last slot). Walks
   forward to the final match span, mirroring sp_re_rindex. */


/* Issue #869: Regexp#match?(str, pos) starts matching at byte
   offset `pos`. Negative pos counts from the end (CRuby compat).
   Out-of-range pos returns false. */
/* String#match?(/re/, pos) — pos is a codepoint index (CRuby semantics),
   unlike Regexp#match?(str, pos) which uses byte offset. Convert the
   codepoint index to a byte offset before dispatching to re_exec. */
static mrb_bool sp_str_re_match_p_at(mrb_regexp_pattern *pat, const char *str, mrb_int cpos) {
  mrb_int cl = sp_str_length(str);
  if (cpos < 0) cpos += cl;
  if (cpos < 0 || cpos > cl) return FALSE;
  size_t boff = sp_utf8_byte_offset(str, cpos);
  int64_t slen = (int64_t)strlen(str);
  int caps[2];
  return re_exec(pat, str, slen, (mrb_int)boff, caps, 2) > 0;
}

/* Issue #855: expand `\1`..`\9` / `\&` / `\0` backreferences in
   the replacement string against the current caps[] array. `\\`
   is a literal backslash. Writes to *out_io at *olen_io, growing
   *out_io / *cap_io as needed. */


/* String#gsub(regex, hash) — per-match hash lookup form. CRuby's
 * semantics: each matched substring is looked up as a key in the
 * hash; the value (if present) is the replacement, otherwise the
 * matched substring is dropped (CRuby returns "", not the match).
 * Used by html_escape / json_escape idioms (gsub(/[&<>]/, ESCAPES)). */
static const char *sp_re_gsub_str_str_hash(mrb_regexp_pattern *pat, const char *str, sp_StrStrHash *h) {
  int64_t slen = (int64_t)strlen(str);
 /* malloc scratch (realloc-safe); exact-sized string emitted below. */
  size_t cap = (slen * 2) + 64; char *out = (char *)malloc(cap); size_t olen = 0;
  int64_t pos = 0; int caps[64];
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 64);
    if (n <= 0 || caps[0] < 0) break;
    size_t before = caps[0] - pos;
    int mlen = caps[1] - caps[0];
    /* Lay a 0xff (rodata-literal) marker byte right before the transient key
       so sp_str_hash's s[-1] read is in-bounds and routes to the plain
       (non-caching) path -- this buffer has no sp_str_hdr to cache into. */
    char keybuf[65]; keybuf[0] = (char)0xff;
    char *kbuf = mlen + 1 < (int)sizeof(keybuf) ? keybuf : (char *)malloc(mlen + 2);
    if (kbuf != keybuf) kbuf[0] = (char)0xff;
    char *key = kbuf + 1;
    memcpy(key, str + caps[0], mlen);
    key[mlen] = 0;
    const char *rep = sp_StrStrHash_has_key(h, key) ? sp_StrStrHash_get(h, key) : "";
    size_t rlen = strlen(rep);
    if (olen + before + rlen >= cap) { cap = ((olen + before + rlen) * 2) + 64; out = (char *)realloc(out, cap); }
    memcpy(out + olen, str + pos, before); olen += before;
    memcpy(out + olen, rep, rlen); olen += rlen;
    if (kbuf != keybuf) free(kbuf);
    if (caps[0] == caps[1]) {
 /* Zero-width match: keep the source char at this position and step
    past it (see sp_re_gsub for the rationale). */
      if (caps[1] < slen) {
        if (olen + 1 >= cap) { cap = (olen * 2) + 64; out = (char *)realloc(out, cap); }
        out[olen++] = str[caps[1]];
      }
      pos = caps[1] + 1;
    }
else {
      pos = caps[1];
    }
  }
  if (pos < slen) {
    size_t rest = slen - pos;
    if (olen + rest >= cap) { cap = olen + rest + 1; out = (char *)realloc(out, cap); }
    memcpy(out + olen, str + pos, rest); olen += rest;
  }
  char *res = sp_str_alloc(olen);
  memcpy(res, out, olen);
  free(out);
  return res;
}

/* Issue #910: sub(regex, hash) — same lookup semantics as
   sp_re_gsub_str_str_hash but only the first match. */
static const char *sp_re_sub_str_str_hash(mrb_regexp_pattern *pat, const char *str, sp_StrStrHash *h) {
  int64_t slen = (int64_t)strlen(str);
  int caps[64];
  int n = re_exec(pat, str, slen, 0, caps, 64);
  if (n <= 0 || caps[0] < 0) return str;
  int mlen = caps[1] - caps[0];
  /* 0xff marker before the transient key: keeps sp_str_hash's s[-1] read
     in-bounds and on the non-caching path (no sp_str_hdr behind this buffer). */
  char keybuf[65]; keybuf[0] = (char)0xff;
  char *kbuf = mlen + 1 < (int)sizeof(keybuf) ? keybuf : (char *)malloc(mlen + 2);
  if (kbuf != keybuf) kbuf[0] = (char)0xff;
  char *key = kbuf + 1;
  memcpy(key, str + caps[0], mlen);
  key[mlen] = 0;
  const char *rep = (h && sp_StrStrHash_has_key(h, key)) ? sp_StrStrHash_get(h, key) : "";
  size_t rlen = strlen(rep);
  size_t rest = slen - caps[1];
  size_t total = caps[0] + rlen + rest;
  char *out = sp_str_alloc_raw(total + 1);
  memcpy(out, str, caps[0]);
  memcpy(out + caps[0], rep, rlen);
  memcpy(out + caps[0] + rlen, str + caps[1], rest);
  out[total] = 0;
  if (kbuf != keybuf) free(kbuf);
  return out;
}

/* Issue #910: sub(string, hash) — literal-substring pattern
   with a hash replacement. Replaces only the first match. */
static const char *sp_str_sub_str_str_hash(const char *str, const char *pat, sp_StrStrHash *h) {
  if (!str || !pat) return str;
  const char *found = strstr(str, pat);
  if (!found) return str;
  size_t before = (size_t)(found - str);
  size_t plen = strlen(pat);
  const char *rep = (h && sp_StrStrHash_has_key(h, pat)) ? sp_StrStrHash_get(h, pat) : "";
  size_t rlen = strlen(rep);
  size_t rest = strlen(str) - before - plen;
  size_t total = before + rlen + rest;
  char *out = sp_str_alloc_raw(total + 1);
  memcpy(out, str, before);
  memcpy(out + before, rep, rlen);
  memcpy(out + before + rlen, found + plen, rest);
  out[total] = 0;
  return out;
}




/* NaN-boxed polymorphic value */
typedef uint64_t sp_RbValue;
#define SP_TAG_INT 0
#define SP_TAG_STR 1
#define SP_TAG_FLT 2
#define SP_TAG_BOOL 3
#define SP_TAG_NIL 4
#define SP_TAG_OBJ 5
#define SP_TAG_SYM 6
/* Class values boxed into a poly slot (e.g.
   as ancestors-array elements). cls_id of the sp_RbVal carries
   the boxed sp_Class's cls_id directly so unboxing is just a
   field read. */
#define SP_TAG_CLASS 7
/* Encoding values boxed into a poly slot. v.s carries the canonical
   encoding name (`"UTF-8"` in Spinel's current runtime model). */
#define SP_TAG_ENCODING 8
/* Arbitrary-precision integer boxed into a poly slot. v.p is a
   GC-allocated sp_Bigint* (so a heterogeneous Array/Hash can hold a
   bignum, and --int-overflow=promote can box an overflow result). */
#define SP_TAG_BIGINT 9
/* Negative cls_id values let SP_TAG_OBJ also carry built-in pointer
   types (IntArray, FloatArray, ...) — avoids minting a new SP_TAG_*
   per type. Non-negative cls_id stays an index into the user-class
   table as before. The element-type tag and the array cls_id are
   paired by `array_cls_id = -element_tag - 1`. */
struct sp_Exception_s;
static const char *sp_exc_class_name(volatile struct sp_Exception_s *ve);
static const char *sp_exc_message(volatile struct sp_Exception_s *ve);
/* Hash variant cls_ids — boxed into the cls_id of a poly slot so
   Hash#dig can recover the concrete hash type at runtime. */
/* SP_BUILTIN_FOREIGN_PTR (-25), SP_BUILTIN_COMPLEX (-26) and
   SP_BUILTIN_RATIONAL (-27) are defined in sp_gc.h (shared with lib readers). */
/* sp_RbVal is defined in sp_gc.h (the mark helpers dispatch on its tag). */
/* Forward declarations for the bigint API the poly helpers below call; the
   full prototypes live further down (near the bigint runtime block). */
typedef struct sp_Bigint sp_Bigint;
const char *sp_bigint_to_s(sp_Bigint *b);
mrb_int sp_bigint_bit_length(sp_Bigint *b);
int64_t sp_bigint_to_int(sp_Bigint *b);
int sp_bigint_cmp(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_new_int(int64_t v);
sp_Bigint *sp_bigint_add(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_sub(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mul(sp_Bigint *a, sp_Bigint *b);
int sp_bigint_sign(sp_Bigint *b);
size_t sp_bigint_byte_len(sp_Bigint *b);
size_t sp_bigint_to_le_bytes(sp_Bigint *b, unsigned char *out, size_t cap);
sp_Bigint *sp_bigint_from_le_bytes(int negative, const unsigned char *bytes, size_t n);
/* Boxing a nullable-int value (int?): SP_INT_NIL is the reserved nil sentinel
   and never a legitimate integer, so a sentinel must surface as Ruby nil rather
   than a boxed INT_MIN. Used when an int? value (hash miss, rindex, nonzero?,
   ...) flows into a poly slot. */
static sp_RbVal sp_box_int_or_nil(mrb_int v) { return v == SP_INT_NIL ? sp_box_nil() : sp_box_int(v); }
/* Ruby integer shifts: a negative count shifts the other way, and a count at or
   beyond the word width saturates (arithmetic for a right shift of a negative).
   A bare C shift by a negative or >= width count is undefined behaviour, so any
   non-constant / possibly-negative shift routes through these. (A left shift
   past the word width truly overflows to a Bignum in Ruby; int mode can't hold
   it, so it saturates to 0 -- the Bignum-promotion path handles the real case.) */
static inline mrb_int sp_int_shl(mrb_int a, mrb_int n) {
  if (n < 0) { mrb_int s = -n; return s >= 64 ? (a < 0 ? -1 : 0) : (a >> s); }
  return n >= 64 ? 0 : (a << n);
}
static inline mrb_int sp_int_shr(mrb_int a, mrb_int n) {
  if (n < 0) { mrb_int s = -n; return s >= 64 ? 0 : (a << s); }
  return n >= 64 ? (a < 0 ? -1 : 0) : (a >> n);
}
/* A class known only by name (an exception's cls_name -- the id table covers
   only a few exception classes, but the name is complete for all of them,
   including the open-ended Errno:: family). Marked with a sentinel cls_id so
   the SP_TAG_CLASS arms read the name from v.s instead of resolving v.i. The
   name points into rodata (see sp_exc_gc_scan), so no GC marking is needed. */
#define SP_CLASS_BY_NAME 0x7F000000
static sp_RbVal sp_box_class_name(const char *name) { sp_RbVal r; r.tag = SP_TAG_CLASS; r.cls_id = SP_CLASS_BY_NAME; r.v.s = name; return r; }
/* box a sp_Class into a poly slot (a name-backed class boxes by name). */
static sp_RbVal sp_box_class(sp_Class c) { if (c.name) return sp_box_class_name(c.name); sp_RbVal r; r.tag = SP_TAG_CLASS; r.cls_id = (int)c.cls_id; r.v.i = c.cls_id; return r; }
/* box a sp_Bigint* into a poly slot (heterogeneous container element, or a
   promote-mode overflow result). */
static sp_RbVal sp_box_bigint(sp_Bigint *b) { sp_RbVal r; r.tag = SP_TAG_BIGINT; r.cls_id = 0; r.v.p = b; return r; }
/* A poly value as a sp_Bigint*, so the bigint comparison/arith helpers can take
   a mixed operand uniformly. Total (never NULL, since sp_bigint_cmp derefs):
   a float truncates (harmless -- a bignum is always outside float-fraction
   range) and a non-numeric collapses to 0 (a type mismatch Ruby would raise
   on; Spinel yields a defined result instead of crashing). */
static sp_Bigint *sp_poly_as_bigint(sp_RbVal v) {
  if (v.tag == SP_TAG_BIGINT) return (sp_Bigint *)v.v.p;
  if (v.tag == SP_TAG_INT) return sp_bigint_new_int(v.v.i);
  if (v.tag == SP_TAG_FLT) return sp_bigint_new_int((int64_t)v.v.f);
  return sp_bigint_new_int(0);
}
static sp_RbVal sp_box_encoding(sp_Encoding e) { sp_RbVal r; r.tag = SP_TAG_ENCODING; r.cls_id = 0; r.v.s = sp_encoding_name(e); return r; }

/* every non-value-type sp_<C> starts
   with `mrb_int cls_id`. Read it back from a void* when
   the static type at the call site has lost the cls_id (e.g.
   sp_PtrArray_get returning void*). NULL-safe via the guard
   on p; expects p to actually point at a user-class struct
   when non-NULL. */
static inline mrb_int sp_obj_cls_id_of(void *p) { return p ? *(mrb_int *)p : 0; }
static sp_RbVal sp_box_nullable_str(const char *v) { return v ? sp_box_str(v) : sp_box_nil(); }
/* String#index / #rindex return a boxed nil for not-found, boxed
   int for found. Issue #532: typed-int slot can't represent CRuby's
   nil-vs-real-index distinction in-band; widening the result type
   to sp_RbVal at the call site lets `pos.nil?` and `puts pos.inspect`
   work via the standard poly-tag dispatch. The -1 sentinel comes
   from the underlying sp_str_*_index helpers; we widen here at the
   boxing layer so existing call sites that want the raw int still
   work via `sp_str_index` directly. */
static sp_RbVal sp_str_index_poly(const char *s, const char *sub) { mrb_int n = sp_str_index(s, sub); return n < 0 ? sp_box_nil() : sp_box_int(n); }
static sp_RbVal sp_str_index_from_poly(const char *s, const char *sub, mrb_int start) { mrb_int n = sp_str_index_from(s, sub, start); return n < 0 ? sp_box_nil() : sp_box_int(n); }
static sp_RbVal sp_str_rindex_poly(const char *s, const char *sub) { mrb_int n = sp_str_rindex(s, sub); return n < 0 ? sp_box_nil() : sp_box_int(n); }
/* int? siblings of the String#index family. Same not-found
   semantics as the _poly variants, but the result is the int?
   sentinel (SP_INT_NIL) rather than a boxed sp_RbVal — keeps the
   `i = s.index(sub); return ... if i.nil?; i + 1` idiom on the
   direct integer arithmetic path. */
/* `s.index(regex)` -- first match start (byte offset, as sp_re_match reports;
   matches the rindex regex variant, which also reports bytes). sp_re_match
   sets $~ as a side effect, which CRuby's String#index(regex) also does.
   Boxed Integer | nil. The plain-string path would lower the regex pattern to
   0 and feed a bogus arg to sp_str_index_opt. */
/* CRuby-compatible =~ wrapper: SP_TAG_INT(pos) on match, SP_TAG_NIL
   on miss. Codegen routes the `=~` operator here so
   `("abc" =~ /xyz/).nil?` answers true and `puts("abc" =~ /xyz/)`
   prints an empty line, matching CRuby. The raw `sp_re_match`
   (returning -1) stays available for internal callers needing the
   sentinel form. */

/* Regexp.escape(s) / Regexp.quote(s) -- prefix every regex metachar
   and whitespace byte with a single backslash, returning a heap
   string that callers can feed into `Regexp.new(...)` to match the
   original bytes literally. Matches CRuby's rb_reg_quote for the
   ASCII range (the only range Spinel's regex engine indexes today,
   so multibyte passes through unchanged).

   The metachars covered: \\ . ? * + ^ $ | ( ) [ ] { } # -
   The whitespace covered: ' ' \t \n \r \f \v
   Everything else copies byte-for-byte. */
static sp_RbVal sp_box_nullable_obj(void *p, int cls_id) { return p ? sp_box_obj(p, cls_id) : sp_box_nil(); }
/* An opaque foreign/FFI pointer: boxed with SP_BUILTIN_FOREIGN_PTR so the
   collector skips it (it is not a sp_gc_alloc allocation). NULL -> nil. */
static sp_RbVal sp_box_foreign_ptr(void *p) { return p ? sp_box_obj(p, SP_BUILTIN_FOREIGN_PTR) : sp_box_nil(); }
/* Built-in pointer boxes — share SP_TAG_OBJ with a reserved negative
   cls_id so the dispatch path is uniform. */
static sp_RbVal sp_box_int_array(void *p)   { return sp_box_obj(p, SP_BUILTIN_INT_ARRAY); }
static sp_RbVal sp_box_float_array(void *p) { return sp_box_obj(p, SP_BUILTIN_FLT_ARRAY); }
static sp_RbVal sp_box_str_array(void *p)   { return sp_box_obj(p, SP_BUILTIN_STR_ARRAY); }
static sp_RbVal sp_box_sym_array(void *p)   { return sp_box_obj(p, SP_BUILTIN_SYM_ARRAY); }
static sp_RbVal sp_box_ptr_array(void *p)   { return sp_box_obj(p, SP_BUILTIN_PTR_ARRAY); }
static sp_RbVal sp_box_proc(void *p)        { return sp_box_obj(p, SP_BUILTIN_PROC); }
static sp_RbVal sp_box_method(void *p)      { return sp_box_obj(p, SP_BUILTIN_METHOD); }

/* CRuby-compatible Array#index / #rindex / #find_index: returns
   sp_RbVal (nil tag for not-found, int tag with the position when
   found). spinel's raw `_index` helpers return the -1 sentinel,
   which diverges from CRuby's nil. Codegen routes
   `arr.index(x)` / `arr.find_index(x)` / `arr.rindex(x)` through
   these `_poly` wrappers so `.nil?` / `== nil` / `inspect` etc.
   on the result behave the CRuby way. Sibling to
   `sp_str_index_poly` above; same widening rationale.
   Issue raised during #585 follow-up: spinel positions itself
   as a Ruby SUBSET, so documented Ruby APIs must match CRuby
   behavior. */
/* int? siblings of the *_index_poly wrappers above. Same not-found
   semantics, but return the int? sentinel (SP_INT_NIL) instead of
   boxing into sp_RbVal. Used when the call site's static type
   tracking carries the result as int? rather than poly — eliminates
   the box/unbox round-trip for the common `i = arr.index(x);
   i.nil? ? ... : <use i as int>` idiom. */
/* Inspect / to_s for an int? value. CRuby distinguishes the two on
   nil: `nil.to_s` is "" while `nil.inspect` is "nil". For a real
   integer they agree (Integer#to_s and #inspect are both the decimal
   form). Two wrappers keep call-site emit local. */
static const char *sp_int_opt_inspect(mrb_int v) { return sp_int_is_nil(v) ? "nil" : sp_int_to_s(v); }
static const char *sp_int_opt_to_s(mrb_int v)    { return sp_int_is_nil(v) ? "" : sp_int_to_s(v); }
/* float? (nullable float) counterparts: a non-nil value formats exactly
   like a plain Float (delegates to sp_float_to_s), nil renders "nil"
   (inspect) / "" (to_s). */
static const char *sp_float_opt_inspect(mrb_float v) { return sp_float_is_nil(v) ? "nil" : sp_float_to_s(v); }
static const char *sp_float_opt_to_s(mrb_float v)    { return sp_float_is_nil(v) ? "" : sp_float_to_s(v); }
/* sp_Range is a 16-byte value type that doesn't fit in sp_RbVal's union
   (max 8 bytes). When a Range crosses into a poly slot (heterogeneous
   hash / array / param / ivar), copy it onto the GC heap and box the
   pointer via SP_BUILTIN_RANGE. The Range has no internal pointer fields
   so no scanner is needed. */
static sp_RbVal sp_box_range(sp_Range v) {
  sp_Range *p = (sp_Range *)sp_gc_alloc(sizeof(sp_Range), NULL, NULL);
  *p = v;
  return sp_box_obj(p, SP_BUILTIN_RANGE);
}
/* Complex / Rational are wider value types (two components); like sp_Range they
   heap-copy when crossing into a poly slot. No internal pointers, so no scan. */
static sp_RbVal sp_box_complex(sp_Complex v) {
  sp_Complex *p = (sp_Complex *)sp_gc_alloc(sizeof(sp_Complex), NULL, NULL);
  *p = v;
  return sp_box_obj(p, SP_BUILTIN_COMPLEX);
}
static sp_RbVal sp_box_rational(sp_Rational v) {
  sp_Rational *p = (sp_Rational *)sp_gc_alloc(sizeof(sp_Rational), NULL, NULL);
  *p = v;
  return sp_box_obj(p, SP_BUILTIN_RATIONAL);
}
/* sp_Range_inspect moved to lib/sp_format.c (cold). */
/* Same heap-box rationale as sp_Range: sp_Time is 12+ bytes (tv_sec +
   tv_nsec), wider than sp_RbVal's 8-byte union. No internal pointers
   so no scanner is needed. */
static sp_RbVal sp_box_time(sp_Time v) {
  sp_Time *p = (sp_Time *)sp_gc_alloc(sizeof(sp_Time), NULL, NULL);
  *p = v;
  return sp_box_obj(p, SP_BUILTIN_TIME);
}
/* sp_Time_inspect moved to lib/sp_format.c (cold). */
static const char *sp_class_to_s(sp_Class c); /* fwd decl: sp_poly_puts' SP_TAG_CLASS arm */
/* Name of a boxed SP_TAG_CLASS value: a name-backed box carries it in v.s,
   otherwise resolve the cls_id through the generated id->name table. */
static inline const char *sp_class_val_name(sp_RbVal v) {
  if (v.cls_id == SP_CLASS_BY_NAME) return v.v.s ? v.v.s : "";
  sp_Class _c = {v.v.i, NULL};
  return sp_class_to_s(_c);
}
/* Class identity: a name-backed class compares by its (complete) name, so it
   equals the id-backed class of the same name. */
static inline mrb_bool sp_class_eq(sp_Class a, sp_Class b) {
  if (!a.name && !b.name) return a.cls_id == b.cls_id;
  const char *an = sp_class_to_s(a), *bn = sp_class_to_s(b);
  return (an && bn) ? strcmp(an, bn) == 0 : an == bn;
}
static inline void sp_poly_puts(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_INT: printf("%lld\n", (long long)v.v.i); break;
    case SP_TAG_STR: if (v.v.s) { fputs(v.v.s, stdout); if (!*v.v.s || v.v.s[strlen(v.v.s)-1] != '\n') putchar('\n'); } else putchar('\n'); break;
    case SP_TAG_FLT: { fputs(sp_float_to_s(v.v.f), stdout); putchar('\n'); break; }
    case SP_TAG_BOOL: puts(v.v.b ? "true" : "false"); break;
    case SP_TAG_NIL: putchar('\n'); break;
    case SP_TAG_SYM: { const char *_ss = sp_sym_to_s((sp_sym)v.v.i); fputs(_ss, stdout); putchar('\n'); break; }
    case SP_TAG_ENCODING: { const char *_es = v.v.s ? v.v.s : sp_str_empty; fputs(_es, stdout); putchar('\n'); break; }
    case SP_TAG_CLASS: { fputs(sp_class_val_name(v), stdout); putchar('\n'); break; }
    case SP_TAG_BIGINT: { const char *_bs = sp_bigint_to_s((sp_Bigint *)v.v.p); if (_bs) { fputs(_bs, stdout); free((void *)_bs); } putchar('\n'); break; }
    case SP_TAG_OBJ: {
      /* MRI's `puts arr` iterates an Array, printing one element per
         line (using to_s on each); a non-Array OBJ falls back to
         inspect / class-name. */
      switch (v.cls_id) {
        case SP_BUILTIN_INT_ARRAY: {
          sp_IntArray *_a = (sp_IntArray *)v.v.p;
          for (mrb_int _i = 0; _i < _a->len; _i++)
            printf("%lld\n", (long long)_a->data[_a->start + _i]);
          break;
        }
        case SP_BUILTIN_FLT_ARRAY: {
          sp_FloatArray *_a = (sp_FloatArray *)v.v.p;
          for (mrb_int _i = 0; _i < _a->len; _i++) {
            fputs(sp_float_to_s(_a->data[_i]), stdout); putchar('\n');
          }
          break;
        }
        case SP_BUILTIN_STR_ARRAY: {
          sp_StrArray *_a = (sp_StrArray *)v.v.p;
          for (mrb_int _i = 0; _i < _a->len; _i++) {
            const char *_s = _a->data[_i];
            if (_s) { fputs(_s, stdout); if (!*_s || _s[strlen(_s)-1] != '\n') putchar('\n'); } else putchar('\n');
          }
          break;
        }
        case SP_BUILTIN_SYM_ARRAY: {
          sp_IntArray *_a = (sp_IntArray *)v.v.p;
          for (mrb_int _i = 0; _i < _a->len; _i++) {
            const char *_s = sp_sym_to_s((sp_sym)_a->data[_a->start + _i]);
            fputs(_s, stdout); putchar('\n');
          }
          break;
        }
        case SP_BUILTIN_RANGE: puts(sp_Range_inspect((sp_Range *)v.v.p)); break;
        case SP_BUILTIN_TIME: puts(sp_Time_inspect((sp_Time *)v.v.p)); break;
        case SP_BUILTIN_COMPLEX: puts(sp_complex_to_s(*(sp_Complex *)v.v.p)); break;
        case SP_BUILTIN_RATIONAL: puts(sp_rational_to_s(*(sp_Rational *)v.v.p)); break;
        default: printf("#<Object:0x%p>\n", v.v.p); break;
      }
      break;
    }
    default: printf("%lld\n", (long long)v.v.i); break;
  }
}
static mrb_bool sp_poly_nil_p(sp_RbVal v) { return v.tag == SP_TAG_NIL; }
static mrb_bool sp_poly_truthy(sp_RbVal v) { return !(v.tag == SP_TAG_NIL || (v.tag == SP_TAG_BOOL && !v.v.b)); }
/* forward-declare the program-emitted class
   name lookup so sp_poly_to_s's SP_TAG_CLASS arm resolves.
   The codegen emits a 1-line stub when no class const is used,
   or the real body when @needs_class_table fires. The forward
   decl always needs a definition somewhere because -Werror
   trips on "used but never defined" otherwise. */
static const char *sp_class_to_s(sp_Class c);
static inline const char *sp_poly_to_s(sp_RbVal v) {
  switch (v.tag) {
    /* int-typed nil (SP_INT_NIL) is Ruby nil; nil.to_s is "" -- match it. */
    case SP_TAG_INT: return v.v.i == SP_INT_NIL ? sp_str_empty : sp_int_to_s(v.v.i);
    case SP_TAG_STR: return v.v.s ? v.v.s : sp_str_empty;
    case SP_TAG_FLT: return sp_float_to_s(v.v.f);
    case SP_TAG_BOOL: return v.v.b ? SPL("true") : SPL("false");
    case SP_TAG_NIL: return sp_str_empty;
    case SP_TAG_SYM: return sp_sym_to_s((sp_sym)v.v.i);
    case SP_TAG_CLASS: return sp_class_val_name(v);
    case SP_TAG_ENCODING: return v.v.s ? v.v.s : sp_str_empty;
    case SP_TAG_BIGINT: return sp_bigint_to_s((sp_Bigint *)v.v.p);
    case SP_TAG_OBJ:
      switch (v.cls_id) {
        case SP_BUILTIN_INT_ARRAY: return sp_IntArray_inspect((sp_IntArray *)v.v.p);
        case SP_BUILTIN_FLT_ARRAY: return sp_FloatArray_inspect((sp_FloatArray *)v.v.p);
        case SP_BUILTIN_STR_ARRAY: return sp_StrArray_inspect((sp_StrArray *)v.v.p);
        case SP_BUILTIN_SYM_ARRAY: return sp_SymArray_inspect((sp_IntArray *)v.v.p);
        case SP_BUILTIN_PTR_ARRAY: return sp_PtrArray_inspect((sp_PtrArray *)v.v.p);
        case SP_BUILTIN_RANGE: return sp_Range_inspect((sp_Range *)v.v.p);
        case SP_BUILTIN_TIME: return sp_Time_inspect((sp_Time *)v.v.p);
        case SP_BUILTIN_COMPLEX: return sp_complex_to_s(*(sp_Complex *)v.v.p);
        case SP_BUILTIN_RATIONAL: return sp_rational_to_s(*(sp_Rational *)v.v.p);
        case SP_BUILTIN_EXCEPTION: return sp_exc_message((volatile struct sp_Exception_s *)v.v.p);
        default: return sp_str_empty;
      }
    default: return sp_str_empty;
  }
}
/* Class name of a boxed value, for `x.class` where x is poly. Returns a
   .rodata or names-table string (never GC-managed). */
static const char *sp_poly_class_name(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_INT: return SPL("Integer");
    case SP_TAG_STR: return SPL("String");
    case SP_TAG_FLT: return SPL("Float");
    case SP_TAG_BOOL: return v.v.b ? SPL("TrueClass") : SPL("FalseClass");
    case SP_TAG_NIL: return SPL("NilClass");
    case SP_TAG_SYM: return SPL("Symbol");
    case SP_TAG_ENCODING: return SPL("Encoding");
    case SP_TAG_CLASS: return SPL("Class");
    case SP_TAG_BIGINT: return SPL("Integer");
    case SP_TAG_OBJ:
      switch (v.cls_id) {
        case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_FLT_ARRAY:
        case SP_BUILTIN_STR_ARRAY: case SP_BUILTIN_SYM_ARRAY:
        case SP_BUILTIN_PTR_ARRAY: case SP_BUILTIN_POLY_ARRAY: return SPL("Array");
        case SP_BUILTIN_RANGE: return SPL("Range");
        case SP_BUILTIN_TIME: return SPL("Time");
        case SP_BUILTIN_COMPLEX: return SPL("Complex");
        case SP_BUILTIN_RATIONAL: return SPL("Rational");
        case SP_BUILTIN_OBJECT: return SPL("Object");   /* a bare Object.new instance */
        case SP_BUILTIN_EXCEPTION: return sp_exc_class_name((volatile struct sp_Exception_s *)v.v.p);
        default: { sp_Class c = {v.cls_id}; return sp_class_to_s(c); }
      }
    default: return SPL("Object");
  }
}
/* .class as a first-class value: name-backed for every receiver kind, so it
   compares via sp_class_eq (name identity) and prints via sp_class_to_s. */
static sp_Class sp_poly_class_val(sp_RbVal v) {
  sp_Class r; r.cls_id = -1; r.name = sp_poly_class_name(v); return r;
}
/* Raise TypeError "no implicit conversion of <class> into String" for a poly
   value, naming its actual runtime class (the statically-typed path bakes the
   class name into a literal; the poly path resolves it here). */
SP_NORETURN SP_COLD static void sp_raise_no_str_conversion(sp_RbVal v) {
  static char buf[128];
  snprintf(buf, sizeof buf, "no implicit conversion of %s into String", sp_poly_class_name(v));
  sp_raise_cls("TypeError", buf);
}
static mrb_bool sp_PolyArray_eq(sp_PolyArray *a, sp_PolyArray *b);
static mrb_float sp_poly_to_f(sp_RbVal v);  /* defined below; used by the bigint+float arms */
static inline int sp_poly_is_array_kind(int cls_id);                       /* defined below; used by the poly `+` array arm */
static sp_PolyArray *sp_poly_to_poly_array(sp_RbVal v);                    /* defined below */
static sp_PolyArray *sp_PolyArray_concat(sp_PolyArray *a, sp_PolyArray *b); /* defined below */
/* int+int that auto-promotes to bigint on overflow in --int-overflow=promote;
   plain (wrapping) C arithmetic otherwise, matching the sp_int_* macro policy. */
#ifdef SP_INT_OVERFLOW_MODE_PROMOTE
#  define SP_POLY_INT_OP(op, x, y) ({ mrb_int _r; sp_int_##op##_overflow_p((x), (y), &_r) \
     ? sp_box_bigint(sp_bigint_##op(sp_bigint_new_int(x), sp_bigint_new_int(y))) : sp_box_int(_r); })
#else
#  define SP_POLY_INT_OP(op, x, y) sp_box_int(sp_int_c_##op((x), (y)))
#endif
static inline mrb_int sp_int_c_add(mrb_int x, mrb_int y) { return x + y; }
static inline mrb_int sp_int_c_sub(mrb_int x, mrb_int y) { return x - y; }
static inline mrb_int sp_int_c_mul(mrb_int x, mrb_int y) { return x * y; }
static sp_RbVal sp_poly_add(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_BIGINT || b.tag == SP_TAG_BIGINT) { if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) return sp_box_float(sp_poly_to_f(a) + sp_poly_to_f(b)); return sp_box_bigint(sp_bigint_add(sp_poly_as_bigint(a), sp_poly_as_bigint(b))); } if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return SP_POLY_INT_OP(add, a.v.i, b.v.i); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(a.v.f + b.v.f); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_FLT) return sp_box_float((mrb_float)a.v.i + b.v.f); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_INT) return sp_box_float(a.v.f + (mrb_float)b.v.i); if (a.tag == SP_TAG_STR && b.tag == SP_TAG_STR) return sp_box_str(sp_str_concat(a.v.s, b.v.s)); if (a.tag == SP_TAG_OBJ && sp_poly_is_array_kind(a.cls_id) && b.tag == SP_TAG_OBJ && sp_poly_is_array_kind(b.cls_id)) { SP_GC_ROOT_RBVAL(a); SP_GC_ROOT_RBVAL(b); sp_PolyArray *pa = sp_poly_to_poly_array(a); SP_GC_ROOT(pa); sp_PolyArray *pb = sp_poly_to_poly_array(b); SP_GC_ROOT(pb); return sp_box_poly_array(sp_PolyArray_concat(pa, pb)); } return sp_box_int(0); }
static sp_RbVal sp_poly_sub(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_BIGINT || b.tag == SP_TAG_BIGINT) { if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) return sp_box_float(sp_poly_to_f(a) - sp_poly_to_f(b)); return sp_box_bigint(sp_bigint_sub(sp_poly_as_bigint(a), sp_poly_as_bigint(b))); } if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return SP_POLY_INT_OP(sub, a.v.i, b.v.i); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(a.v.f - b.v.f); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_FLT) return sp_box_float((mrb_float)a.v.i - b.v.f); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_INT) return sp_box_float(a.v.f - (mrb_float)b.v.i); return sp_box_int(0); }
static sp_RbVal sp_poly_mul(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_BIGINT || b.tag == SP_TAG_BIGINT) { if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) return sp_box_float(sp_poly_to_f(a) * sp_poly_to_f(b)); return sp_box_bigint(sp_bigint_mul(sp_poly_as_bigint(a), sp_poly_as_bigint(b))); } if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT) return SP_POLY_INT_OP(mul, a.v.i, b.v.i); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_FLT) return sp_box_float(a.v.f * b.v.f); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_FLT) return sp_box_float((mrb_float)a.v.i * b.v.f); if (a.tag == SP_TAG_FLT && b.tag == SP_TAG_INT) return sp_box_float(a.v.f * (mrb_float)b.v.i); if (a.tag == SP_TAG_STR && b.tag == SP_TAG_INT) return a.v.s ? sp_box_str(sp_str_repeat(a.v.s, b.v.i)) : a; /* String#*; NULL is the empty string */ return sp_box_int(0); }
static mrb_int sp_poly_to_i(sp_RbVal v) { if (v.tag == SP_TAG_INT || v.tag == SP_TAG_SYM) return v.v.i; if (v.tag == SP_TAG_BIGINT) return (mrb_int)sp_bigint_to_int((sp_Bigint *)v.v.p); if (v.tag == SP_TAG_STR) return (mrb_int)strtoll(v.v.s ? v.v.s : sp_str_empty, NULL, 10); if (v.tag == SP_TAG_FLT) return (mrb_int)v.v.f; if (v.tag == SP_TAG_BOOL) return v.v.b ? 1 : 0; return 0; }
static mrb_float sp_poly_to_f(sp_RbVal v) { if (v.tag == SP_TAG_FLT) return v.v.f; if (v.tag == SP_TAG_INT || v.tag == SP_TAG_SYM) return (mrb_float)v.v.i; if (v.tag == SP_TAG_BIGINT) return (mrb_float)sp_bigint_to_int((sp_Bigint *)v.v.p); if (v.tag == SP_TAG_BOOL) return v.v.b ? 1.0 : 0.0; return 0.0; }
/* Unbox to float? preserving nil as the float-nil sentinel. Used by the
   unpack1 literal-float-directive fast path: sp_str_unpack pads short input
   with nil, which must stay nil through the unboxed TY_FLOAT result (CRuby
   returns nil there) instead of coercing to 0.0 like sp_poly_to_f. */
static mrb_float sp_poly_to_f_opt(sp_RbVal v) { return v.tag == SP_TAG_NIL ? sp_float_nil() : sp_poly_to_f(v); }
static mrb_bool sp_poly_numeric_p(sp_RbVal v) { return v.tag == SP_TAG_INT || v.tag == SP_TAG_FLT || v.tag == SP_TAG_BIGINT; }
/* Numeric queries / rounding on a poly value: dispatch on the runtime tag the
   way CRuby dispatches on the class. A tag whose class does not define the
   method raises CRuby's NoMethodError (e.g. `1.nan?`, `"x".abs`). */
SP_NORETURN SP_COLD static void sp_raise_poly_nomethod(const char *m, sp_RbVal v) {
  sp_raise_cls("NoMethodError",
               sp_sprintf("undefined method '%s' for an instance of %s", m, sp_poly_class_name(v)));
}
/* floor/ceil/round/truncate on a non-finite Float: casting NaN/Inf to an
   integer is C UB; CRuby raises FloatDomainError naming the value. */
static inline void sp_poly_flo_domain_ck(mrb_float f) {
  if (!isfinite(f)) sp_raise_cls("FloatDomainError", isnan(f) ? "NaN" : f > 0 ? "Infinity" : "-Infinity");
}
static mrb_bool sp_poly_nan_p(sp_RbVal v) { if (v.tag == SP_TAG_FLT) return isnan(v.v.f) != 0; sp_raise_poly_nomethod("nan?", v); }
static mrb_bool sp_poly_finite_p(sp_RbVal v) { if (v.tag == SP_TAG_FLT) return isfinite(v.v.f) != 0; if (v.tag == SP_TAG_INT || v.tag == SP_TAG_BIGINT) return TRUE; sp_raise_poly_nomethod("finite?", v); }
static sp_RbVal sp_poly_infinite(sp_RbVal v) { if (v.tag == SP_TAG_FLT) return isinf(v.v.f) ? sp_box_int(v.v.f > 0 ? 1 : -1) : sp_box_nil(); if (v.tag == SP_TAG_INT || v.tag == SP_TAG_BIGINT) return sp_box_nil(); sp_raise_poly_nomethod("infinite?", v); }
static mrb_bool sp_poly_zero_p(sp_RbVal v) { if (v.tag == SP_TAG_INT) return v.v.i == 0; if (v.tag == SP_TAG_FLT) return v.v.f == 0.0; if (v.tag == SP_TAG_BIGINT) return sp_bigint_sign((sp_Bigint *)v.v.p) == 0; sp_raise_poly_nomethod("zero?", v); }
static mrb_bool sp_poly_positive_p(sp_RbVal v) { if (v.tag == SP_TAG_INT) return v.v.i > 0; if (v.tag == SP_TAG_FLT) return v.v.f > 0.0; if (v.tag == SP_TAG_BIGINT) return sp_bigint_sign((sp_Bigint *)v.v.p) > 0; sp_raise_poly_nomethod("positive?", v); }
static mrb_bool sp_poly_negative_p(sp_RbVal v) { if (v.tag == SP_TAG_INT) return v.v.i < 0; if (v.tag == SP_TAG_FLT) return v.v.f < 0.0; if (v.tag == SP_TAG_BIGINT) return sp_bigint_sign((sp_Bigint *)v.v.p) < 0; sp_raise_poly_nomethod("negative?", v); }
/* abs of a negative int goes through SP_POLY_INT_OP(sub, 0, x): plain -x is
   UB for INT_MIN; promote mode boxes it as a bigint, wrap mode keeps the
   documented wrapping C arithmetic. fabs covers -0.0 -> 0.0 too. */
static sp_RbVal sp_poly_abs(sp_RbVal v) { if (v.tag == SP_TAG_INT) { if (v.v.i >= 0) return v; return SP_POLY_INT_OP(sub, (mrb_int)0, v.v.i); } if (v.tag == SP_TAG_FLT) return sp_box_float(fabs(v.v.f)); if (v.tag == SP_TAG_BIGINT) { sp_Bigint *b = (sp_Bigint *)v.v.p; return sp_bigint_sign(b) < 0 ? sp_box_bigint(sp_bigint_sub(sp_bigint_new_int(0), b)) : v; } sp_raise_poly_nomethod("abs", v); }
/* No-arg floor/ceil/round/truncate return Integer in Ruby: an int/bigint tag
   is already its own floor (returned unchanged, lossless for bigints), a
   float converts through the matching libm rounding. */
static sp_RbVal sp_poly_floor(sp_RbVal v) { if (v.tag == SP_TAG_FLT) { sp_poly_flo_domain_ck(v.v.f); return sp_box_int((mrb_int)floor(v.v.f)); } if (v.tag == SP_TAG_INT || v.tag == SP_TAG_BIGINT) return v; sp_raise_poly_nomethod("floor", v); }
/* a NULL char* carried under SP_TAG_STR is the empty string (as in
   sp_poly_to_i / sp_poly_eq): bytesize 0, ord raises CRuby's ArgumentError. */
static mrb_int sp_poly_bytesize(sp_RbVal v) { if (v.tag == SP_TAG_STR) return v.v.s ? sp_str_bytesize_m(v.v.s) : 0; sp_raise_poly_nomethod("bytesize", v); }
static mrb_int sp_poly_ord(sp_RbVal v) { if (v.tag == SP_TAG_STR) { if (!v.v.s) sp_raise_cls("ArgumentError", "empty string"); return sp_str_ord(v.v.s); } if (v.tag == SP_TAG_INT) return v.v.i; sp_raise_poly_nomethod("ord", v); }
static mrb_int sp_poly_bit_length(sp_RbVal v) { if (v.tag == SP_TAG_INT) return sp_int_bit_length(v.v.i); sp_raise_poly_nomethod("bit_length", v); }
/* String#getbyte on a poly value; nil (not 0) for an out-of-range index, per
   CRuby, so the result is boxed. */
static sp_RbVal sp_poly_getbyte(sp_RbVal v, mrb_int i) { if (v.tag != SP_TAG_STR) sp_raise_poly_nomethod("getbyte", v); const char *s = v.v.s; if (!s) return sp_box_nil(); mrb_int bl = (mrb_int)sp_str_byte_len(s); if (i < 0) i += bl; if (i < 0 || i >= bl) return sp_box_nil(); return sp_box_int((mrb_int)(unsigned char)s[i]); }
static sp_RbVal sp_poly_ceil(sp_RbVal v) { if (v.tag == SP_TAG_FLT) { sp_poly_flo_domain_ck(v.v.f); return sp_box_int((mrb_int)ceil(v.v.f)); } if (v.tag == SP_TAG_INT || v.tag == SP_TAG_BIGINT) return v; sp_raise_poly_nomethod("ceil", v); }
static sp_RbVal sp_poly_round(sp_RbVal v) { if (v.tag == SP_TAG_FLT) { sp_poly_flo_domain_ck(v.v.f); return sp_box_int((mrb_int)round(v.v.f)); } if (v.tag == SP_TAG_INT || v.tag == SP_TAG_BIGINT) return v; sp_raise_poly_nomethod("round", v); }
static sp_RbVal sp_poly_truncate(sp_RbVal v) { if (v.tag == SP_TAG_FLT) { sp_poly_flo_domain_ck(v.v.f); return sp_box_int((mrb_int)trunc(v.v.f)); } if (v.tag == SP_TAG_INT || v.tag == SP_TAG_BIGINT) return v; sp_raise_poly_nomethod("truncate", v); }
/* forward: generic array length/element (defined later in this header) and
   the array-kind predicate for cross-kind value equality. */
static mrb_int sp_poly_length(sp_RbVal v);
static sp_RbVal sp_poly_arr_get(sp_RbVal a, mrb_int i);
/* poly-valued hash variants are defined later in this header */
typedef struct sp_StrPolyHash sp_StrPolyHash;
typedef struct sp_SymPolyHash sp_SymPolyHash;
typedef struct sp_PolyPolyHash sp_PolyPolyHash;
static mrb_bool sp_StrPolyHash_eq(sp_StrPolyHash *a, sp_StrPolyHash *b);
static mrb_bool sp_SymPolyHash_eq(sp_SymPolyHash *a, sp_SymPolyHash *b);
static mrb_bool sp_PolyPolyHash_eq(sp_PolyPolyHash *a, sp_PolyPolyHash *b);
static inline int sp_poly_is_array_kind(int cls_id) {
  return cls_id == SP_BUILTIN_INT_ARRAY || cls_id == SP_BUILTIN_STR_ARRAY ||
         cls_id == SP_BUILTIN_FLT_ARRAY || cls_id == SP_BUILTIN_SYM_ARRAY ||
         cls_id == SP_BUILTIN_POLY_ARRAY;
}
static mrb_bool sp_poly_eq(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_BIGINT || b.tag == SP_TAG_BIGINT) { sp_Bigint *ba = sp_poly_as_bigint(a), *bb = sp_poly_as_bigint(b); if (ba && bb) return sp_bigint_cmp(ba, bb) == 0; if (sp_poly_numeric_p(a) && sp_poly_numeric_p(b)) return sp_poly_to_f(a) == sp_poly_to_f(b); return FALSE; } if (sp_poly_numeric_p(a) && sp_poly_numeric_p(b)) return sp_poly_to_f(a) == sp_poly_to_f(b); if (a.tag != b.tag) return FALSE; switch (a.tag) { case SP_TAG_INT: return a.v.i == b.v.i; case SP_TAG_STR: return (a.v.s == NULL || b.v.s == NULL) ? (a.v.s == b.v.s) : (strcmp(a.v.s, b.v.s) == 0); case SP_TAG_FLT: return a.v.f == b.v.f; case SP_TAG_BOOL: return a.v.b == b.v.b; case SP_TAG_NIL: return TRUE; case SP_TAG_SYM: return a.v.i == b.v.i; case SP_TAG_ENCODING: return (a.v.s == NULL || b.v.s == NULL) ? (a.v.s == b.v.s) : (strcmp(a.v.s, b.v.s) == 0); case SP_TAG_OBJ: /* Arrays compare by VALUE across storage kinds: [1,2] boxed as an IntArray equals the same numbers rebuilt as a PolyArray (a splat-rest, a mapped run). Ruby has one Array; the kinds are a storage optimization and must not leak into ==. */ if (sp_poly_is_array_kind(a.cls_id) && sp_poly_is_array_kind(b.cls_id)) { if (a.cls_id == b.cls_id && a.v.p == b.v.p) return TRUE; { mrb_int __n = sp_poly_length(a); if (__n != sp_poly_length(b)) return FALSE; for (mrb_int __i = 0; __i < __n; __i++) if (!sp_poly_eq(sp_poly_arr_get(a, __i), sp_poly_arr_get(b, __i))) return FALSE; return TRUE; } } if (a.cls_id != b.cls_id) return FALSE; if (a.v.p == b.v.p) return TRUE; switch (a.cls_id) { case SP_BUILTIN_INT_ARRAY: return sp_IntArray_eq((sp_IntArray*)a.v.p,(sp_IntArray*)b.v.p); case SP_BUILTIN_STR_ARRAY: return sp_StrArray_eq((sp_StrArray*)a.v.p,(sp_StrArray*)b.v.p); case SP_BUILTIN_FLT_ARRAY: return sp_FloatArray_eq((sp_FloatArray*)a.v.p,(sp_FloatArray*)b.v.p); case SP_BUILTIN_POLY_ARRAY: return sp_PolyArray_eq((sp_PolyArray*)a.v.p,(sp_PolyArray*)b.v.p); /* boxed hashes of the same variant compare by value like every other
     container -- the arm was simply missing, so [h] == [h-literal] was
     pointer identity and always false. */ case SP_BUILTIN_STR_INT_HASH: return sp_StrIntHash_eq((sp_StrIntHash*)a.v.p,(sp_StrIntHash*)b.v.p); case SP_BUILTIN_STR_STR_HASH: return sp_StrStrHash_eq((sp_StrStrHash*)a.v.p,(sp_StrStrHash*)b.v.p); case SP_BUILTIN_INT_STR_HASH: return sp_IntStrHash_eq((sp_IntStrHash*)a.v.p,(sp_IntStrHash*)b.v.p); case SP_BUILTIN_STR_POLY_HASH: return sp_StrPolyHash_eq((sp_StrPolyHash*)a.v.p,(sp_StrPolyHash*)b.v.p); case SP_BUILTIN_SYM_POLY_HASH: return sp_SymPolyHash_eq((sp_SymPolyHash*)a.v.p,(sp_SymPolyHash*)b.v.p); case SP_BUILTIN_POLY_POLY_HASH: return sp_PolyPolyHash_eq((sp_PolyPolyHash*)a.v.p,(sp_PolyPolyHash*)b.v.p); default: return FALSE; } case SP_TAG_CLASS: { const char *an = sp_class_val_name(a), *bn = sp_class_val_name(b); return (an && bn) ? strcmp(an, bn) == 0 : an == bn; } default: return FALSE; } }
/* sp_sym_name_fn is now an extern hook (sp_gc.h / sp_gc.c) so cold lib readers
   like sp_json.c can resolve symbol names too; the generated TU still sets it. */
static mrb_int sp_poly_arr_cmp(sp_RbVal a, sp_RbVal b, mrb_bool *comparable);
/* A user class that mixes in Comparable defines `<=>` in generated code, which
   the runtime archive cannot call directly. The generated TU installs
   sp_obj_cmp_hook to dispatch `<=>` by cls_id; sp_poly_cmp consults it for two
   user objects so no-block sort/min/max/clamp compare correctly. A nil `<=>`
   result clears *comparable, which the callers turn into an ArgumentError. */
typedef mrb_int (*sp_obj_cmp_fn)(sp_RbVal a, sp_RbVal b, mrb_bool *comparable);
static sp_obj_cmp_fn sp_obj_cmp_hook = NULL;
#define SP_IS_BUILTIN_ARRAY(id) ((id) == SP_BUILTIN_INT_ARRAY || (id) == SP_BUILTIN_STR_ARRAY || \
                                 (id) == SP_BUILTIN_FLT_ARRAY || (id) == SP_BUILTIN_POLY_ARRAY)
static mrb_int sp_poly_cmp(sp_RbVal a, sp_RbVal b, mrb_bool *comparable) { if (a.tag == SP_TAG_OBJ && b.tag == SP_TAG_OBJ && SP_IS_BUILTIN_ARRAY(a.cls_id) && SP_IS_BUILTIN_ARRAY(b.cls_id)) return sp_poly_arr_cmp(a, b, comparable); if (a.tag == SP_TAG_BIGINT || b.tag == SP_TAG_BIGINT) { sp_Bigint *ba = sp_poly_as_bigint(a), *bb = sp_poly_as_bigint(b); if (ba && bb) { *comparable = TRUE; return sp_bigint_cmp(ba, bb); } if (sp_poly_numeric_p(a) && sp_poly_numeric_p(b)) { mrb_float af = sp_poly_to_f(a), bf = sp_poly_to_f(b); *comparable = TRUE; return (af > bf) - (af < bf); } *comparable = FALSE; return 0; } if (sp_poly_numeric_p(a) && sp_poly_numeric_p(b)) { mrb_float af = sp_poly_to_f(a), bf = sp_poly_to_f(b); *comparable = TRUE; return (af > bf) - (af < bf); } if (a.tag == SP_TAG_STR && b.tag == SP_TAG_STR) { if (a.v.s == NULL || b.v.s == NULL) { *comparable = (a.v.s == b.v.s); return 0; } *comparable = TRUE; return strcmp(a.v.s, b.v.s); } if (a.tag == SP_TAG_SYM && b.tag == SP_TAG_SYM) { *comparable = TRUE; if (sp_sym_name_fn) { int _r = strcmp(sp_sym_name_fn((sp_sym)a.v.i), sp_sym_name_fn((sp_sym)b.v.i)); return _r; } return (a.v.i > b.v.i) - (a.v.i < b.v.i); } if (a.tag == SP_TAG_OBJ && sp_obj_cmp_hook) return sp_obj_cmp_hook(a, b, comparable); *comparable = FALSE; return 0; }
/* Lexicographic <=> between two boxed int arrays (Array#<=> over int elems),
   so Array#max/min/sort work on an array of int pairs ([delta, idx] tuples). */
static mrb_int sp_poly_cmp_int_arrays(sp_RbVal a, sp_RbVal b, mrb_bool *comparable) {
  if (a.tag != SP_TAG_OBJ || b.tag != SP_TAG_OBJ ||
      a.cls_id != SP_BUILTIN_INT_ARRAY || b.cls_id != SP_BUILTIN_INT_ARRAY) { *comparable = FALSE; return 0; }
  sp_IntArray *x = (sp_IntArray *)a.v.p, *y = (sp_IntArray *)b.v.p;
  if (!x || !y) { *comparable = FALSE; return 0; }
  mrb_int n = x->len < y->len ? x->len : y->len;
  for (mrb_int i = 0; i < n; i++) {
    mrb_int xe = x->data[x->start + i], ye = y->data[y->start + i];
    if (xe != ye) { *comparable = TRUE; return xe < ye ? -1 : 1; }
  }
  *comparable = TRUE;
  return (x->len > y->len) - (x->len < y->len);
}
static mrb_bool sp_poly_lt(sp_RbVal a, sp_RbVal b) { mrb_bool comparable; mrb_int cmp = sp_poly_cmp(a, b, &comparable); return comparable ? (cmp < 0) : FALSE; }
static mrb_bool sp_poly_le(sp_RbVal a, sp_RbVal b) { mrb_bool comparable; mrb_int cmp = sp_poly_cmp(a, b, &comparable); return comparable ? (cmp <= 0) : FALSE; }
static mrb_bool sp_poly_gt(sp_RbVal a, sp_RbVal b) { mrb_bool comparable; mrb_int cmp = sp_poly_cmp(a, b, &comparable); return comparable ? (cmp > 0) : FALSE; }
static mrb_bool sp_poly_ge(sp_RbVal a, sp_RbVal b) { mrb_bool comparable; mrb_int cmp = sp_poly_cmp(a, b, &comparable); return comparable ? (cmp >= 0) : FALSE; }
/* rb_cmpint-checked comparison: an incomparable pair (nil `<=>`) raises the
   Comparable ArgumentError. Backs the object <,<=,>,>=,between? emitters when
   the user `<=>` can return nil (a TY_INT `<=>` keeps the inline fast path). */
static mrb_int sp_poly_cmp_ck(sp_RbVal a, sp_RbVal b) __attribute__((unused));
static mrb_int sp_poly_cmp_ck(sp_RbVal a, sp_RbVal b) {
  mrb_bool ok = FALSE; mrb_int r = sp_poly_cmp(a, b, &ok);
  if (!ok) sp_raise_cls("ArgumentError", sp_sprintf("comparison of %s with %s failed", sp_poly_class_name(a), sp_poly_class_name(b)));
  return r;
}
/* Comparable#==: identity is equal; a nil `<=>` makes it false, never an
   error (CRuby cmp_equal). */
static mrb_bool sp_poly_cmp_eq(sp_RbVal a, sp_RbVal b) __attribute__((unused));
static mrb_bool sp_poly_cmp_eq(sp_RbVal a, sp_RbVal b) {
  if (a.tag == b.tag && a.v.p == b.v.p) return TRUE;
  mrb_bool ok = FALSE; mrb_int r = sp_poly_cmp(a, b, &ok);
  return ok ? (r == 0) : FALSE;
}
/* Comparable#clamp(lo, hi) for user objects, mirroring CRuby cmp_clamp: a nil
   bound clamps one-sided; both bounds present and lo > hi (or incomparable)
   raise ArgumentError; the result is the receiver or the applied bound. The
   user `<=>` dispatches through sp_obj_cmp_hook (via sp_poly_cmp). */
static sp_RbVal sp_obj_clamp(sp_RbVal v, sp_RbVal lo, sp_RbVal hi) __attribute__((unused));
static sp_RbVal sp_obj_clamp(sp_RbVal v, sp_RbVal lo, sp_RbVal hi) {
  /* Each operand can be a heap object and sp_poly_cmp_ck dispatches the user
     `<=>` (which allocates), so root all three -- v is live but unused across
     the first lo<=>hi comparison, lo/hi across the later ones. */
  SP_GC_ROOT_RBVAL(v); SP_GC_ROOT_RBVAL(lo); SP_GC_ROOT_RBVAL(hi);
  if (lo.tag != SP_TAG_NIL && hi.tag != SP_TAG_NIL && sp_poly_cmp_ck(lo, hi) > 0)
    sp_raise_cls("ArgumentError", "min argument must be less than or equal to max argument");
  if (lo.tag != SP_TAG_NIL) {
    mrb_int c1 = sp_poly_cmp_ck(v, lo);
    if (c1 == 0) return v;
    if (c1 < 0) return lo;
  }
  if (hi.tag != SP_TAG_NIL && sp_poly_cmp_ck(v, hi) > 0) return hi;
  return v;
}
/* Comparable#clamp(range) for user objects: an exclusive range with a real
   end cannot clamp (CRuby); beginless/endless endpoints (the INTPTR_MIN/MAX
   range sentinels) clamp one-sided as nil bounds. Integer endpoints are boxed
   and flow to the user `<=>` like any operand. */
static sp_RbVal sp_obj_clamp_range(sp_RbVal v, sp_Range r) __attribute__((unused));
static sp_RbVal sp_obj_clamp_range(sp_RbVal v, sp_Range r) {
  if (r.excl && r.last != INTPTR_MAX)
    sp_raise_cls("ArgumentError", "cannot clamp with an exclusive range");
  sp_RbVal lo = r.first == INTPTR_MIN ? sp_box_nil() : sp_box_int(r.first);
  sp_RbVal hi = r.last == INTPTR_MAX ? sp_box_nil() : sp_box_int(r.last);
  return sp_obj_clamp(v, lo, hi);
}
/* Stable ascending sort of idx[0..n) by the poly key keys[idx[k]], leaving equal
   (or incomparable) keys in their original relative order. Backs sort_by's
   Schwartzian lowering: keys are precomputed once per element, so the comparator
   never re-runs the block. Bottom-up merge sort -- stable by construction, O(n log
   n), and (unlike qsort) carries the key array without a non-portable qsort_r. */
static void sp_sort_idx_by_poly(mrb_int *idx, const sp_RbVal *keys, mrb_int n) {
  if (n < 2) return;
  mrb_int *tmp = (mrb_int *)malloc(sizeof(mrb_int) * (size_t)n);
  if (!tmp) sp_oom_die();
  mrb_int *src = idx, *dst = tmp;
  for (mrb_int width = 1; width < n; width *= 2) {
    for (mrb_int lo = 0; lo < n; lo += 2 * width) {
      mrb_int mid = lo + width < n ? lo + width : n;
      mrb_int hi = lo + 2 * width < n ? lo + 2 * width : n;
      mrb_int i = lo, j = mid, k = lo;
      while (i < mid && j < hi) {
        mrb_bool ok; mrb_int c = sp_poly_cmp(keys[src[i]], keys[src[j]], &ok);
        if (!ok || c <= 0) dst[k++] = src[i++];   /* left wins ties -> stable */
        else dst[k++] = src[j++];
      }
      while (i < mid) dst[k++] = src[i++];
      while (j < hi) dst[k++] = src[j++];
    }
    mrb_int *t = src; src = dst; dst = t;   /* ping-pong buffers; no per-level copy */
  }
  if (src != idx) for (mrb_int x = 0; x < n; x++) idx[x] = src[x];   /* odd #levels: result is in tmp */
  free(tmp);
}
static sp_RbVal sp_poly_div(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) return sp_box_float(sp_poly_to_f(a) / sp_poly_to_f(b)); return sp_box_int(sp_idiv(sp_poly_to_i(a), sp_poly_to_i(b))); }
static sp_RbVal sp_poly_mod(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_FLT || b.tag == SP_TAG_FLT) return sp_box_float(sp_fmod(sp_poly_to_f(a), sp_poly_to_f(b))); return sp_box_int(sp_imod(sp_poly_to_i(a), sp_poly_to_i(b))); }  /* sp_fmod: CRuby divisor-sign result + zero-divisor raise */
/* Comparable#clamp on boxed numerics, faithful to CRuby: the result is the
   applied operand returned UNCHANGED, so an in-range Integer receiver stays
   Integer while a Float bound that clamps stays Float (5.clamp(1.0, 3.0) is
   3.0 but 5.clamp(1.0, 10.0) is 5). The lo<=>hi then self<=>bound checks
   mirror CRuby's NaN/ordering ArgumentErrors. The operand class and value in
   the message go through the generic sp_poly_class_name / sp_poly_to_s helpers
   (as the sort/min/max comparison errors already do), so a non-numeric poly
   bound names its real class and renders its value rather than reinterpreting
   the union as an integer. */
static sp_RbVal sp_num_clamp(sp_RbVal v, sp_RbVal lo, sp_RbVal hi) {
  mrb_float dv = sp_poly_to_f(v), dlo = sp_poly_to_f(lo), dhi = sp_poly_to_f(hi);
  if (dlo != dlo || dhi != dhi)
    sp_raise_cls("ArgumentError", sp_sprintf("comparison of %s with %s failed", sp_poly_class_name(lo), sp_poly_to_s(hi)));
  if (dlo > dhi)
    sp_raise_cls("ArgumentError", "min argument must be less than or equal to max argument");
  if (dv != dv)
    sp_raise_cls("ArgumentError", sp_sprintf("comparison of %s with %s failed", sp_poly_class_name(v), sp_poly_to_s(lo)));
  if (dv < dlo) return lo;
  if (dv > dhi) return hi;
  return v;
}
/* clamp on a boxed value: numerics route through sp_num_clamp so the returned
   operand keeps its own Integer/Float class; a user object anywhere in the
   triple routes through sp_obj_clamp (the user `<=>` via the cmp hook) instead
   of being reinterpreted as a float. */
static sp_RbVal sp_poly_clamp(sp_RbVal v, sp_RbVal lo, sp_RbVal hi) {
  if ((v.tag == SP_TAG_OBJ && !sp_poly_numeric_p(v)) ||
      (lo.tag == SP_TAG_OBJ && !sp_poly_numeric_p(lo)) ||
      (hi.tag == SP_TAG_OBJ && !sp_poly_numeric_p(hi)))
    return sp_obj_clamp(v, lo, hi);
  return sp_num_clamp(v, lo, hi);
}
/* clamp(range) on a boxed value: an exclusive range with a real end cannot
   clamp (CRuby); the INTPTR_MIN/MAX beginless/endless sentinels act as
   unbounded sides for numerics and nil bounds for user objects. */
static sp_RbVal sp_poly_clamp_range(sp_RbVal v, sp_Range r) __attribute__((unused));
static sp_RbVal sp_poly_clamp_range(sp_RbVal v, sp_Range r) {
  if (r.excl && r.last != INTPTR_MAX)
    sp_raise_cls("ArgumentError", "cannot clamp with an exclusive range");
  if (v.tag == SP_TAG_OBJ && !sp_poly_numeric_p(v)) return sp_obj_clamp_range(v, r);
  return sp_num_clamp(v, sp_box_int(r.first), sp_box_int(r.last));
}
/* Integer #** : Spinel has no Rational, so a negative integer exponent --
   which CRuby evaluates to a Rational like (1/2) -- raises RangeError rather
   than silently truncating toward 0. Float ** negative stays a float
   (CRuby-compatible: 2.0 ** -1 == 0.5). See docs/limitations.md. */
static mrb_int sp_int_pow(mrb_int base, mrb_int exp) __attribute__((unused));
static mrb_int sp_int_pow(mrb_int base, mrb_int exp) { if (exp < 0) sp_raise_cls("RangeError", "negative exponent"); return (mrb_int)pow((double)base, (double)exp); }
static sp_RbVal sp_poly_pow(sp_RbVal a, sp_RbVal b) { if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT && b.v.i < 0) sp_raise_cls("RangeError", "negative exponent"); double r = pow((double)sp_poly_to_f(a), (double)sp_poly_to_f(b)); if (a.tag == SP_TAG_INT && b.tag == SP_TAG_INT && b.v.i >= 0) return sp_box_int((mrb_int)r); return sp_box_float((mrb_float)r); }
/* sp_poly_shl is defined after sp_PolyArray_push (below) so the
   push-dispatch path can call it directly. The Integer-bit-shift
   semantics fall through when the recv isn't an array. */
static sp_RbVal sp_poly_shl(sp_RbVal a, sp_RbVal b);
static sp_RbVal sp_poly_shr(sp_RbVal a, sp_RbVal b) { return sp_box_int(sp_poly_to_i(a) >> sp_poly_to_i(b)); }
static sp_RbVal sp_poly_band(sp_RbVal a, sp_RbVal b) { return sp_box_int(sp_poly_to_i(a) & sp_poly_to_i(b)); }
static sp_RbVal sp_poly_bor(sp_RbVal a, sp_RbVal b) { return sp_box_int(sp_poly_to_i(a) | sp_poly_to_i(b)); }
static sp_RbVal sp_poly_bxor(sp_RbVal a, sp_RbVal b) { return sp_box_int(sp_poly_to_i(a) ^ sp_poly_to_i(b)); }
static sp_RbVal sp_poly_neg(sp_RbVal a) { if (a.tag == SP_TAG_FLT) return sp_box_float(-a.v.f); return sp_box_int(-sp_poly_to_i(a)); }

/* sp_mark_rbval: inline helper in sp_gc.h. */
/* Definition of the root-entry marker forward-declared near
   sp_gc_mark_all: a low-bit-tagged slot is an sp_RbVal* root. */
/* sp_gc_mark_root_entry is an inline helper in sp_gc.h. */
static sp_RbVal sp_PolyArray_pop(sp_PolyArray *a) { if (!a || a->len <= 0) return sp_box_nil(); if (a->frozen) { sp_raise_frozen_array(); return sp_box_nil(); } return a->data[--a->len]; }
/* log(|Gamma(x)|) for x > 0, via the Stirling asymptotic series pushed into
   its accurate region (x >= 12) by the recurrence Gamma(x) = Gamma(x+1)/x. We
   compute it ourselves rather than calling the platform `lgamma_r`, whose
   last-ULP result varies between libm implementations (macOS vs glibc) and
   would make the golden test output machine-specific. The Bernoulli series
   below carries it to ~1e-15. Gamma(1) and Gamma(2) are exactly 1, so their
   logs are returned as an exact 0 rather than a rounding-noise residual. */
static double sp_lgamma_pos(double x) {  /* x > 0 */
  if (x == 1.0 || x == 2.0) return 0.0;
  double corr = 0.0;
  while (x < 12.0) { corr -= log(x); x += 1.0; }
  double inv = 1.0 / x, inv2 = inv * inv;
  /* sum_{k>=1} B_2k / (2k(2k-1) x^(2k-1)) up to the 1/x^11 term */
  double series = (1.0/12.0) + (inv2 * (-(1.0/360.0) + (inv2 * ((1.0/1260.0)
                  + (inv2 * (-(1.0/1680.0) + (inv2 * (1.0/1188.0))))))));
  return corr + ((x - 0.5) * log(x)) - x + (0.5 * log(2.0 * M_PI)) + (series * inv);
}
/* Math.lgamma(x) -> [log(|gamma(x)|), sign of gamma(x)]. */
static sp_PolyArray *sp_math_lgamma(double x) {
  int sign = 1; double v;
  if (x > 0.0) {
    v = sp_lgamma_pos(x);
  }
  else {
    double s = sin(M_PI * x);
    if (s == 0.0) v = INFINITY;            /* pole at a non-positive integer */
    else { if (s < 0.0) sign = -1; v = log(M_PI / fabs(s)) - sp_lgamma_pos(1.0 - x); }
  }
  sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r);
  sp_PolyArray_push(r, sp_box_float(v));
  sp_PolyArray_push(r, sp_box_int(sign));
  return r;
}
static sp_RbVal sp_PolyArray_shift(sp_PolyArray *a) { if (!a || a->len <= 0) return sp_box_nil(); if (a->frozen) { sp_raise_frozen_array(); return sp_box_nil(); } sp_RbVal v = a->data[0]; memmove(a->data, a->data+1, (size_t)(--a->len)*sizeof(sp_RbVal)); return v; }
static sp_RbVal sp_PolyArray_delete_at(sp_PolyArray *a, mrb_int i) { if (!a) return sp_box_nil(); if (i < 0) i += a->len; if (i < 0 || i >= a->len) return sp_box_nil(); sp_RbVal v = a->data[i]; for (mrb_int j = i; j < a->len - 1; j++) a->data[j] = a->data[j+1]; a->len--; return v; }
/* Array#delete(v): removes every element sp_poly_eq to v, returns v (or
   nil if not found). Was missing for TY_POLY_ARRAY -- only TY_INT_ARRAY/
   TY_STR_ARRAY had it -- which blocked the array-backed Set package's
   #delete (doom's `@secret_sectors.delete(sector_idx)`). Lives here (not
   sp_array.c, home of sp_IntArray_delete et al) because it needs
   sp_poly_eq, which is inline-per-TU in this file, not linkable from the
   separately-compiled cold array library. */
static sp_RbVal sp_PolyArray_delete(sp_PolyArray *a, sp_RbVal v) {
  if (a && a->frozen) { sp_raise_frozen_array(); return sp_box_nil(); }
  if (!a) return sp_box_nil();
  /* sp_poly_eq can allocate (bigint promotion) and so trigger a collection
     mid-loop; a and v may be reachable only through the call expression. */
  SP_GC_ROOT(a); SP_GC_ROOT_RBVAL(v);
  mrb_int w = 0;
  mrb_bool found = FALSE;
  for (mrb_int i = 0; i < a->len; i++) {
    if (!sp_poly_eq(a->data[i], v)) { a->data[w] = a->data[i]; w++; }
    else found = TRUE;
  }
  a->len = w;
  return found ? v : sp_box_nil();
}

/* FFI array hand-off. Concrete arrays expose their element storage zero-copy
   (mrb_int/mrb_float are int64/double on 64-bit targets). A poly_array can't
   be punned -- its ->data is sp_RbVal[] (boxed) -- so unbox element-wise into
   a fresh GC-tracked buffer (sp_gc_alloc_nogc: no collection mid-build, so a
   sibling array arg's buffer can't be swept; freed at a later GC). */
static const int64_t *sp_PolyArray_ffi_int_data(sp_PolyArray *a) {
  if (!a || a->len <= 0) return (const int64_t *)0;
  int64_t *buf = (int64_t *)sp_gc_alloc_nogc((size_t)a->len * sizeof(int64_t), NULL, NULL);
  for (mrb_int i = 0; i < a->len; i++) buf[i] = (int64_t)a->data[i].v.i;
  return buf;
}
static const double *sp_PolyArray_ffi_float_data(sp_PolyArray *a) {
  if (!a || a->len <= 0) return (const double *)0;
  double *buf = (double *)sp_gc_alloc_nogc((size_t)a->len * sizeof(double), NULL, NULL);
  for (mrb_int i = 0; i < a->len; i++) buf[i] = (double)a->data[i].v.f;
  return buf;
}
/* MatchData — holds the source string and the per-group byte offsets
   the engine produced. `[]`/captures extract substrings on demand;
   offset/begin/end report CHARACTER offsets (CRuby semantics), so
   byte offsets are converted via sp_str_count_chars. Group i occupies
   caps[2i] (begin) and caps[2i+1] (end); -1 marks a non-participating
   group. Issue #974. */

static sp_RbVal sp_poly_shl(sp_RbVal a, sp_RbVal b) {
  /* Dispatch by recv cls_id: an IntArray / PolyArray / etc. boxed
     into a poly slot still wants Array#<< (push), not Integer#<<
     (bit-shift). Falls through to bit-shift only when the recv is
     a non-array. Returns the recv (matching `<<`s chainability). */
  if (a.tag == SP_TAG_OBJ) {
    if (a.cls_id == SP_BUILTIN_INT_ARRAY) {
      sp_IntArray_push((sp_IntArray *)a.v.p, b.tag == SP_TAG_INT ? b.v.i : sp_poly_to_i(b));
      return a;
    }
    if (a.cls_id == SP_BUILTIN_POLY_ARRAY) {
      sp_PolyArray_push((sp_PolyArray *)a.v.p, b);
      return a;
    }
    if (a.cls_id == SP_BUILTIN_PTR_ARRAY) {
      sp_PtrArray_push((sp_PtrArray *)a.v.p, b.v.p);
      return a;
    }
    if (a.cls_id == SP_BUILTIN_FLT_ARRAY) {
      sp_FloatArray_push((sp_FloatArray *)a.v.p, b.tag == SP_TAG_FLT ? b.v.f : (mrb_float)sp_poly_to_i(b));
      return a;
    }
    if (a.cls_id == SP_BUILTIN_STR_ARRAY) {
      sp_StrArray_push((sp_StrArray *)a.v.p, b.tag == SP_TAG_STR ? (const char *)b.v.p : sp_str_empty);
      return a;
    }
  }
  /* String#<< appends (sp_str_concat treats NULL as the empty string) */
  if (a.tag == SP_TAG_STR && b.tag == SP_TAG_STR)
    return sp_box_str(sp_str_concat(a.v.s, b.v.s));
  return sp_box_int(sp_poly_to_i(a) << sp_poly_to_i(b));
}
static mrb_int sp_PolyArray_length(sp_PolyArray *a) { if (!a) return 0; return a->len; }
/* Helpers for iterating over a poly value that holds a boxed array. */
static mrb_int sp_poly_arr_len(sp_RbVal a) {
  if (a.tag != SP_TAG_OBJ) return 0;
  switch (a.cls_id) {
    case SP_BUILTIN_INT_ARRAY: return ((sp_IntArray *)a.v.p)->len;
    case SP_BUILTIN_FLT_ARRAY: return ((sp_FloatArray *)a.v.p)->len;
    case SP_BUILTIN_STR_ARRAY: return ((sp_StrArray *)a.v.p)->len;
    case SP_BUILTIN_POLY_ARRAY: return ((sp_PolyArray *)a.v.p)->len;
    case SP_BUILTIN_STR_INT_HASH: return ((sp_StrIntHash *)a.v.p)->len;
    case SP_BUILTIN_STR_STR_HASH: return ((sp_StrStrHash *)a.v.p)->len;
    case SP_BUILTIN_INT_STR_HASH: return ((sp_IntStrHash *)a.v.p)->len;
    default: return 0;
  }
}
/* `when *arr`: does any element of arr match the scrutinee? Value equality
   via sp_poly_eq (the splat form is used with value lists; Class/Regexp
   elements inside a splat are not dispatched through #=== here). */
static mrb_bool sp_case_splat_match(sp_RbVal scrut, sp_RbVal arr) {
  mrb_int n = sp_poly_length(arr);
  for (mrb_int i = 0; i < n; i++)
    if (sp_poly_eq(scrut, sp_poly_arr_get(arr, i))) return TRUE;
  return FALSE;
}
/* `break *x` / `next *x`: Ruby's splat-to-array -- nil becomes [], an array
   stays itself, any other value wraps in a one-element array. */
static sp_RbVal sp_splat_to_array(sp_RbVal v) {
  if (v.tag == SP_TAG_NIL) return sp_box_poly_array(sp_PolyArray_new());
  if (v.tag == SP_TAG_OBJ && sp_poly_is_array_kind(v.cls_id)) return v;
  { sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r); sp_PolyArray_push(r, v); return sp_box_poly_array(r); }
}
static sp_RbVal sp_poly_arr_get(sp_RbVal a, mrb_int i) {
  if (a.tag != SP_TAG_OBJ) return sp_box_nil();
  switch (a.cls_id) {
    case SP_BUILTIN_INT_ARRAY: { sp_IntArray *ar=(sp_IntArray*)a.v.p; if(!ar||i<0||i>=ar->len) return sp_box_nil(); return sp_box_int(ar->data[ar->start+i]); }
    case SP_BUILTIN_SYM_ARRAY: { sp_IntArray *ar=(sp_IntArray*)a.v.p; if(!ar||i<0||i>=ar->len) return sp_box_nil(); return sp_box_sym((sp_sym)ar->data[ar->start+i]); }
    case SP_BUILTIN_FLT_ARRAY: { sp_FloatArray *ar=(sp_FloatArray*)a.v.p; if(!ar||i<0||i>=ar->len) return sp_box_nil(); return sp_box_float(ar->data[i]); }
    case SP_BUILTIN_STR_ARRAY: { sp_StrArray *ar=(sp_StrArray*)a.v.p; if(!ar||i<0||i>=ar->len) return sp_box_nil(); return sp_box_str(ar->data[i]); }
    case SP_BUILTIN_POLY_ARRAY: { sp_PolyArray *ar=(sp_PolyArray*)a.v.p; if(!ar||i<0||i>=ar->len) return sp_box_nil(); return ar->data[i]; }
    default: return sp_box_nil();
  }
}
/* Coerce a poly value that holds an array (any builtin array kind) into an
   sp_PolyArray of boxed elements. A poly-array value is returned as-is; nil or a
   non-array yields an empty array. Lets block methods (flat_map/each/...) run on
   a poly receiver whose array-ness is only known at runtime. */
static sp_PolyArray *sp_poly_to_poly_array(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_POLY_ARRAY) return (sp_PolyArray *)v.v.p;
  sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r);
  mrb_int n = sp_poly_arr_len(v);
  for (mrb_int i = 0; i < n; i++) sp_PolyArray_push(r, sp_poly_arr_get(v, i));
  return r;
}
/* Array#<=> across any pair of builtin array kinds: lexicographic element-wise
   compare via sp_poly_cmp, breaking ties on length. `*comparable` is cleared
   when an element pair is not mutually comparable (CRuby yields nil there). */
static mrb_int sp_poly_arr_cmp(sp_RbVal a, sp_RbVal b, mrb_bool *comparable) {
  /* Same object compares equal in O(1); this also terminates self-referential
     arrays (a contains a), which would otherwise recurse without bound. */
  if (a.v.p == b.v.p) { *comparable = TRUE; return 0; }
  mrb_int la = sp_poly_arr_len(a), lb = sp_poly_arr_len(b);
  mrb_int n = la < lb ? la : lb;
  for (mrb_int i = 0; i < n; i++) {
    mrb_bool ec; mrb_int r = sp_poly_cmp(sp_poly_arr_get(a, i), sp_poly_arr_get(b, i), &ec);
    if (!ec) { *comparable = FALSE; return 0; }
    if (r != 0) { *comparable = TRUE; return r < 0 ? -1 : 1; }
  }
  *comparable = TRUE;
  return (la > lb) - (la < lb);
}
/* Kernel#Array(x): nil -> []; an array -> its elements; anything else -> [x].
   Array-typed arguments are passed through directly in codegen, so this runs
   for nil/scalars and for a poly value that may hold an array at run time. */
static sp_PolyArray *sp_kernel_array(sp_RbVal x) {
  if (x.tag == SP_TAG_NIL) return sp_PolyArray_new();
  if (x.tag == SP_TAG_OBJ) {
    /* An existing poly array is returned as-is, preserving object identity as
       CRuby's Array(arr) does. A typed array cannot be returned directly (the
       result is a poly array), so its elements are copied. */
    if (x.cls_id == SP_BUILTIN_POLY_ARRAY) return (sp_PolyArray *)x.v.p;
    if (x.cls_id == SP_BUILTIN_INT_ARRAY || x.cls_id == SP_BUILTIN_STR_ARRAY ||
        x.cls_id == SP_BUILTIN_FLT_ARRAY) {
      sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r);
      mrb_int n = sp_poly_arr_len(x);
      for (mrb_int i = 0; i < n; i++) sp_PolyArray_push(r, sp_poly_arr_get(x, i));
      return r;
    }
  }
  sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r);
  sp_PolyArray_push(r, x);
  return r;
}
/* Issues #770, #789: NULL + bounds guard. Out-of-range set no-ops. */
static void sp_PolyArray_set(sp_PolyArray *a, mrb_int i, sp_RbVal v) { if (!a) return; if (a->frozen) { sp_raise_frozen_array(); return; } mrb_int orig=i; if (i < 0) i += a->len; if (i < 0) sp_raise_cls("IndexError", sp_sprintf("index %lld too small for array; minimum: %lld",(long long)orig,(long long)-a->len)); if (i >= a->len) return; a->data[i] = v; }
static sp_PolyArray *sp_PolyArray_slice(sp_PolyArray *a, mrb_int start, mrb_int len) { SP_GC_ROOT(a); if (start < 0) start += a->len; if (start < 0) start = 0; sp_PolyArray *b = sp_PolyArray_new(); if (start >= a->len || len <= 0) return b; if (start + len > a->len) len = a->len - start; for (mrb_int i = 0; i < len; i++) sp_PolyArray_push(b, a->data[start + i]); return b; }
static sp_PolyArray *sp_PolyArray_slice_range(sp_PolyArray *a, mrb_int start, mrb_int end_, mrb_int excl) { if (end_ < 0) end_ += a->len; if (start < 0) start += a->len; mrb_int n = end_ - start + (excl ? 0 : 1); if (n < 0 || start < 0) n = 0; return sp_PolyArray_slice(a, start, n); }
/* 2-arg slice on a poly receiver: dispatch to the typed slice functions. */
typedef struct sp_BoundMethod { void *self; mrb_int fn; const char *name; } sp_BoundMethod;
static sp_RbVal sp_poly_slice(sp_RbVal a, mrb_int start, mrb_int len) {
  if (a.tag == SP_TAG_STR) return sp_box_str(sp_str_sub_range(a.v.s ? a.v.s : "", start, len));
  if (a.tag != SP_TAG_OBJ) return sp_box_nil();
  /* bm[a, b]: a boxed bound Method called with two int arguments (optcarrot's
     store dispatch table: `@store[addr][addr, value]`). */
  if (a.cls_id == SP_BUILTIN_METHOD) {
    sp_BoundMethod *m = (sp_BoundMethod *)a.v.p;
    return sp_box_int(((mrb_int (*)(void *, mrb_int, mrb_int))(uintptr_t)m->fn)((void *)m->self, start, len));
  }
  switch (a.cls_id) {
    case SP_BUILTIN_INT_ARRAY:  return sp_box_int_array(sp_IntArray_slice((sp_IntArray*)a.v.p, start, len));
    case SP_BUILTIN_FLT_ARRAY:  return sp_box_float_array(sp_FloatArray_slice((sp_FloatArray*)a.v.p, start, len));
    case SP_BUILTIN_STR_ARRAY:  return sp_box_str_array(sp_StrArray_slice((sp_StrArray*)a.v.p, start, len));
    case SP_BUILTIN_POLY_ARRAY: return sp_box_poly_array(sp_PolyArray_slice((sp_PolyArray*)a.v.p, start, len));
    default: return sp_box_nil();
  }
}
/* True when a boxed value is one of the builtin array kinds. */
static int sp_rbval_is_array(sp_RbVal v) {
  return v.tag == SP_TAG_OBJ &&
    (v.cls_id == SP_BUILTIN_INT_ARRAY || v.cls_id == SP_BUILTIN_FLT_ARRAY ||
     v.cls_id == SP_BUILTIN_STR_ARRAY || v.cls_id == SP_BUILTIN_POLY_ARRAY);
}
/* Frozen flag of a builtin array, matching what sp_*Array_splice check (the
   struct field, which the promote path would otherwise bypass by building a new
   array, and which lets us check frozen up front before any GC root is live). */
static int sp_typed_arr_frozen(sp_RbVal v) {
  switch (v.cls_id) {
    case SP_BUILTIN_INT_ARRAY: return ((sp_IntArray *)v.v.p)->frozen;
    case SP_BUILTIN_FLT_ARRAY: return ((sp_FloatArray *)v.v.p)->frozen;
    case SP_BUILTIN_STR_ARRAY: return ((sp_StrArray *)v.v.p)->frozen;
    case SP_BUILTIN_POLY_ARRAY: return ((sp_PolyArray *)v.v.p)->frozen;
    default: return 0;
  }
}
/* 3-arg []= on a poly receiver whose runtime object is a builtin array. Matches
   CRuby: a POLY_ARRAY splices directly (sp_PolyArray_splice already inserts a
   nil/scalar src as one element, splats an array src, and nil-fills a gap past
   the end). A typed array stays typed only when the result provably remains
   homogeneous -- an empty ([]) src, a same-kind array, or a matching scalar, AND
   no nil-fill (start <= len). Otherwise the array is promoted to a poly array
   (boxing its elements) and spliced there. Returns the possibly-new boxed array
   so the caller stores it back into the receiver's slot. */
static sp_RbVal sp_poly_splice(sp_RbVal recv, mrb_int start, mrb_int len, sp_RbVal src) {
  if (recv.tag != SP_TAG_OBJ) return recv;
  mrb_int alen = sp_poly_arr_len(recv);
  mrb_int s = start < 0 ? start + alen : start;
  /* Validate frozen/length/index UP FRONT -- before any delegate roots the
     array -- so a raise never longjmps with a GC root live (an inline rescue
     does not restore sp_gc_nroots, so such a root would dangle). The delegates
     re-check the same conditions but, being pre-satisfied, never raise. The
     order matches CRuby: modify-check first, then negative length, then the
     too-small index. */
  if (sp_typed_arr_frozen(recv)) sp_raise_frozen_array();
  if (len < 0) sp_raise_cls("IndexError", sp_sprintf("negative length (%lld)", (long long)len));
  if (s < 0) sp_raise_cls("IndexError",
                          sp_sprintf("index %lld too small for array; minimum: %lld", (long long)start, (long long)-alen));
  if (recv.cls_id == SP_BUILTIN_POLY_ARRAY) {
    sp_PolyArray_splice((sp_PolyArray *)recv.v.p, start, len, src);
    return recv;
  }
  int nofill = s <= alen;   /* a start past the end needs a nil-fill -> promote */
  int is_empty_arr = sp_rbval_is_array(src) && sp_poly_arr_len(src) == 0;
  switch (recv.cls_id) {
    case SP_BUILTIN_INT_ARRAY: {
      sp_IntArray *a = (sp_IntArray *)recv.v.p;
      if (nofill && is_empty_arr) { sp_IntArray_splice(a, start, len, NULL, 0); return recv; }
      if (nofill && src.tag == SP_TAG_OBJ && src.cls_id == SP_BUILTIN_INT_ARRAY) {
        sp_IntArray *sa = (sp_IntArray *)src.v.p;
        sp_IntArray_splice(a, start, len, sa->data + sa->start, sa->len); return recv;
      }
      if (nofill && src.tag == SP_TAG_INT) { mrb_int v = src.v.i; sp_IntArray_splice(a, start, len, &v, 1); return recv; }
      break;
    }
    case SP_BUILTIN_FLT_ARRAY: {
      sp_FloatArray *a = (sp_FloatArray *)recv.v.p;
      if (nofill && is_empty_arr) { sp_FloatArray_splice(a, start, len, NULL, 0); return recv; }
      if (nofill && src.tag == SP_TAG_OBJ && src.cls_id == SP_BUILTIN_FLT_ARRAY) {
        sp_FloatArray *sa = (sp_FloatArray *)src.v.p;
        sp_FloatArray_splice(a, start, len, sa->data, sa->len); return recv;
      }
      if (nofill && src.tag == SP_TAG_FLT) { mrb_float v = src.v.f; sp_FloatArray_splice(a, start, len, &v, 1); return recv; }
      break;
    }
    case SP_BUILTIN_STR_ARRAY: {
      sp_StrArray *a = (sp_StrArray *)recv.v.p;
      if (nofill && is_empty_arr) { sp_StrArray_splice(a, start, len, NULL, 0); return recv; }
      if (nofill && src.tag == SP_TAG_OBJ && src.cls_id == SP_BUILTIN_STR_ARRAY) {
        sp_StrArray *sa = (sp_StrArray *)src.v.p;
        sp_StrArray_splice(a, start, len, sa->data, sa->len); return recv;
      }
      if (nofill && src.tag == SP_TAG_STR) { const char *v = src.v.s; sp_StrArray_splice(a, start, len, &v, 1); return recv; }
      break;
    }
    default: return recv;
  }
  /* promote to poly and splice there (handles nil / heterogeneous / nil-fill).
     Index/length/frozen were validated up front, so nothing below raises; the
     GC roots are pushed only around the actual allocation and pop normally. */
  SP_GC_ROOT_RBVAL(src);
  /* recv is read element-by-element inside the conversion's push loop, each of
     which can collect; a temporary receiver held by no rooted container would
     otherwise dangle mid-loop. */
  SP_GC_ROOT_RBVAL(recv);
  sp_PolyArray *p = sp_poly_to_poly_array(recv);
  SP_GC_ROOT(p);
  sp_PolyArray_splice(p, start, len, src);
  return sp_box_poly_array(p);
}
/* Render a Range for a RangeError message ("-10..1", "1...3", "-10..", "..2"). */
static const char *sp_range_str(sp_Range r) {
  const char *dots = r.excl ? "..." : "..";
  if (r.first == INTPTR_MIN && r.last == INTPTR_MAX) return dots;
  if (r.first == INTPTR_MIN) return sp_sprintf("%s%lld", dots, (long long)r.last);
  if (r.last == INTPTR_MAX)  return sp_sprintf("%lld%s", (long long)r.first, dots);
  return sp_sprintf("%lld%s%lld", (long long)r.first, dots, (long long)r.last);
}
/* `arr[range] = src` on a poly receiver: resolve beginless (INTPTR_MIN -> 0) and
   endless (INTPTR_MAX -> length) endpoints and negative endpoints against the
   runtime length, then splice. A begin index below -length raises RangeError
   (CRuby uses RangeError here, not the (start,len) form's IndexError). */
static sp_RbVal sp_poly_splice_range(sp_RbVal recv, sp_Range r, sp_RbVal src) {
  /* frozen precedes range validation (CRuby's modify-check runs first) */
  if (recv.tag == SP_TAG_OBJ && sp_typed_arr_frozen(recv)) sp_raise_frozen_array();
  mrb_int alen = sp_poly_arr_len(recv);
  mrb_int first = r.first;
  if (first == INTPTR_MIN) first = 0;      /* beginless */
  else if (first < 0) {
    if (first < -alen) sp_raise_cls("RangeError", sp_sprintf("%s out of range", sp_range_str(r)));
    first += alen;
  }
  mrb_int len;
  if (r.last == INTPTR_MAX) { len = alen - first; if (len < 0) len = 0; }  /* endless */
  else {
    mrb_int last = r.last < 0 ? r.last + alen : r.last;
    len = last - first + (r.excl ? 0 : 1);
    if (len < 0) len = 0;
  }
  return sp_poly_splice(recv, first, len, src);
}
/* Array#replace(other): replace recv's contents with other's, returning recv.
   recv keeps the same boxed pointer (the underlying array is mutated in place),
   so a nullable-array slot typed poly stays valid. A nil/non-array recv is a
   no-op (the call sites that reach a nil poly are dead-guarded in Ruby). */
static sp_RbVal sp_poly_replace(sp_RbVal recv, sp_RbVal src) {
  if (recv.tag != SP_TAG_OBJ || src.tag != SP_TAG_OBJ) return recv;
  if (recv.cls_id == SP_BUILTIN_INT_ARRAY && src.cls_id == SP_BUILTIN_INT_ARRAY)
    sp_IntArray_replace((sp_IntArray *)recv.v.p, (sp_IntArray *)src.v.p);
  else if (recv.cls_id == SP_BUILTIN_FLT_ARRAY && src.cls_id == SP_BUILTIN_FLT_ARRAY)
    sp_FloatArray_replace((sp_FloatArray *)recv.v.p, (sp_FloatArray *)src.v.p);
  else if (recv.cls_id == SP_BUILTIN_STR_ARRAY && src.cls_id == SP_BUILTIN_STR_ARRAY)
    sp_StrArray_replace((sp_StrArray *)recv.v.p, (sp_StrArray *)src.v.p);
  else if (recv.cls_id == SP_BUILTIN_POLY_ARRAY) {
    sp_PolyArray *d = (sp_PolyArray *)recv.v.p;
    d->len = 0;
    switch (src.cls_id) {
      case SP_BUILTIN_INT_ARRAY: { sp_IntArray *s = (sp_IntArray *)src.v.p; for (mrb_int i = 0; i < s->len; i++) sp_PolyArray_push(d, sp_box_int(s->data[s->start + i])); break; }
      case SP_BUILTIN_FLT_ARRAY: { sp_FloatArray *s = (sp_FloatArray *)src.v.p; for (mrb_int i = 0; i < s->len; i++) sp_PolyArray_push(d, sp_box_float(s->data[i])); break; }
      case SP_BUILTIN_STR_ARRAY: { sp_StrArray *s = (sp_StrArray *)src.v.p; for (mrb_int i = 0; i < s->len; i++) sp_PolyArray_push(d, sp_box_str(s->data[i])); break; }
      case SP_BUILTIN_POLY_ARRAY: { sp_PolyArray *s = (sp_PolyArray *)src.v.p; for (mrb_int i = 0; i < s->len; i++) sp_PolyArray_push(d, s->data[i]); break; }
      default: break;
    }
  }
  return recv;
}
static sp_PolyArray *sp_PolyArray_slice_bang(sp_PolyArray *a, mrb_int from, mrb_int n) {
  if (!a) return sp_PolyArray_new();
  if (a->frozen) { sp_raise_frozen_array(); return sp_PolyArray_new(); }
  if (from < 0) from += a->len;
  if (from < 0) from = 0;
  if (from > a->len) from = a->len;
  if (n < 0) n = 0;
  if (from + n > a->len) n = a->len - from;
  sp_PolyArray *r = sp_PolyArray_new();
  for (mrb_int i = 0; i < n; i++) sp_PolyArray_push(r, a->data[from + i]);
  for (mrb_int i = from; i + n < a->len; i++) a->data[i] = a->data[i + n];
  a->len -= n;
  return r;
}
static sp_PolyArray *sp_PolyArray_dup(sp_PolyArray *a) { SP_GC_ROOT(a); sp_PolyArray *b = sp_PolyArray_new(); for (mrb_int i = 0; i < a->len; i++) sp_PolyArray_push(b, a->data[i]); return b; }
static sp_PolyArray *sp_PolyArray_replace(sp_PolyArray *dst, sp_PolyArray *src) { if (!dst || !src) return dst; dst->len = 0; for (mrb_int i = 0; i < src->len; i++) sp_PolyArray_push(dst, src->data[i]); return dst; }
/* Array#+ : a fresh (unfrozen) array of a's then b's elements. */
static sp_PolyArray *sp_PolyArray_concat(sp_PolyArray *a, sp_PolyArray *b) { sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r); if (a) for (mrb_int i = 0; i < a->len; i++) sp_PolyArray_push(r, a->data[i]); if (b) for (mrb_int i = 0; i < b->len; i++) sp_PolyArray_push(r, b->data[i]); return r; }
static mrb_bool sp_PolyArray_include_val(sp_PolyArray *a, sp_RbVal v) { if (!a) return FALSE; for (mrb_int i = 0; i < a->len; i++) if (sp_poly_eq(a->data[i], v)) return TRUE; return FALSE; }
static sp_PolyArray *sp_PolyArray_intersect(sp_PolyArray *a, sp_PolyArray *b) { sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r); if (!a || !b) return r; for (mrb_int i = 0; i < a->len; i++) { sp_RbVal v = a->data[i]; if (sp_PolyArray_include_val(b, v) && !sp_PolyArray_include_val(r, v)) sp_PolyArray_push(r, v); } return r; }
/* intersect? predicate: early-exit, no allocation (matches CRuby's non-building Array#intersect?). */
static mrb_bool sp_PolyArray_intersect_p(sp_PolyArray *a, sp_PolyArray *b) { if (!a || !b) return 0; for (mrb_int i = 0; i < a->len; i++) if (sp_PolyArray_include_val(b, a->data[i])) return 1; return 0; }
static sp_PolyArray *sp_PolyArray_union(sp_PolyArray *a, sp_PolyArray *b) { sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r); if (a) for (mrb_int i = 0; i < a->len; i++) { sp_RbVal v = a->data[i]; if (!sp_PolyArray_include_val(r, v)) sp_PolyArray_push(r, v); } if (b) for (mrb_int i = 0; i < b->len; i++) { sp_RbVal v = b->data[i]; if (!sp_PolyArray_include_val(r, v)) sp_PolyArray_push(r, v); } return r; }
static sp_PolyArray *sp_PolyArray_difference(sp_PolyArray *a, sp_PolyArray *b) { sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r); if (!a) return r; for (mrb_int i = 0; i < a->len; i++) { sp_RbVal v = a->data[i]; if (!sp_PolyArray_include_val(b, v)) sp_PolyArray_push(r, v); } return r; }
/* Array#compact for poly_array: keep elements whose tag is not SP_TAG_NIL. */
static sp_PolyArray *sp_PolyArray_compact(sp_PolyArray *a) { SP_GC_ROOT(a); sp_PolyArray *b = sp_PolyArray_new(); SP_GC_ROOT(b); if (!a) return b; for (mrb_int i = 0; i < a->len; i++) { if (a->data[i].tag != SP_TAG_NIL) sp_PolyArray_push(b, a->data[i]); } return b; }
static sp_PolyArray *sp_PolyArray_compact_bang(sp_PolyArray *a) { if (!a) return a; mrb_int w = 0; for (mrb_int i = 0; i < a->len; i++) { if (a->data[i].tag != SP_TAG_NIL) a->data[w++] = a->data[i]; } a->len = w; return a; }
/* Issue #738: Hash#to_a as poly_array of [key, value] poly_array pairs. */
static sp_PolyArray*sp_StrIntHash_to_a(sp_StrIntHash*h){sp_PolyArray*r=sp_PolyArray_new();if(!h)return r;for(mrb_int i=0;i<h->len;i++){sp_PolyArray*p=sp_PolyArray_new();sp_PolyArray_push(p,sp_box_str(h->order[i]));sp_PolyArray_push(p,sp_box_int(sp_StrIntHash_get(h,h->order[i])));sp_PolyArray_push(r,sp_box_poly_array(p));}return r;}
static sp_PolyArray*sp_StrStrHash_to_a(sp_StrStrHash*h){sp_PolyArray*r=sp_PolyArray_new();if(!h)return r;for(mrb_int i=0;i<h->len;i++){sp_PolyArray*p=sp_PolyArray_new();sp_PolyArray_push(p,sp_box_str(h->order[i]));sp_PolyArray_push(p,sp_box_str(sp_StrStrHash_get(h,h->order[i])));sp_PolyArray_push(r,sp_box_poly_array(p));}return r;}
static sp_PolyArray*sp_IntStrHash_to_a(sp_IntStrHash*h){sp_PolyArray*r=sp_PolyArray_new();if(!h)return r;for(mrb_int i=0;i<h->len;i++){sp_PolyArray*p=sp_PolyArray_new();sp_PolyArray_push(p,sp_box_int(h->order[i]));sp_PolyArray_push(p,sp_box_str(sp_IntStrHash_get(h,h->order[i])));sp_PolyArray_push(r,sp_box_poly_array(p));}return r;}
/* Array#flatten -- walk into nested array values recursively. Each
   array-tagged element (IntArray / StrArray / SymArray / FloatArray /
   PolyArray) is expanded inline; scalars are appended as-is. Issue
   #739. */
static void sp_PolyArray_flatten_into(sp_PolyArray *dst, sp_RbVal v) {
  if (v.tag != SP_TAG_OBJ) { sp_PolyArray_push(dst, v); return; }
  if (v.cls_id == SP_BUILTIN_INT_ARRAY) { sp_IntArray *ia = (sp_IntArray *)v.v.p; for (mrb_int i = 0; i < ia->len; i++) sp_PolyArray_push(dst, sp_box_int(ia->data[ia->start + i])); return; }
  if (v.cls_id == SP_BUILTIN_STR_ARRAY) { sp_StrArray *sa = (sp_StrArray *)v.v.p; for (mrb_int i = 0; i < sa->len; i++) sp_PolyArray_push(dst, sp_box_str(sa->data[i])); return; }
  if (v.cls_id == SP_BUILTIN_SYM_ARRAY) { sp_IntArray *ya = (sp_IntArray *)v.v.p; for (mrb_int i = 0; i < ya->len; i++) sp_PolyArray_push(dst, sp_box_sym((sp_sym)ya->data[ya->start + i])); return; }
  if (v.cls_id == SP_BUILTIN_FLT_ARRAY) { sp_FloatArray *fa = (sp_FloatArray *)v.v.p; for (mrb_int i = 0; i < fa->len; i++) sp_PolyArray_push(dst, sp_box_float(fa->data[i])); return; }
  if (v.cls_id == SP_BUILTIN_POLY_ARRAY) { sp_PolyArray *pa = (sp_PolyArray *)v.v.p; for (mrb_int i = 0; i < pa->len; i++) sp_PolyArray_flatten_into(dst, pa->data[i]); return; }
  /* Other array variants fall through as opaque elements; rare for
     deep-flatten use cases. */
  sp_PolyArray_push(dst, v);
}
static sp_PolyArray *sp_PolyArray_flatten(sp_PolyArray *a) { SP_GC_ROOT(a); sp_PolyArray *b = sp_PolyArray_new(); SP_GC_ROOT(b); if (!a) return b; for (mrb_int i = 0; i < a->len; i++) sp_PolyArray_flatten_into(b, a->data[i]); return b; }

/* Box-into-poly converters used by the printf-with-array codegen
   (`"%fmt" % typed_array`). The format helper expects sp_RbVal
   slots so it can dispatch per-element. */

/* String#% with a poly_array argument. Walks the format and for
   each spec ("%s", "%d", "%f", "%x", "%o", etc.) pulls the next
   array element. Width / flag chars between `%` and the conv
   letter (`-+0 #`, digits, `.`) are copied verbatim so printf
   does the substitution work. */
/* Ruby-style %b / %B binary conversion (C printf has no binary conversion).
   Handles the -, +, space, #, 0 flags, width, and precision. A negative value
   uses Ruby's two's-complement ".." notation: the leading run of 1 bits is
   collapsed to a single 1 (e.g. -5 -> "..1011"), and precision/`#`/width apply
   around that body. Writes into out (osz bytes) and returns the length. */
static int sp_fmt_binary(const char *spec, size_t sl, char conv, long long val,
                         char *out, size_t osz) {
  /* parse "%<flags><width>.<prec>b" out of spec[0..sl-1] (spec[sl-1] == conv) */
  int f_minus = 0, f_plus = 0, f_space = 0, f_hash = 0, f_zero = 0;
  size_t i = 1;
  for (; i < sl; i++) {
    if (spec[i] == '-') f_minus = 1;
    else if (spec[i] == '+') f_plus = 1;
    else if (spec[i] == ' ') f_space = 1;
    else if (spec[i] == '#') f_hash = 1;
    else if (spec[i] == '0') f_zero = 1;
    else break;
  }
  int width = 0;
  for (; i < sl && spec[i] >= '0' && spec[i] <= '9'; i++) width = (width * 10) + (spec[i] - '0');
  int prec = -1;
  if (i < sl && spec[i] == '.') {
    i++; prec = 0;
    for (; i < sl && spec[i] >= '0' && spec[i] <= '9'; i++) prec = (prec * 10) + (spec[i] - '0');
  }

  int neg = val < 0;
  /* Ruby shows the two's-complement ".." body only for a negative value with no
     sign flag; a + or space flag switches to signed magnitude ("-101"). */
  int twos = neg && !f_plus && !f_space;
  char digits[256]; int dn = 0;
  if (twos) {
    unsigned long long uv = (unsigned long long)val;
    int p = -1;  /* highest 0-bit position; -1 means val == -1 (all ones) */
    for (int bit = 62; bit >= 0; bit--) if (!((uv >> bit) & 1ULL)) { p = bit; break; }
    int ndig = p + 2; if (ndig < 1) ndig = 1;
    for (int bit = ndig - 1; bit >= 0; bit--) digits[dn++] = (char)('0' + (int)((uv >> bit) & 1ULL));
  } else {
    /* signed magnitude: |val| in binary. 0 has no significant digits, so it
       contributes a single '0' only when precision is not 0. */
    unsigned long long mag = neg ? (unsigned long long)(-(val + 1)) + 1 : (unsigned long long)val;
    if (mag == 0) { if (prec != 0) digits[dn++] = '0'; }
    else { char t[80]; int tn = 0; while (mag) { t[tn++] = (char)('0' + (int)(mag & 1ULL)); mag >>= 1; }
           while (tn) digits[dn++] = t[--tn]; }
  }
  /* precision: minimum digit count. The ".." body counts as 2 toward it and pads
     with the sign bit (1); signed magnitude pads with 0. */
  if (prec >= 0) {
    int target = twos ? (prec - 2) : prec;
    char padc = twos ? '1' : '0';
    int t2 = target - dn;
    if (t2 > 0) {
      /* clamp to the digits buffer (output is capped at osz anyway) */
      if (t2 + dn >= (int)sizeof(digits)) t2 = (int)sizeof(digits) - dn - 1;
      memmove(digits + t2, digits, (size_t)dn);  /* shift body right */
      memset(digits, padc, (size_t)t2);           /* leading padding */
      dn += t2;
    }
    f_zero = 0;  /* precision disables 0-flag for integer conversions */
  }
  /* assemble sign/prefix + body, then apply width padding */
  char body[300]; int bn = 0;
  char sign = twos ? 0 : (neg ? '-' : (f_plus ? '+' : (f_space ? ' ' : 0)));
  char prefix0 = 0, prefix1 = 0;
  if (f_hash && val != 0) { prefix0 = '0'; prefix1 = (conv == 'B') ? 'B' : 'b'; }
  if (sign) body[bn++] = sign;
  if (prefix0) { body[bn++] = prefix0; body[bn++] = prefix1; }
  if (twos) { body[bn++] = '.'; body[bn++] = '.'; }
  for (int k = 0; k < dn; k++) body[bn++] = digits[k];

  int o = 0;
  int pad = width - bn;
  if (pad > 0 && !f_minus && f_zero) {
    /* zero-pad: emit sign/prefix/".." first, then fill, then the rest. A two's-
       complement body fills with the sign bit (1); signed magnitude with 0. */
    int head = (sign ? 1 : 0) + (prefix0 ? 2 : 0) + (twos ? 2 : 0);
    char fillc = twos ? '1' : '0';
    for (int k = 0; k < head && o < (int)osz; k++) out[o++] = body[k];
    for (int k = 0; k < pad && o < (int)osz; k++) out[o++] = fillc;
    for (int k = head; k < bn && o < (int)osz; k++) out[o++] = body[k];
  } else {
    if (pad > 0 && !f_minus) for (int k = 0; k < pad && o < (int)osz; k++) out[o++] = ' ';
    for (int k = 0; k < bn && o < (int)osz; k++) out[o++] = body[k];
    if (pad > 0 && f_minus) for (int k = 0; k < pad && o < (int)osz; k++) out[o++] = ' ';
  }
  return o;
}

static const char *sp_str_format_polyarr(const char *fmt, sp_PolyArray *a) {
  size_t cap = strlen(fmt) + 64;
  char *buf = (char *)malloc(cap);
  if (!buf) { perror("malloc"); exit(1); }
  size_t out = 0; mrb_int idx = 0; const char *p = fmt;
  while (*p) {
    if (*p != '%') {
      if (out + 1 >= cap) { cap = cap * 2; buf = (char *)realloc(buf, cap); }
      buf[out++] = *p++; continue;
    }
    if (p[1] == '%') {
      if (out + 1 >= cap) { cap = cap * 2; buf = (char *)realloc(buf, cap); }
      buf[out++] = '%'; p += 2; continue;
    }
    char spec[64]; size_t sl = 0; spec[sl++] = *p++;
    /* positional argument reference: %N$conv selects the Nth (1-based) arg */
    {
      const char *q = p; mrb_int argnum = 0; mrb_bool overflow = FALSE;
      while (*q >= '0' && *q <= '9') {
        if (sp_int_mul_overflow_p(argnum, 10, &argnum) ||
            sp_int_add_overflow_p(argnum, *q - '0', &argnum)) { overflow = TRUE; break; }
        q++;
      }
      if (!overflow && argnum > 0 && *q == '$') { idx = argnum - 1; p = q + 1; }
    }
    while (*p && sl < sizeof(spec) - 4) {
      char c = *p;
      if (c == '-' || c == '+' || c == ' ' || c == '#' || c == '0' || c == '.' || (c >= '0' && c <= '9')) { spec[sl++] = c; p++; }
      else break;
    }
    if (!*p) break;
    char conv = *p++; spec[sl++] = conv;
    char fmt_use[80];
    if (conv == 'd' || conv == 'i' || conv == 'x' || conv == 'X' || conv == 'o') {
      memcpy(fmt_use, spec, sl - 1);
      fmt_use[sl - 1] = 'l'; fmt_use[sl] = 'l'; fmt_use[sl + 1] = conv; fmt_use[sl + 2] = 0;
    }
else {
      memcpy(fmt_use, spec, sl); fmt_use[sl] = 0;
    }
    char tmp[256]; int wn = 0;
    sp_RbVal v = (idx < a->len) ? a->data[idx] : sp_box_nil();
    idx++;
    if (conv == 'd' || conv == 'i' || conv == 'x' || conv == 'X' || conv == 'o') {
      long long lv = 0;
      if (v.tag == SP_TAG_INT) lv = (long long)v.v.i;
      else if (v.tag == SP_TAG_FLT) lv = (long long)v.v.f;
      else if (v.tag == SP_TAG_STR && v.v.s) lv = strtoll(v.v.s, NULL, 10);
      wn = snprintf(tmp, sizeof(tmp), fmt_use, lv);
    }
else if (conv == 'b' || conv == 'B') {
      long long lv = 0;
      if (v.tag == SP_TAG_INT) lv = (long long)v.v.i;
      else if (v.tag == SP_TAG_FLT) lv = (long long)v.v.f;
      else if (v.tag == SP_TAG_STR && v.v.s) lv = strtoll(v.v.s, NULL, 10);
      wn = sp_fmt_binary(spec, sl, conv, lv, tmp, sizeof(tmp));
    }
else if (conv == 'f' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G') {
      double dv = 0;
      if (v.tag == SP_TAG_FLT) dv = v.v.f;
      else if (v.tag == SP_TAG_INT) dv = (double)v.v.i;
      wn = snprintf(tmp, sizeof(tmp), fmt_use, dv);
    }
else if (conv == 's') {
      const char *sv = "";
      char num_buf[32];
      if (v.tag == SP_TAG_STR) sv = v.v.s ? v.v.s : "";
      else if (v.tag == SP_TAG_INT) { snprintf(num_buf, sizeof(num_buf), "%lld", (long long)v.v.i); sv = num_buf; }
      else if (v.tag == SP_TAG_FLT) { snprintf(num_buf, sizeof(num_buf), "%g", v.v.f); sv = num_buf; }
      wn = snprintf(tmp, sizeof(tmp), fmt_use, sv);
    }
else if (conv == 'c') {
      int cv = 0;
      if (v.tag == SP_TAG_INT) cv = (int)v.v.i;
      else if (v.tag == SP_TAG_STR && v.v.s && v.v.s[0]) cv = (unsigned char)v.v.s[0];
      wn = snprintf(tmp, sizeof(tmp), fmt_use, cv);
    }
else {
      memcpy(tmp, spec, sl); tmp[sl] = 0; wn = (int)sl; idx--;
    }
    if (wn < 0) continue;
    if (out + (size_t)wn + 1 >= cap) { cap = ((out + wn) * 2) + 64; buf = (char *)realloc(buf, cap); }
    memcpy(buf + out, tmp, wn); out += wn;
  }
  buf[out] = 0;
  char *r = sp_str_alloc(out); memcpy(r, buf, out); free(buf); return r;
}

/* Element count of an array-kind value, or -1 if `el` is not an array (a
   non-object, a user object, a hash, etc.). Lets assoc/rassoc skip non-array
   and too-short pairs without indexing them, so a `nil` search key cannot
   spuriously match an out-of-bounds (nil) read. */
static mrb_int sp_array_kind_len(sp_RbVal el) {
  if (el.tag != SP_TAG_OBJ || !el.v.p) return -1;
  switch (el.cls_id) {
    case SP_BUILTIN_INT_ARRAY:
    case SP_BUILTIN_SYM_ARRAY:  return ((sp_IntArray *)el.v.p)->len;
    case SP_BUILTIN_FLT_ARRAY:  return ((sp_FloatArray *)el.v.p)->len;
    case SP_BUILTIN_STR_ARRAY:  return ((sp_StrArray *)el.v.p)->len;
    case SP_BUILTIN_POLY_ARRAY: return ((sp_PolyArray *)el.v.p)->len;
    default: return -1;
  }
}

/* Box any array-kind element into a PolyArray so assoc/rassoc can return it
   through their PolyArray* type regardless of the matched pair's own kind
   (a like-typed pair such as [1, 2] is an IntArray, not a PolyArray). */
static sp_PolyArray *sp_pair_to_poly(sp_RbVal el) {
  if (el.tag == SP_TAG_OBJ && el.cls_id == SP_BUILTIN_POLY_ARRAY) return (sp_PolyArray *)el.v.p;
  mrb_int n = sp_array_kind_len(el);
  if (n < 0) n = 0;
  sp_PolyArray *r = sp_PolyArray_new();
  SP_GC_ROOT(r);
  for (mrb_int j = 0; j < n; j++) sp_PolyArray_push(r, sp_poly_arr_get(el, j));
  return r;
}

/* Array#assoc — return the first sub-array whose first element equals `key`.
   Returns NULL when no match so the caller's `.inspect` round-trips to "nil".
   Each pair may be any array kind, so compare element 0 via sp_poly_arr_get;
   a pair with no element 0 (a non-array or empty array) is skipped. */
static sp_PolyArray *sp_PolyArray_assoc(sp_PolyArray *a, sp_RbVal key) {
  if (!a) return NULL;
  for (mrb_int i = 0; i < a->len; i++) {
    sp_RbVal el = a->data[i];
    if (sp_array_kind_len(el) >= 1 && sp_poly_eq(sp_poly_arr_get(el, 0), key))
      return sp_pair_to_poly(el);
  }
  return NULL;
}

/* Array#rassoc — same as assoc but matches against the second
   element of each sub-array (a pair with fewer than 2 elements is skipped). */
static sp_PolyArray *sp_PolyArray_rassoc(sp_PolyArray *a, sp_RbVal val) {
  if (!a) return NULL;
  for (mrb_int i = 0; i < a->len; i++) {
    sp_RbVal el = a->data[i];
    if (sp_array_kind_len(el) >= 2 && sp_poly_eq(sp_poly_arr_get(el, 1), val))
      return sp_pair_to_poly(el);
  }
  return NULL;
}
/* Depth-bounded variant. depth==0 returns a shallow copy; each
   recursive step decrements depth, and a negative depth means
   "unlimited" (same as flatten without arg). Used by
   `Array#flatten(n)`. */
static void sp_PolyArray_flatten_into_n(sp_PolyArray *dst, sp_RbVal v, mrb_int depth) {
  if (depth == 0 || v.tag != SP_TAG_OBJ) { sp_PolyArray_push(dst, v); return; }
  if (v.cls_id == SP_BUILTIN_INT_ARRAY) { sp_IntArray *ia = (sp_IntArray *)v.v.p; for (mrb_int i = 0; i < ia->len; i++) sp_PolyArray_push(dst, sp_box_int(ia->data[ia->start + i])); return; }
  if (v.cls_id == SP_BUILTIN_STR_ARRAY) { sp_StrArray *sa = (sp_StrArray *)v.v.p; for (mrb_int i = 0; i < sa->len; i++) sp_PolyArray_push(dst, sp_box_str(sa->data[i])); return; }
  if (v.cls_id == SP_BUILTIN_SYM_ARRAY) { sp_IntArray *ya = (sp_IntArray *)v.v.p; for (mrb_int i = 0; i < ya->len; i++) sp_PolyArray_push(dst, sp_box_sym((sp_sym)ya->data[ya->start + i])); return; }
  if (v.cls_id == SP_BUILTIN_FLT_ARRAY) { sp_FloatArray *fa = (sp_FloatArray *)v.v.p; for (mrb_int i = 0; i < fa->len; i++) sp_PolyArray_push(dst, sp_box_float(fa->data[i])); return; }
  if (v.cls_id == SP_BUILTIN_POLY_ARRAY) { sp_PolyArray *pa = (sp_PolyArray *)v.v.p; for (mrb_int i = 0; i < pa->len; i++) sp_PolyArray_flatten_into_n(dst, pa->data[i], depth - 1); return; }
  sp_PolyArray_push(dst, v);
}
static sp_PolyArray *sp_PolyArray_flatten_n(sp_PolyArray *a, mrb_int depth) {
  SP_GC_ROOT(a);
  sp_PolyArray *b = sp_PolyArray_new();
  SP_GC_ROOT(b);
  if (!a) return b;
  if (depth < 0) depth = INT64_MAX;
  for (mrb_int i = 0; i < a->len; i++) sp_PolyArray_flatten_into_n(b, a->data[i], depth);
  return b;
}
/* Transpose a poly-array of typed arrays (each row becomes a column).
   Handles rows that are IntArray, FloatArray, or StrArray.
   Result: a PolyArray of boxed typed column arrays. */
static sp_PolyArray *sp_poly_array_transpose(sp_PolyArray *rows) {
  SP_GC_SAVE();
  SP_GC_ROOT(rows);
  if (!rows || rows->len == 0) return sp_PolyArray_new();
  mrb_int nrows = rows->len;
  /* Determine column count and element kind from first non-empty row. */
  mrb_int ncols = 0;
  int16_t kind = 0; /* 0=unknown, SP_BUILTIN_INT_ARRAY, SP_BUILTIN_FLT_ARRAY, SP_BUILTIN_STR_ARRAY */
  for (mrb_int r = 0; r < nrows; r++) {
    sp_RbVal rv = rows->data[r];
    if (rv.tag != SP_TAG_OBJ) continue;
    mrb_int rlen = 0;
    if (rv.cls_id == SP_BUILTIN_INT_ARRAY)  { rlen = ((sp_IntArray *)rv.v.p)->len; if(!kind) kind = SP_BUILTIN_INT_ARRAY; }
    else if (rv.cls_id == SP_BUILTIN_FLT_ARRAY) { rlen = ((sp_FloatArray *)rv.v.p)->len; if(!kind) kind = SP_BUILTIN_FLT_ARRAY; }
    else if (rv.cls_id == SP_BUILTIN_STR_ARRAY) { rlen = ((sp_StrArray *)rv.v.p)->len; if(!kind) kind = SP_BUILTIN_STR_ARRAY; }
    if (rlen > ncols) ncols = rlen;
  }
  sp_PolyArray *result = sp_PolyArray_new();
  SP_GC_ROOT(result);
  for (mrb_int c = 0; c < ncols; c++) {
    sp_RbVal cv = sp_box_nil();
    if (kind == SP_BUILTIN_INT_ARRAY) {
      sp_IntArray *col = sp_IntArray_new();
      SP_GC_ROOT(col);
      for (mrb_int r = 0; r < nrows; r++) {
        sp_RbVal rv = rows->data[r];
        mrb_int val = SP_INT_NIL;
        if (rv.tag == SP_TAG_OBJ && rv.cls_id == SP_BUILTIN_INT_ARRAY) {
          sp_IntArray *row = (sp_IntArray *)rv.v.p;
          if (c < row->len) val = row->data[c];
        }
        sp_IntArray_push(col, val);
      }
      cv.tag = SP_TAG_OBJ; cv.cls_id = SP_BUILTIN_INT_ARRAY; cv.v.p = col;
    }
else if (kind == SP_BUILTIN_FLT_ARRAY) {
      sp_FloatArray *col = sp_FloatArray_new();
      SP_GC_ROOT(col);
      for (mrb_int r = 0; r < nrows; r++) {
        sp_RbVal rv = rows->data[r];
        mrb_float val = 0.0;
        if (rv.tag == SP_TAG_OBJ && rv.cls_id == SP_BUILTIN_FLT_ARRAY) {
          sp_FloatArray *row = (sp_FloatArray *)rv.v.p;
          if (c < row->len) val = row->data[c];
        }
        sp_FloatArray_push(col, val);
      }
      cv.tag = SP_TAG_OBJ; cv.cls_id = SP_BUILTIN_FLT_ARRAY; cv.v.p = col;
    }
else if (kind == SP_BUILTIN_STR_ARRAY) {
      sp_StrArray *col = sp_StrArray_new();
      SP_GC_ROOT(col);
      for (mrb_int r = 0; r < nrows; r++) {
        sp_RbVal rv = rows->data[r];
        const char *val = sp_str_empty;
        if (rv.tag == SP_TAG_OBJ && rv.cls_id == SP_BUILTIN_STR_ARRAY) {
          sp_StrArray *row = (sp_StrArray *)rv.v.p;
          if (c < row->len && row->data[c]) val = row->data[c];
        }
        sp_StrArray_push(col, val);
      }
      cv.tag = SP_TAG_OBJ; cv.cls_id = SP_BUILTIN_STR_ARRAY; cv.v.p = col;
    }
    sp_PolyArray_push(result, cv);
  }
  return result;
}
/* Keep old name as alias for backward compat with existing generated code. */
#define sp_int_array_transpose sp_poly_array_transpose
/* Sum the integer-tagged elements of a poly_array. Used by
   `Array#sum` on a poly_array whose runtime tags are uniform int
   (e.g. the result of `arr.map { _1[:int_key] }`). Non-int tags
   contribute zero. */
static mrb_int sp_PolyArray_sum_int(sp_PolyArray *a) { if (!a) return 0; mrb_int s = 0; for (mrb_int i = 0; i < a->len; i++) { if (a->data[i].tag == SP_TAG_INT) s += a->data[i].v.i; } return s; }
static sp_PolyArray *sp_PolyArray_from_int_array(sp_IntArray *a) { sp_PolyArray *p = sp_PolyArray_new(); if (!a) return p; for (mrb_int i = 0; i < a->len; i++) { mrb_int v = a->data[a->start+i]; sp_PolyArray_push(p, v == SP_INT_NIL ? sp_box_nil() : sp_box_int(v)); } return p; }
static sp_PolyArray *sp_PolyArray_from_str_array(sp_StrArray *a) { sp_PolyArray *p = sp_PolyArray_new(); if (!a) return p; for (mrb_int i = 0; i < a->len; i++) sp_PolyArray_push(p, sp_box_str(a->data[i])); return p; }
static sp_PolyArray *sp_PolyArray_from_float_array(sp_FloatArray *a) { sp_PolyArray *p = sp_PolyArray_new(); if (!a) return p; for (mrb_int i = 0; i < a->len; i++) sp_PolyArray_push(p, sp_box_float(a->data[i])); return p; }
static void sp_PolyArray_reverse_bang(sp_PolyArray *a) { if (!a || a->frozen) { if (a && a->frozen) sp_raise_frozen_array(); return; } for (mrb_int i = 0, j = a->len - 1; i < j; i++, j--) { sp_RbVal t = a->data[i]; a->data[i] = a->data[j]; a->data[j] = t; } }
static void sp_PolyArray_shuffle_bang(sp_PolyArray *a) { if (!a || a->frozen) { if (a && a->frozen) sp_raise_frozen_array(); return; } for (mrb_int i = a->len - 1; i > 0; i--) { mrb_int j = (mrb_int)(rand() % (i + 1)); sp_RbVal t = a->data[i]; a->data[i] = a->data[j]; a->data[j] = t; } }
static void sp_PolyArray_rotate_bang(sp_PolyArray*a,mrb_int n){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}if(a->len<=0)return;n=((n%a->len)+a->len)%a->len;if(n==0)return;sp_RbVal*d=a->data;mrb_int lo=0,hi=n-1;while(lo<hi){sp_RbVal t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=n;hi=a->len-1;while(lo<hi){sp_RbVal t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=0;hi=a->len-1;while(lo<hi){sp_RbVal t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}}
static sp_PolyArray *sp_PolyArray_shuffle(sp_PolyArray *a) { sp_PolyArray *b = sp_PolyArray_dup(a); sp_PolyArray_shuffle_bang(b); return b; }
/* When sort hits an incomparable pair the result is discarded and we raise
   ArgumentError, matching CRuby. The comparator cannot raise (it would longjmp
   out of the sort), so it records the offending pair and sort_bang raises after. */
static int _sp_sort_incomparable;
static sp_RbVal _sp_sort_inc_a, _sp_sort_inc_b;
static int _sp_poly_cmp_rec(const void *pa, const void *pb) {
  if (_sp_sort_incomparable) return 0;
  mrb_bool ok = FALSE;
  mrb_int r = sp_poly_cmp(*(const sp_RbVal *)pa, *(const sp_RbVal *)pb, &ok);
  if (!ok) { _sp_sort_incomparable = 1; _sp_sort_inc_a = *(const sp_RbVal *)pa; _sp_sort_inc_b = *(const sp_RbVal *)pb; return 0; }
  return (int)r;
}
/* Bottom-up merge sort over boxed elements. libc qsort visits pairs in an
   implementation-defined order, so which pair gets recorded as incomparable
   (and hence the ArgumentError message operands) would vary by platform; a
   fixed merge schedule keeps it identical everywhere, with the left/earlier
   element as the `<=>` receiver like CRuby. Both `a` and the scratch copy
   are rooted PolyArrays, so every element stays GC-reachable while the
   comparator (which can allocate) runs; stale slots in the buffer being
   overwritten are still valid boxed values, so scanning them is safe. */
static void _sp_poly_msort(sp_PolyArray *a, int (*cmp)(const void *, const void *)) {
  if (!a || a->len < 2) return;
  SP_GC_ROOT(a);
  sp_PolyArray *tmp = sp_PolyArray_dup(a);
  SP_GC_ROOT(tmp);
  sp_RbVal *src = a->data, *dst = tmp->data;
  for (mrb_int w = 1; w < a->len; w *= 2) {
    for (mrb_int lo = 0; lo < a->len; lo += 2 * w) {
      mrb_int mid = lo + w < a->len ? lo + w : a->len;
      mrb_int hi = lo + 2 * w < a->len ? lo + 2 * w : a->len;
      mrb_int i = lo, j = mid, k = lo;
      while (i < mid && j < hi)
        dst[k++] = cmp(&src[i], &src[j]) <= 0 ? src[i++] : src[j++];
      while (i < mid) dst[k++] = src[i++];
      while (j < hi) dst[k++] = src[j++];
    }
    sp_RbVal *t = src; src = dst; dst = t;
  }
  if (src != a->data) memcpy(a->data, src, (size_t)a->len * sizeof(sp_RbVal));
}
/* max/min over boxed elements: numerics/strings via sp_poly_cmp, int arrays
   lexicographically. Returns nil for an empty array. */
static sp_RbVal sp_PolyArray_max(sp_PolyArray *a) {
  if (!a || a->len == 0) return sp_box_nil();
  SP_GC_ROOT(a);  /* sp_poly_cmp can allocate; keep a (and best, which is one of
                     its elements) reachable across the comparisons. */
  sp_RbVal best = a->data[0];
  for (mrb_int i = 1; i < a->len; i++) {
    mrb_bool ok = FALSE;
    mrb_int r = sp_poly_cmp(a->data[i], best, &ok);
    if (!ok) r = sp_poly_cmp_int_arrays(a->data[i], best, &ok);
    if (!ok) sp_raise_cls("ArgumentError", sp_sprintf("comparison of %s with %s failed", sp_poly_class_name(a->data[i]), sp_poly_class_name(best)));
    if (r > 0) best = a->data[i];
  }
  return best;
}
static sp_RbVal sp_PolyArray_min(sp_PolyArray *a) {
  if (!a || a->len == 0) return sp_box_nil();
  SP_GC_ROOT(a);  /* sp_poly_cmp can allocate; keep a (and best, which is one of
                     its elements) reachable across the comparisons. */
  sp_RbVal best = a->data[0];
  for (mrb_int i = 1; i < a->len; i++) {
    mrb_bool ok = FALSE;
    mrb_int r = sp_poly_cmp(a->data[i], best, &ok);
    if (!ok) r = sp_poly_cmp_int_arrays(a->data[i], best, &ok);
    if (!ok) sp_raise_cls("ArgumentError", sp_sprintf("comparison of %s with %s failed", sp_poly_class_name(a->data[i]), sp_poly_class_name(best)));
    if (r < 0) best = a->data[i];
  }
  return best;
}
static void sp_PolyArray_sort_bang(sp_PolyArray *a) {
  if (!a || a->frozen) { if (a && a->frozen) sp_raise_frozen_array(); return; }
  /* Root the array across the sort: the comparator runs sp_poly_cmp, which can
     allocate (e.g. a bigint temp) and trigger GC; without this, a precise sweep
     could collect a (and its elements) mid-sort. This also roots the transient
     copy made by sp_PolyArray_sort. */
  SP_GC_ROOT(a);
  if (a->len > 1) {
    /* save/restore the flag and the offending pair so a comparison that
       re-enters sort cannot clobber this call's state. */
    int prev = _sp_sort_incomparable;
    sp_RbVal prev_a = _sp_sort_inc_a, prev_b = _sp_sort_inc_b;
    _sp_sort_incomparable = 0;
    _sp_poly_msort(a, _sp_poly_cmp_rec);
    int inc = _sp_sort_incomparable;
    sp_RbVal ia = _sp_sort_inc_a, ib = _sp_sort_inc_b;
    _sp_sort_incomparable = prev;
    _sp_sort_inc_a = prev_a;
    _sp_sort_inc_b = prev_b;
    if (inc) sp_raise_cls("ArgumentError", sp_sprintf("comparison of %s with %s failed", sp_poly_class_name(ia), sp_poly_class_name(ib)));
  }
}
static sp_PolyArray *sp_PolyArray_sort(sp_PolyArray *a) { sp_PolyArray *b = sp_PolyArray_dup(a); sp_PolyArray_sort_bang(b); return b; }
/* Object-array (sp_PtrArray of one user class, TY_OBJ_ARRAY) comparison
   family: box the elements with the statically-known cls_id (tag assembly,
   no allocation) and reuse the PolyArray comparator machinery -- the user
   `<=>` via the cmp hook, incomparable pairs raising the Comparable
   ArgumentError. Only the fresh result containers allocate; everything live
   across an allocation is rooted. */
static sp_PolyArray *sp_ptr_array_box(sp_PtrArray *a, int cls_id) {
  sp_PolyArray *p = sp_PolyArray_new(); SP_GC_ROOT(p);
  if (a) for (mrb_int i = 0; i < a->len; i++)
    sp_PolyArray_push(p, sp_box_nullable_obj(a->data[i], cls_id));
  return p;
}
static sp_PtrArray *sp_PtrArray_sort_obj(sp_PtrArray *a, int cls_id) __attribute__((unused));
static sp_PtrArray *sp_PtrArray_sort_obj(sp_PtrArray *a, int cls_id) {
  SP_GC_ROOT(a);
  sp_PolyArray *p = sp_ptr_array_box(a, cls_id);
  SP_GC_ROOT(p);   /* box's own root is popped on its return; sort + new_scan below allocate */
  sp_PolyArray_sort_bang(p);
  sp_PtrArray *r = a ? sp_PtrArray_new_scan(a->scan_elem) : sp_PtrArray_new();
  SP_GC_ROOT(r);
  for (mrb_int i = 0; i < p->len; i++) sp_PtrArray_push(r, p->data[i].v.p);
  return r;
}
static void sp_PtrArray_sort_obj_bang(sp_PtrArray *a, int cls_id) __attribute__((unused));
static void sp_PtrArray_sort_obj_bang(sp_PtrArray *a, int cls_id) {
  if (!a) return;
  if (a->frozen) { sp_raise_frozen_array(); return; }
  SP_GC_ROOT(a);
  sp_PolyArray *p = sp_ptr_array_box(a, cls_id);
  SP_GC_ROOT(p);   /* box's own root is popped on its return; sort_bang runs the user <=> (allocates) */
  sp_PolyArray_sort_bang(p);
  for (mrb_int i = 0; i < a->len; i++) a->data[i] = p->data[i].v.p;
}
/* min/max over an object array; empty -> NULL (the object-typed nil). */
static void *sp_PtrArray_minmax_obj(sp_PtrArray *a, int cls_id, int want_max) __attribute__((unused));
static void *sp_PtrArray_minmax_obj(sp_PtrArray *a, int cls_id, int want_max) {
  if (!a || a->len == 0) return NULL;
  SP_GC_ROOT(a);
  void *best = a->data[0];
  for (mrb_int i = 1; i < a->len; i++) {
    mrb_bool ok = FALSE;
    sp_RbVal bi = sp_box_nullable_obj(a->data[i], cls_id);
    sp_RbVal bb = sp_box_nullable_obj(best, cls_id);
    mrb_int r = sp_poly_cmp(bi, bb, &ok);
    if (!ok) sp_raise_cls("ArgumentError", sp_sprintf("comparison of %s with %s failed", sp_poly_class_name(bi), sp_poly_class_name(bb)));
    if (want_max ? (r > 0) : (r < 0)) best = a->data[i];
  }
  return best;
}
/* Compare two boxed arrays element-wise (Array#<=> semantics): first differing
   comparable element decides, else the shorter array sorts first. Used to order
   Hash#sort's [key, value] pairs. */
static int _sp_pair_cmp_incomparable;  /* set when two pairs cannot be ordered */
static int _sp_poly_pair_cmp(const void *pa, const void *pb) {
  /* Once a pair is found incomparable the sort result is discarded and we
     raise, so skip the remaining comparisons (and any work they would do). */
  if (_sp_pair_cmp_incomparable) return 0;
  sp_RbVal a = *(const sp_RbVal *)pa, b = *(const sp_RbVal *)pb;
  mrb_int na = sp_poly_arr_len(a), nb = sp_poly_arr_len(b);
  mrb_int n = na < nb ? na : nb;
  for (mrb_int i = 0; i < n; i++) {
    mrb_bool ok = FALSE;
    mrb_int r = sp_poly_cmp(sp_poly_arr_get(a, i), sp_poly_arr_get(b, i), &ok);
    if (!ok) { _sp_pair_cmp_incomparable = 1; return 0; }
    if (r != 0) return r < 0 ? -1 : 1;
  }
  return (na > nb) - (na < nb);
}
static sp_PolyArray *sp_PolyArray_sort_pairs(sp_PolyArray *a) {
  /* `a` is the caller's transient pair array, unrooted at the call site; root
     it before sp_PolyArray_dup allocates (and may collect). */
  SP_GC_ROOT(a);
  sp_PolyArray *b = sp_PolyArray_dup(a);
  if (b && b->len > 1) {
    /* Save/restore the flag around the sort so a comparison that re-enters
       sort_pairs (e.g. via a nested sort) cannot clobber this call's state. */
    int prev = _sp_pair_cmp_incomparable;
    _sp_pair_cmp_incomparable = 0;
    _sp_poly_msort(b, _sp_poly_pair_cmp);
    int incomparable = _sp_pair_cmp_incomparable;
    _sp_pair_cmp_incomparable = prev;
    if (incomparable)
      sp_raise_cls("ArgumentError", "comparison of Array with Array failed");
  }
  return b;
}
/* Schwartzian helper for Hash#sort_by: `a` is an array of [sort_key, value]
   tuples; sort it by the comparable sort_key and return the values in order. */
static int _sp_poly_first_cmp(const void *pa, const void *pb) {
  mrb_bool ok = FALSE;
  mrb_int r = sp_poly_cmp(sp_poly_arr_get(*(const sp_RbVal *)pa, 0),
                          sp_poly_arr_get(*(const sp_RbVal *)pb, 0), &ok);
  return ok ? (r < 0 ? -1 : (r > 0 ? 1 : 0)) : 0;
}
static sp_PolyArray *sp_PolyArray_sort_by_first(sp_PolyArray *a) {
  SP_GC_ROOT(a);
  sp_PolyArray *b = sp_PolyArray_dup(a); SP_GC_ROOT(b);
  if (b && b->len > 1) _sp_poly_msort(b, _sp_poly_first_cmp);
  sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r);
  for (mrb_int i = 0; b && i < b->len; i++) sp_PolyArray_push(r, sp_poly_arr_get(b->data[i], 1));
  return r;
}
static void sp_PolyArray_uniq_bang(sp_PolyArray*a){if(!a||a->frozen){if(a&&a->frozen)sp_raise_frozen_array();return;}for(mrb_int i=0;i<a->len;){int dup=0;for(mrb_int j=0;j<i;j++){if(sp_poly_eq(a->data[j],a->data[i])){dup=1;break;}}if(dup){for(mrb_int k2=i;k2<a->len-1;k2++)a->data[k2]=a->data[k2+1];a->len--;}else i++;}}
static sp_RbVal sp_PolyArray_sample(sp_PolyArray *a) { if (a->len <= 0) return sp_box_nil(); return a->data[(mrb_int)(rand()%a->len)]; }

/* Forward decl: sp_poly_inspect dispatches into sp_PolyArray_inspect
   for nested poly arrays (under promote, an `each_cons` chain's outer
   accumulator boxes each inner poly_array element), but the
   sp_PolyArray_inspect body lives a few lines below. */
static const char *sp_PolyArray_inspect(sp_PolyArray *a);
static const char*sp_PolyArrayPtrArray_inspect(sp_PtrArray*a){SP_GC_ROOT(a);sp_String*s=sp_String_new("[");SP_GC_ROOT(s);for(mrb_int i=0;i<a->len;i++){if(i>0)sp_String_append(s,", ");sp_String_append(s,sp_PolyArray_inspect((sp_PolyArray*)a->data[i]));}sp_String_append(s,"]");return s->data;}

/* Poly-key/value hash inspect helpers are defined after sp_poly_inspect
   (they call back into it for their elements), so forward-declare them
   here for the SP_TAG_OBJ hash arms below. The struct typedefs also live
   further down, so forward-declare those tags too. */
typedef struct sp_StrPolyHash sp_StrPolyHash;
typedef struct sp_SymPolyHash sp_SymPolyHash;
typedef struct sp_PolyPolyHash sp_PolyPolyHash;
static const char *sp_StrPolyHash_inspect(sp_StrPolyHash *h);
static const char *sp_SymPolyHash_inspect(sp_SymPolyHash *h);
static const char *sp_PolyPolyHash_inspect(sp_PolyPolyHash *h);

/* Object#inspect for a tagged sp_RbVal. Dispatches on the runtime tag;
   each branch reuses the matching primitive inspect helper. Falls back
   to "#<Object>" for SP_TAG_OBJ because the runtime has no class-name
   table yet (follow-up PR). Returns a GC-managed C string. */
static inline const char *sp_poly_inspect(sp_RbVal v) {
  switch (v.tag) {
    /* An int-typed nil (unfilled int block param, nullable-int miss) carries
       the SP_INT_NIL sentinel; render it as nil, not the raw INT64_MIN. */
    case SP_TAG_INT:  return v.v.i == SP_INT_NIL ? SPL("nil") : sp_int_to_s(v.v.i);
    case SP_TAG_STR:  return sp_str_inspect(v.v.s);
    case SP_TAG_FLT:  return sp_float_to_s(v.v.f);
    case SP_TAG_BOOL: return v.v.b ? SPL("true") : SPL("false");
    case SP_TAG_NIL:  return SPL("nil");
    case SP_TAG_SYM:  return sp_sym_inspect((sp_sym)v.v.i);
    case SP_TAG_ENCODING: return sp_sprintf("#<Encoding:%s>", v.v.s ? v.v.s : "");
    case SP_TAG_CLASS: return sp_class_val_name(v);
    case SP_TAG_BIGINT: return sp_bigint_to_s((sp_Bigint *)v.v.p);
    case SP_TAG_OBJ:
 /* Built-in container / value-type tags get their typed inspect
    helper. Matches the dispatch shape in sp_poly_to_s above and the
    `puts` poly arm earlier in this file; without it, a Range / Time
    / typed Array stored as an sp_RbVal value (e.g. a sym_poly_hash
    that mixes `200..299` and `404`) reported "#<Object>" from
    `.inspect`, which was both wrong for CRuby parity and useless
    for debugging. */
      switch (v.cls_id) {
        case SP_BUILTIN_INT_ARRAY: return sp_IntArray_inspect((sp_IntArray *)v.v.p);
        case SP_BUILTIN_FLT_ARRAY: return sp_FloatArray_inspect((sp_FloatArray *)v.v.p);
        case SP_BUILTIN_STR_ARRAY: return sp_StrArray_inspect((sp_StrArray *)v.v.p);
        case SP_BUILTIN_SYM_ARRAY: return sp_SymArray_inspect((sp_IntArray *)v.v.p);
        case SP_BUILTIN_PTR_ARRAY: return sp_PtrArray_inspect((sp_PtrArray *)v.v.p);
        case SP_BUILTIN_POLY_ARRAY: return sp_PolyArray_inspect((sp_PolyArray *)v.v.p);
        case SP_BUILTIN_RANGE:     return sp_Range_inspect((sp_Range *)v.v.p);
        case SP_BUILTIN_TIME:      return sp_Time_inspect((sp_Time *)v.v.p);
        case SP_BUILTIN_COMPLEX:   return sp_complex_inspect(*(sp_Complex *)v.v.p);
        case SP_BUILTIN_RATIONAL:  return sp_rational_inspect(*(sp_Rational *)v.v.p);
        case SP_BUILTIN_EXCEPTION: return sp_sprintf("#<%s: %s>", sp_exc_class_name((volatile struct sp_Exception_s *)v.v.p), sp_exc_message((volatile struct sp_Exception_s *)v.v.p));
        case SP_BUILTIN_STR_INT_HASH:  return sp_StrIntHash_inspect((sp_StrIntHash *)v.v.p);
        case SP_BUILTIN_STR_STR_HASH:  return sp_StrStrHash_inspect((sp_StrStrHash *)v.v.p);
        case SP_BUILTIN_INT_STR_HASH:  return sp_IntStrHash_inspect((sp_IntStrHash *)v.v.p);
        case SP_BUILTIN_STR_POLY_HASH: return sp_StrPolyHash_inspect((sp_StrPolyHash *)v.v.p);
        case SP_BUILTIN_SYM_POLY_HASH: return sp_SymPolyHash_inspect((sp_SymPolyHash *)v.v.p);
        case SP_BUILTIN_POLY_POLY_HASH: return sp_PolyPolyHash_inspect((sp_PolyPolyHash *)v.v.p);
        default:                   return SPL("#<Object>");
      }
    default:          return sp_str_empty;
  }
}
/* Array#inspect for heterogeneous poly arrays. Each element dispatches
   through sp_poly_inspect, so a mixed `[1, "x", :y]` renders
   `[1, "x", :y]` byte-for-byte identical to CRuby. NULL renders
   "nil" so callers that store a nil-returning slot (assoc/rassoc
   miss, etc.) round-trip cleanly through `.inspect`. */
static const char *sp_PolyArray_inspect(sp_PolyArray *a) {
  if (!a) { char *r = sp_str_alloc(3); r[0] = 'n'; r[1] = 'i'; r[2] = 'l'; r[3] = 0; sp_str_set_len(r, 3); return r; }
  return sp_inspect_container(sp_box_poly_array(a));
}
/* Array#join for a mixed-element (poly) array: to_s each element via
   sp_poly_to_s and concatenate with sep. Mirrors sp_StrArray_join for
   the boxed-element case. */
static const char *sp_PolyArray_join(sp_PolyArray *a, const char *sep) {
  if (!a) return sp_str_empty;
  SP_GC_ROOT(a); SP_GC_ROOT(sep);
  sp_String *s = sp_String_new("");
  SP_GC_ROOT(s);
  for (mrb_int i = 0; i < a->len; i++) {
    if (i > 0 && sep) sp_String_append(s, sep);
    sp_String_append(s, sp_poly_to_s(a->data[i]));
  }
  return s->data;
}
/* join on a boxed array (poly value holding any array kind) */
static const char *sp_poly_join(sp_RbVal a, const char *sep) {
  if (a.tag != SP_TAG_OBJ) return sp_poly_to_s(a);
  /* `.join` on a poly value is Array#join here, but a Thread carried in a poly
     slot (e.g. `threads.each(&:join)`, where the thread array boxed to poly)
     means Thread#join: run it to completion. Its result (the thread) is almost
     always discarded, so yield the empty string for the const char* surface. */
  if (a.cls_id == SP_BUILTIN_THREAD) { sp_Thread_join((sp_thread *)a.v.p); return sp_str_empty; }
  switch (a.cls_id) {
    case SP_BUILTIN_STR_ARRAY: return sp_StrArray_join((sp_StrArray *)a.v.p, sep);
    case SP_BUILTIN_POLY_ARRAY: return sp_PolyArray_join((sp_PolyArray *)a.v.p, sep);
    case SP_BUILTIN_INT_ARRAY: {
      sp_IntArray *ar = (sp_IntArray *)a.v.p;
      if (!ar || ar->len == 0) return sp_str_empty;
      sp_String *s = sp_String_new(""); SP_GC_ROOT(s);
      for (mrb_int i = 0; i < ar->len; i++) {
        if (i > 0 && sep) sp_String_append(s, sep);
        sp_String_append(s, sp_int_to_s(ar->data[ar->start + i]));
      }
      return s->data;
    }
    default: return sp_poly_to_s(a);
  }
}
static mrb_bool sp_PolyArray_eq(sp_PolyArray *a, sp_PolyArray *b) {
  if (!a || !b) return a == b;
  if (a->len != b->len) return FALSE;
  for (mrb_int i = 0; i < a->len; i++) {
    if (!sp_poly_eq(a->data[i], b->data[i])) return FALSE;
  }
  return TRUE;
}
/* Box a typed (int/str/float) array into a fresh poly array element-wise.
   `kind` is the typed array's SP_BUILTIN_* tag. */
static sp_PolyArray *sp_typed_to_poly(void *tp, int kind) {
  sp_PolyArray *tb = sp_PolyArray_new();
  if (!tp) return tb;
  if (kind == SP_BUILTIN_STR_ARRAY) {
    sp_StrArray *a = (sp_StrArray *)tp;
    for (mrb_int i = 0; i < a->len; i++) sp_PolyArray_push(tb, sp_box_str(sp_StrArray_get(a, i)));
  }
  else if (kind == SP_BUILTIN_FLT_ARRAY) {
    sp_FloatArray *a = (sp_FloatArray *)tp;
    for (mrb_int i = 0; i < a->len; i++) sp_PolyArray_push(tb, sp_box_float(sp_FloatArray_get(a, i)));
  }
  else {
    sp_IntArray *a = (sp_IntArray *)tp;
    for (mrb_int i = 0; i < a->len; i++) sp_PolyArray_push(tb, sp_box_int(sp_IntArray_get(a, i)));
  }
  return tb;
}
/* Compare a poly array against a typed (int/str/float) array by boxing the
   typed side element-wise. `kind` is the typed array's SP_BUILTIN_* tag. */
static mrb_bool sp_PolyArray_eq_typed(sp_PolyArray *pa, void *tp, int kind) {
  if (!pa || !tp) return FALSE;
  SP_GC_ROOT(pa); SP_GC_ROOT(tp);  /* sp_typed_to_poly allocates */
  return sp_PolyArray_eq(pa, sp_typed_to_poly(tp, kind));
}
static mrb_bool sp_PolyArray_include(sp_PolyArray *a, sp_RbVal v) {
  if (!a) return FALSE;
  for (mrb_int i = 0; i < a->len; i++) {
    if (sp_poly_eq(a->data[i], v)) return TRUE;
  }
  return FALSE;
}

/* Mark the embedded GC reference inside an sp_RbVal (string or obj).
   Used as the scan hook for containers that store polymorphic values. */
/* sp_mark_rbval is an inline helper in sp_gc.h. */

/* StrPolyHash: string keys, sp_RbVal values — for hashes with mixed value types. */
/* `dproc` holds a Hash.new{} default block, lowered to a dedicated C
   fn `sp_RbVal (*)(sp_StrPolyHash *self, const char *key)` with typed
   params (codegen emits it). Called by _get on a miss. Issue #912. */
typedef struct sp_StrPolyHash sp_StrPolyHash;
typedef sp_RbVal (*sp_strpoly_dproc_t)(sp_StrPolyHash *, const char *, void *);
struct sp_StrPolyHash{const char**keys;sp_RbVal*vals;const char**order;mrb_int len;mrb_int cap;mrb_int mask;sp_RbVal default_v;sp_strpoly_dproc_t dproc;void *dproc_self;};
static void sp_StrPolyHash_fin(void*p){sp_StrPolyHash*h=(sp_StrPolyHash*)p;free(h->keys);free(h->vals);free(h->order);}
static void sp_StrPolyHash_scan(void*p){sp_StrPolyHash*h=(sp_StrPolyHash*)p;for(mrb_int i=0;i<h->cap;i++){if(h->keys[i]){sp_mark_string(h->keys[i]);sp_mark_rbval(h->vals[i]);}}sp_mark_rbval(h->default_v);}
static sp_StrPolyHash*sp_StrPolyHash_new(void){sp_StrPolyHash*h=(sp_StrPolyHash*)sp_gc_alloc(sizeof(sp_StrPolyHash),sp_StrPolyHash_fin,sp_StrPolyHash_scan);h->cap=16;h->mask=15;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->order=(const char**)malloc(sizeof(const char*)*h->cap);h->len=0;h->default_v=sp_box_nil();return h;}
static sp_StrPolyHash*sp_StrPolyHash_new_with_default(sp_RbVal d){sp_StrPolyHash*h=sp_StrPolyHash_new();h->default_v=d;return h;}
static sp_StrPolyHash*sp_StrPolyHash_new_dproc(sp_strpoly_dproc_t fn,void*self){sp_StrPolyHash*h=sp_StrPolyHash_new();h->dproc=fn;h->dproc_self=self;return h;}
static void sp_StrPolyHash_grow(sp_StrPolyHash*h){mrb_int oc=h->cap;const char**ok=h->keys;sp_RbVal*ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(const char**)calloc(h->cap,sizeof(const char*));h->vals=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->order=(const char**)realloc(h->order,sizeof(const char*)*h->cap);h->len=0;for(mrb_int i=0;i<oc;i++){if(ok[i]){mrb_int idx=(mrb_int)(sp_str_hash(ok[i])&h->mask);while(h->keys[idx])idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}
static sp_RbVal sp_StrPolyHash_get(sp_StrPolyHash*h,const char*k){if(!h)return sp_box_nil();mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k))return h->vals[idx];idx=(idx+1)&h->mask;}if(h->dproc)return h->dproc(h,k,h->dproc_self);return h->default_v;}
static void sp_StrPolyHash_set(sp_StrPolyHash*h,const char*k,sp_RbVal v){if(h->len*2>=h->cap)sp_StrPolyHash_grow(h);mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k)){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}
static mrb_bool sp_StrPolyHash_has_key(sp_StrPolyHash*h,const char*k){if(!h)return FALSE;mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k))return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static mrb_int sp_StrPolyHash_length(sp_StrPolyHash*h){return h->len;}
static sp_StrArray*sp_StrPolyHash_keys(sp_StrPolyHash*h){SP_GC_ROOT(h);sp_StrArray*a=sp_StrArray_new();SP_GC_ROOT(a);if(!h)return a;for(mrb_int i=0;i<h->len;i++)sp_StrArray_push(a,h->order[i]);return a;}
static sp_PolyArray*sp_StrPolyHash_values(sp_StrPolyHash*h){SP_GC_ROOT(h);sp_PolyArray*a=sp_PolyArray_new();SP_GC_ROOT(a);for(mrb_int i=0;i<h->len;i++)sp_PolyArray_push(a,sp_StrPolyHash_get(h,h->order[i]));return a;}
static mrb_bool sp_StrPolyHash_has_value(sp_StrPolyHash*h,sp_RbVal v){if(!h)return FALSE;for(mrb_int i=0;i<h->len;i++)if(sp_poly_eq(sp_StrPolyHash_get(h,h->order[i]),v))return TRUE;return FALSE;}
static void sp_StrPolyHash_delete(sp_StrPolyHash*h,const char*k){mrb_int idx=(mrb_int)(sp_str_hash(k)&h->mask);while(h->keys[idx]){if(sp_str_eq(h->keys[idx],k)){h->keys[idx]=NULL;h->vals[idx]=sp_box_nil();h->len--;mrb_int j=(idx+1)&h->mask;while(h->keys[j]){mrb_int nj=(mrb_int)(sp_str_hash(h->keys[j])&h->mask);if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];h->keys[j]=NULL;h->vals[j]=sp_box_nil();idx=j;}j=(j+1)&h->mask;}{mrb_int oi=0;while(oi<=h->len){if(strcmp(h->order[oi],k)==0){while(oi<h->len){h->order[oi]=h->order[oi+1];oi++;}break;}oi++;}}return;}idx=(idx+1)&h->mask;}}
/* Hash#merge for str_poly_hash. Same shape as the
   StrIntHash / SymPolyHash siblings -- copy recv's entries into a
   fresh hash, then overlay other's. */
static sp_StrPolyHash*sp_StrPolyHash_merge(sp_StrPolyHash*a,sp_StrPolyHash*b){sp_StrPolyHash*r=sp_StrPolyHash_new();r->default_v=a->default_v;for(mrb_int i=0;i<a->len;i++)sp_StrPolyHash_set(r,a->order[i],sp_StrPolyHash_get(a,a->order[i]));for(mrb_int i=0;i<b->len;i++)sp_StrPolyHash_set(r,b->order[i],sp_StrPolyHash_get(b,b->order[i]));return r;}
static sp_StrPolyHash*sp_StrPolyHash_dup(sp_StrPolyHash*h){sp_StrPolyHash*r=sp_StrPolyHash_new();r->default_v=h->default_v;for(mrb_int i=0;i<h->len;i++)sp_StrPolyHash_set(r,h->order[i],sp_StrPolyHash_get(h,h->order[i]));return r;}
static sp_StrPolyHash*sp_StrPolyHash_replace(sp_StrPolyHash*h,sp_StrPolyHash*o){if(!h)return h;for(mrb_int i=0;i<h->cap;i++)h->keys[i]=NULL;h->len=0;if(o)for(mrb_int i=0;i<o->len;i++)sp_StrPolyHash_set(h,o->order[i],sp_StrPolyHash_get(o,o->order[i]));return h;}
static void sp_StrPolyHash_clear(sp_StrPolyHash*h){if(!h)return;for(mrb_int i=0;i<h->cap;i++)h->keys[i]=NULL;h->len=0;}
static mrb_bool sp_StrPolyHash_eq(sp_StrPolyHash*a,sp_StrPolyHash*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++){const char*k=a->order[i];if(!sp_StrPolyHash_has_key(b,k))return FALSE;if(!sp_poly_eq(sp_StrPolyHash_get(a,k),sp_StrPolyHash_get(b,k)))return FALSE;}return TRUE;}
/* Issue #851: inspect for str_poly_hash. */
static const char*sp_StrPolyHash_inspect(sp_StrPolyHash*h){return h?sp_inspect_container(sp_box_obj(h,SP_BUILTIN_STR_POLY_HASH)):"{}";}
/* Convert a narrower StrStrHash to a StrPolyHash. Needed when the
   analyzer widens an LV slot to sp_StrPolyHash* (e.g. later poly-value
   writes) but the initial RHS is a sibling narrower hash variant —
   raw pointer assignment would mix incompatible struct layouts
   (vals[] of const char** vs sp_RbVal*). See issue #614. */
static sp_StrPolyHash*sp_StrPolyHash_from_str_str_hash(sp_StrStrHash*h){sp_StrPolyHash*r=sp_StrPolyHash_new();if(!h)return r;if(h->default_v)r->default_v=sp_box_str(h->default_v);for(mrb_int i=0;i<h->len;i++){const char*k=h->order[i];sp_StrPolyHash_set(r,k,sp_box_str(sp_StrStrHash_get(h,k)));}return r;}
/* MatchData#named_captures: {String name => group substring | nil}. A
   non-participating named group maps to nil, so the value side is poly. Lives
   here (not sp_re.c) because the typed-hash machinery is TU-coupled. */
static sp_StrPolyHash *sp_md_named_captures(sp_MatchData *m) {
  sp_StrPolyHash *h = sp_StrPolyHash_new();
  if (!m) return h;
  int n = re_num_named(m->pat);
  for (int i = 0; i < n; i++) {
    int g = -1;
    const char *nm = re_named_name(m->pat, i, &g);
    if (nm) sp_StrPolyHash_set(h, sp_str_dup(nm), sp_box_nullable_str(sp_MatchData_aref(m, g)));
  }
  return h;
}
static sp_StrPolyHash*sp_StrPolyHash_from_str_int_hash(sp_StrIntHash*h){sp_StrPolyHash*r=sp_StrPolyHash_new();if(!h)return r;r->default_v=sp_box_int(h->default_v);for(mrb_int i=0;i<h->len;i++){const char*k=h->order[i];sp_StrPolyHash_set(r,k,sp_box_int(sp_StrIntHash_get(h,k)));}return r;}

/* SymPolyHash: symbol keys, sp_RbVal values — same shape as SymStrHash but with poly values. */
/* Named struct so lib/sp_fiber.c can forward-declare it for sp_Fiber's
   `storage` member without pulling in the full poly-hash machinery. */
/* dproc holds a Hash.new{} default block (symbol-keyed), lowered to a C fn
   with a typed sp_sym key param. Called by _get on a miss. */
typedef sp_RbVal (*sp_sympoly_dproc_t)(sp_SymPolyHash *, sp_sym, void *);
typedef struct sp_SymPolyHash{sp_sym*keys;sp_RbVal*vals;sp_sym*order;mrb_int len;mrb_int cap;mrb_int mask;sp_RbVal default_v;sp_sympoly_dproc_t dproc;void *dproc_self;}sp_SymPolyHash;
static void sp_SymPolyHash_fin(void*p){sp_SymPolyHash*h=(sp_SymPolyHash*)p;free(h->keys);free(h->vals);free(h->order);}
static void sp_SymPolyHash_scan(void*p){sp_SymPolyHash*h=(sp_SymPolyHash*)p;for(mrb_int i=0;i<h->cap;i++){if(h->keys[i]>=0)sp_mark_rbval(h->vals[i]);}sp_mark_rbval(h->default_v);if(h->dproc_self)sp_gc_mark(h->dproc_self);}
static sp_SymPolyHash*sp_SymPolyHash_new(void){sp_SymPolyHash*h=(sp_SymPolyHash*)sp_gc_alloc(sizeof(sp_SymPolyHash),sp_SymPolyHash_fin,sp_SymPolyHash_scan);h->cap=16;h->mask=15;h->keys=(sp_sym*)malloc(sizeof(sp_sym)*h->cap);for(mrb_int i=0;i<h->cap;i++)h->keys[i]=-1;h->vals=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->order=(sp_sym*)malloc(sizeof(sp_sym)*h->cap);h->len=0;h->default_v=sp_box_nil();return h;}
static sp_SymPolyHash*sp_SymPolyHash_new_with_default(sp_RbVal d){sp_SymPolyHash*h=sp_SymPolyHash_new();h->default_v=d;return h;}
static sp_SymPolyHash*sp_SymPolyHash_new_dproc(sp_sympoly_dproc_t fn,void*self){sp_SymPolyHash*h=sp_SymPolyHash_new();h->dproc=fn;h->dproc_self=self;return h;}
static void sp_SymPolyHash_grow(sp_SymPolyHash*h){mrb_int oc=h->cap;sp_sym*ok=h->keys;sp_RbVal*ov=h->vals;h->cap*=2;h->mask=h->cap-1;h->keys=(sp_sym*)malloc(sizeof(sp_sym)*h->cap);for(mrb_int i=0;i<h->cap;i++)h->keys[i]=-1;h->vals=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->order=(sp_sym*)realloc(h->order,sizeof(sp_sym)*h->cap);h->len=0;for(mrb_int i=0;i<oc;i++){if(ok[i]>=0){mrb_int idx=(mrb_int)(((mrb_int)ok[i])&h->mask);while(h->keys[idx]>=0)idx=(idx+1)&h->mask;h->keys[idx]=ok[i];h->vals[idx]=ov[i];h->len++;}}free(ok);free(ov);}
/* miss path split out cold+noinline: the dproc check must not sit inline in
   _get -- the extra branch/code pushed the hot inlined lookup over the inline
   threshold and cost optcarrot ~35% fps (same lesson as the string-hash cache:
   hot-path inline branches regress; SP_NOINLINE the cold side). */
static SP_NOINLINE sp_RbVal sp_SymPolyHash_miss(sp_SymPolyHash*h,sp_sym k){if(h->dproc)return h->dproc(h,k,h->dproc_self);return h->default_v;}
static sp_RbVal sp_SymPolyHash_get(sp_SymPolyHash*h,sp_sym k){if(!h)return sp_box_nil();mrb_int idx=(mrb_int)(((mrb_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k)return h->vals[idx];idx=(idx+1)&h->mask;}return sp_SymPolyHash_miss(h,k);}
static void sp_SymPolyHash_set(sp_SymPolyHash*h,sp_sym k,sp_RbVal v){if(h->len*2>=h->cap)sp_SymPolyHash_grow(h);mrb_int idx=(mrb_int)(((mrb_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->order[h->len]=k;h->len++;}
static mrb_bool sp_SymPolyHash_has_key(sp_SymPolyHash*h,sp_sym k){mrb_int idx=(mrb_int)(((mrb_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k)return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static mrb_int sp_SymPolyHash_length(sp_SymPolyHash*h){return h->len;}
static sp_IntArray*sp_SymPolyHash_keys(sp_SymPolyHash*h){SP_GC_ROOT(h);sp_IntArray*a=sp_IntArray_new();SP_GC_ROOT(a);for(mrb_int i=0;i<h->len;i++)sp_IntArray_push(a,(mrb_int)h->order[i]);return a;}
static sp_PolyArray*sp_SymPolyHash_values(sp_SymPolyHash*h){SP_GC_ROOT(h);sp_PolyArray*a=sp_PolyArray_new();SP_GC_ROOT(a);for(mrb_int i=0;i<h->len;i++)sp_PolyArray_push(a,sp_SymPolyHash_get(h,h->order[i]));return a;}
static mrb_bool sp_SymPolyHash_has_value(sp_SymPolyHash*h,sp_RbVal v){if(!h)return FALSE;for(mrb_int i=0;i<h->len;i++)if(sp_poly_eq(sp_SymPolyHash_get(h,h->order[i]),v))return TRUE;return FALSE;}
static sp_sym sp_SymPolyHash_key(sp_SymPolyHash*h,sp_RbVal v){if(!h)return (sp_sym)-1;for(mrb_int i=0;i<h->len;i++)if(sp_poly_eq(sp_SymPolyHash_get(h,h->order[i]),v))return h->order[i];return (sp_sym)-1;}
static sp_SymPolyHash*sp_SymPolyHash_merge(sp_SymPolyHash*a,sp_SymPolyHash*b){sp_SymPolyHash*r=sp_SymPolyHash_new();r->default_v=a->default_v;for(mrb_int i=0;i<a->len;i++)sp_SymPolyHash_set(r,a->order[i],sp_SymPolyHash_get(a,a->order[i]));for(mrb_int i=0;i<b->len;i++)sp_SymPolyHash_set(r,b->order[i],sp_SymPolyHash_get(b,b->order[i]));return r;}
static void sp_SymPolyHash_update(sp_SymPolyHash*a,sp_SymPolyHash*b){if(!a||!b||a==b)return;SP_GC_ROOT(a);SP_GC_ROOT(b);for(mrb_int i=0;i<b->len;i++)sp_SymPolyHash_set(a,b->order[i],sp_SymPolyHash_get(b,b->order[i]));}
/* Hash#delete for sym_poly_hash. Removes key and re-tombstones the
   slot, shifting probe-chain successors backward and dropping the
   key from the insertion-order list. Issue #510. */
static void sp_SymPolyHash_delete(sp_SymPolyHash*h,sp_sym k){mrb_int idx=(mrb_int)(((mrb_int)k)&h->mask);while(h->keys[idx]>=0){if(h->keys[idx]==k){h->keys[idx]=-1;h->vals[idx]=sp_box_nil();h->len--;mrb_int j=(idx+1)&h->mask;while(h->keys[j]>=0){mrb_int nj=(mrb_int)(((mrb_int)h->keys[j])&h->mask);if((j>idx&&(nj<=idx||nj>j))||(j<idx&&nj<=idx&&nj>j)){h->keys[idx]=h->keys[j];h->vals[idx]=h->vals[j];h->keys[j]=-1;h->vals[j]=sp_box_nil();idx=j;}j=(j+1)&h->mask;}{mrb_int oi=0;while(oi<=h->len){if(h->order[oi]==k){while(oi<h->len){h->order[oi]=h->order[oi+1];oi++;}break;}oi++;}}return;}idx=(idx+1)&h->mask;}}
static sp_SymPolyHash*sp_SymPolyHash_dup(sp_SymPolyHash*h){sp_SymPolyHash*r=sp_SymPolyHash_new();r->default_v=h->default_v;for(mrb_int i=0;i<h->len;i++)sp_SymPolyHash_set(r,h->order[i],sp_SymPolyHash_get(h,h->order[i]));return r;}
static sp_SymPolyHash *sp_PolyArray_tally(sp_PolyArray *a) { sp_SymPolyHash *h = sp_SymPolyHash_new(); if (!a) return h; for (mrb_int i = 0; i < a->len; i++) { sp_RbVal v = a->data[i]; if (v.tag != SP_TAG_SYM) continue; sp_sym k = (sp_sym)v.v.i; sp_RbVal cur = sp_SymPolyHash_get(h, k); mrb_int c = (cur.tag == SP_TAG_INT) ? cur.v.i : 0; sp_SymPolyHash_set(h, k, sp_box_int(c + 1)); } return h; }
static sp_SymPolyHash*sp_SymPolyHash_replace(sp_SymPolyHash*h,sp_SymPolyHash*o){if(!h)return h;for(mrb_int i=0;i<h->cap;i++)h->keys[i]=-1;h->len=0;if(o)for(mrb_int i=0;i<o->len;i++)sp_SymPolyHash_set(h,o->order[i],sp_SymPolyHash_get(o,o->order[i]));return h;}
static void sp_SymPolyHash_clear(sp_SymPolyHash*h){if(!h)return;for(mrb_int i=0;i<h->cap;i++)h->keys[i]=-1;h->len=0;}
static mrb_bool sp_SymPolyHash_eq(sp_SymPolyHash*a,sp_SymPolyHash*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++){sp_sym k=a->order[i];if(!sp_SymPolyHash_has_key(b,k))return FALSE;if(!sp_poly_eq(sp_SymPolyHash_get(a,k),sp_SymPolyHash_get(b,k)))return FALSE;}return TRUE;}
/* Hash#inspect for sym_poly_hash. CRuby 4.0 renders symbol keys
   in shorthand: `{a: 1, b: "x"}` rather than `{:a => 1, :b => "x"}`. */
static const char*sp_SymPolyHash_inspect(sp_SymPolyHash*h){return h?sp_inspect_container(sp_box_obj(h,SP_BUILTIN_SYM_POLY_HASH)):"{}";}

/* poly_val[sym_key]: runtime dispatch for poly receiver `[]` with symbol arg. */
/* sp_poly_get_sym moved below PolyPolyHash so it can dispatch to it. */
/* poly_val[str_key]: runtime dispatch for poly receiver `[]` with string arg.
   Defined after PolyPolyHash. */
/* PolyPolyHash: heterogeneous keys + values (both sp_RbVal). For
   primitives the hash/eql is tag-based (value equality); for OBJ tag
   the default is pointer identity. Codegen patches sp_obj_hash_hook /
   sp_obj_eql_hook with class-aware overrides (e.g. Method#eql? compares
   bound receiver + fn_ptr) when the program needs them — null hooks
   leave the runtime at identity, which is the right default for typed
   pointers like IntArray. */
typedef mrb_int  (*sp_obj_hash_fn)(int cls_id, void *p);
typedef mrb_bool (*sp_obj_eql_fn)(int cls_id, void *a, void *b);
static sp_obj_hash_fn sp_obj_hash_hook = NULL;
static sp_obj_eql_fn  sp_obj_eql_hook  = NULL;
static mrb_int sp_rbval_hash_key(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_INT: case SP_TAG_BOOL: case SP_TAG_NIL: case SP_TAG_SYM:
      return (mrb_int)v.v.i;
    case SP_TAG_BIGINT:
      return (mrb_int)sp_bigint_to_int((sp_Bigint *)v.v.p);
    case SP_TAG_STR:
      return v.v.s ? (mrb_int)sp_str_hash(v.v.s) : 0;
    case SP_TAG_ENCODING:
      return v.v.s ? (mrb_int)sp_str_hash(v.v.s) : 0;
    case SP_TAG_FLT: { uint64_t b; memcpy(&b, &v.v.f, sizeof(b)); return (mrb_int)b; }
    case SP_TAG_OBJ:
      if (v.cls_id == SP_BUILTIN_INT_ARRAY) {
        sp_IntArray *ia = (sp_IntArray *)v.v.p;
        mrb_int h = 0; if (ia) for (mrb_int i = 0; i < ia->len; i++) h = (h * 31) + ia->data[ia->start+i];
        return h;
      }
      if (v.cls_id == SP_BUILTIN_METHOD) {
        /* Method keys hash/eql by (bound self, fn ptr, name), so two
           `obj.method(:m)` instances collapse to one entry (optcarrot
           @peeks/@pokes dedup). The name disambiguates class methods, whose
           fn slot is 0 (no resolvable callable address). */
        sp_BoundMethod *m = (sp_BoundMethod *)v.v.p;
        if (!m) return 0;
        return (mrb_int)(((uintptr_t)m->self * 31) + m->fn) +
               (m->name ? (mrb_int)sp_str_hash(m->name) : 0);
      }
      if (sp_obj_hash_hook) return sp_obj_hash_hook(v.cls_id, v.v.p);
      return (mrb_int)((uintptr_t)v.v.p);
  }
  return 0;
}
static mrb_bool sp_rbval_eql_key(sp_RbVal a, sp_RbVal b) {
  if (a.tag != b.tag) return FALSE;
  switch (a.tag) {
    case SP_TAG_INT: case SP_TAG_BOOL: case SP_TAG_NIL: case SP_TAG_SYM:
      return a.v.i == b.v.i;
    case SP_TAG_BIGINT:
      return sp_bigint_cmp((sp_Bigint *)a.v.p, (sp_Bigint *)b.v.p) == 0;
    case SP_TAG_STR:
      if (a.v.s == b.v.s) return TRUE;
      if (!a.v.s || !b.v.s) return FALSE;
      return strcmp(a.v.s, b.v.s) == 0;
    case SP_TAG_ENCODING:
      if (a.v.s == b.v.s) return TRUE;
      if (!a.v.s || !b.v.s) return FALSE;
      return strcmp(a.v.s, b.v.s) == 0;
    case SP_TAG_FLT:
      return a.v.f == b.v.f;
    case SP_TAG_OBJ:
      if (a.cls_id != b.cls_id) return FALSE;
      if (a.v.p == b.v.p) return TRUE;
      if (a.cls_id == SP_BUILTIN_INT_ARRAY) {
        sp_IntArray *ia = (sp_IntArray *)a.v.p, *ib = (sp_IntArray *)b.v.p;
        if (!ia || !ib) return ia == ib;
        if (ia->len != ib->len) return FALSE;
        for (mrb_int i = 0; i < ia->len; i++) if (ia->data[ia->start+i] != ib->data[ib->start+i]) return FALSE;
        return TRUE;
      }
      if (a.cls_id == SP_BUILTIN_METHOD) {
        sp_BoundMethod *ma = (sp_BoundMethod *)a.v.p, *mb = (sp_BoundMethod *)b.v.p;
        if (!ma || !mb) return ma == mb;
        if (ma->self != mb->self || ma->fn != mb->fn) return FALSE;
        if (ma->name == mb->name) return TRUE;
        return ma->name && mb->name && strcmp(ma->name, mb->name) == 0;
      }
      if (sp_obj_eql_hook) return sp_obj_eql_hook(a.cls_id, a.v.p, b.v.p);
      return FALSE;
  }
  return FALSE;
}
/* dproc holds a Hash.new{} default block (arbitrary keys), lowered to a C fn
   with a boxed sp_RbVal key param. Called by _get on a miss. */
typedef sp_RbVal (*sp_polypoly_dproc_t)(sp_PolyPolyHash *, sp_RbVal, void *);
typedef struct sp_PolyPolyHash{sp_RbVal*keys;sp_RbVal*vals;mrb_int*order;mrb_bool*occ;mrb_int len;mrb_int cap;mrb_int mask;sp_RbVal default_v;sp_polypoly_dproc_t dproc;void *dproc_self;}sp_PolyPolyHash;
static void sp_PolyPolyHash_fin(void*p){sp_PolyPolyHash*h=(sp_PolyPolyHash*)p;free(h->keys);free(h->vals);free(h->order);free(h->occ);}
static void sp_PolyPolyHash_scan(void*p){sp_PolyPolyHash*h=(sp_PolyPolyHash*)p;for(mrb_int i=0;i<h->cap;i++){if(h->occ[i]){sp_mark_rbval(h->keys[i]);sp_mark_rbval(h->vals[i]);}}sp_mark_rbval(h->default_v);if(h->dproc_self)sp_gc_mark(h->dproc_self);}
static sp_PolyPolyHash*sp_PolyPolyHash_new(void){sp_PolyPolyHash*h=(sp_PolyPolyHash*)sp_gc_alloc(sizeof(sp_PolyPolyHash),sp_PolyPolyHash_fin,sp_PolyPolyHash_scan);h->cap=16;h->mask=15;h->keys=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->vals=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->order=(mrb_int*)malloc(sizeof(mrb_int)*h->cap);h->occ=(mrb_bool*)calloc(h->cap,sizeof(mrb_bool));h->len=0;h->default_v=sp_box_nil();return h;}
static sp_PolyPolyHash*sp_PolyPolyHash_new_with_default(sp_RbVal d){sp_PolyPolyHash*h=sp_PolyPolyHash_new();h->default_v=d;return h;}
static sp_PolyPolyHash*sp_PolyPolyHash_new_dproc(sp_polypoly_dproc_t fn,void*self){sp_PolyPolyHash*h=sp_PolyPolyHash_new();h->dproc=fn;h->dproc_self=self;return h;}
static void sp_PolyPolyHash_grow(sp_PolyPolyHash*h){sp_RbVal*ok=h->keys;sp_RbVal*ov=h->vals;mrb_bool*oo=h->occ;mrb_int*oord=h->order;mrb_int olen=h->len;h->cap*=2;h->mask=h->cap-1;h->keys=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->vals=(sp_RbVal*)calloc(h->cap,sizeof(sp_RbVal));h->order=(mrb_int*)malloc(sizeof(mrb_int)*h->cap);h->occ=(mrb_bool*)calloc(h->cap,sizeof(mrb_bool));for(mrb_int i=0;i<olen;i++){mrb_int oi=oord[i];sp_RbVal k=ok[oi];mrb_int idx=(mrb_int)(sp_rbval_hash_key(k)&h->mask);while(h->occ[idx])idx=(idx+1)&h->mask;h->keys[idx]=k;h->vals[idx]=ov[oi];h->occ[idx]=TRUE;h->order[i]=idx;}free(ok);free(ov);free(oo);free(oord);}
/* Miss path returns default_v, which is nil unless set via Hash.new(d) /
   Hash#default= -- so plain {} hashes keep surfacing Ruby nil (#801). */
/* miss path cold+noinline, same reason as sp_SymPolyHash_miss above. */
static SP_NOINLINE sp_RbVal sp_PolyPolyHash_miss(sp_PolyPolyHash*h,sp_RbVal k){if(h->dproc)return h->dproc(h,k,h->dproc_self);return h->default_v;}
static sp_RbVal sp_PolyPolyHash_get(sp_PolyPolyHash*h,sp_RbVal k){if(!h)return sp_box_nil();mrb_int idx=(mrb_int)(sp_rbval_hash_key(k)&h->mask);while(h->occ[idx]){if(sp_rbval_eql_key(h->keys[idx],k))return h->vals[idx];idx=(idx+1)&h->mask;}return sp_PolyPolyHash_miss(h,k);}
static sp_RbVal sp_poly_get_sym(sp_RbVal v, sp_sym key) {
  if (v.tag != SP_TAG_OBJ) return sp_box_nil();
  switch (v.cls_id) {
    case SP_BUILTIN_SYM_POLY_HASH: return sp_SymPolyHash_get((sp_SymPolyHash*)v.v.p, key);
    case SP_BUILTIN_POLY_POLY_HASH: return sp_PolyPolyHash_get((sp_PolyPolyHash*)v.v.p, sp_box_sym(key));
    default: return sp_box_nil();
  }
}
static void sp_PolyPolyHash_set(sp_PolyPolyHash*h,sp_RbVal k,sp_RbVal v){if(h->len*2>=h->cap)sp_PolyPolyHash_grow(h);mrb_int idx=(mrb_int)(sp_rbval_hash_key(k)&h->mask);while(h->occ[idx]){if(sp_rbval_eql_key(h->keys[idx],k)){h->vals[idx]=v;return;}idx=(idx+1)&h->mask;}h->keys[idx]=k;h->vals[idx]=v;h->occ[idx]=TRUE;h->order[h->len]=idx;h->len++;}
/* order[] holds slot indices (not keys), so iterate keys/vals by the stored
   index; merge inherits the LEFT receiver's default per CRuby. */
static sp_PolyPolyHash*sp_PolyPolyHash_merge(sp_PolyPolyHash*a,sp_PolyPolyHash*b){sp_PolyPolyHash*r=sp_PolyPolyHash_new();if(a){r->default_v=a->default_v;for(mrb_int i=0;i<a->len;i++){mrb_int idx=a->order[i];sp_PolyPolyHash_set(r,a->keys[idx],a->vals[idx]);}}if(b){for(mrb_int i=0;i<b->len;i++){mrb_int idx=b->order[i];sp_PolyPolyHash_set(r,b->keys[idx],b->vals[idx]);}}return r;}
static mrb_bool sp_PolyPolyHash_has_key(sp_PolyPolyHash*h,sp_RbVal k){mrb_int idx=(mrb_int)(sp_rbval_hash_key(k)&h->mask);while(h->occ[idx]){if(sp_rbval_eql_key(h->keys[idx],k))return TRUE;idx=(idx+1)&h->mask;}return FALSE;}
static mrb_int sp_PolyPolyHash_length(sp_PolyPolyHash*h){return h->len;}
static void sp_PolyPolyHash_clear(sp_PolyPolyHash*h){if(!h)return;for(mrb_int i=0;i<h->cap;i++)h->occ[i]=0;h->len=0;}
/* Hash#delete for a poly-keyed hash: was entirely missing (only the
   String/Symbol-keyed hash kinds had a delete), so `poly_poly_hash.
   delete(k)` hit codegen's "unsupported call" catch-all -- e.g. doom's
   `@active_doors.delete(sector_idx)` once @active_doors unifies to
   poly-keyed storage. Rebuild-and-swap rather than an in-place
   backward-shift: order[] here stores *slot indices* (unlike Str/
   SymPolyHash, which store keys directly), so a backward-shift would
   also need to renumber every order[] entry whose slot moved -- this is
   O(n) either way for a table this size, and much less error-prone. `tmp`
   is a GC-allocated shell; steal its arrays into `h` and null tmp's own
   fields so its finalizer (which frees h->keys et al by pointer) doesn't
   double-free the arrays h now owns when tmp is later collected. Root h
   and tmp for the rebuild (house pattern, cf. sp_StrIntHash_keys and the
   codegen compact arm): sp_rbval_eql_key/_hash_key can reach the user
   ==/hash hooks, whose generated code may allocate and collect -- an
   unrooted tmp would be swept (and finalized) mid-loop. */
static void sp_PolyPolyHash_delete(sp_PolyPolyHash*h,sp_RbVal k){
  if(!h||!sp_PolyPolyHash_has_key(h,k))return;
  SP_GC_ROOT(h);
  sp_PolyPolyHash*tmp=sp_PolyPolyHash_new();
  SP_GC_ROOT(tmp);
  for(mrb_int i=0;i<h->len;i++){
    mrb_int idx=h->order[i];
    if(!sp_rbval_eql_key(h->keys[idx],k))sp_PolyPolyHash_set(tmp,h->keys[idx],h->vals[idx]);
  }
  free(h->keys);free(h->vals);free(h->order);free(h->occ);
  h->keys=tmp->keys;h->vals=tmp->vals;h->order=tmp->order;h->occ=tmp->occ;
  h->len=tmp->len;h->cap=tmp->cap;h->mask=tmp->mask;
  tmp->keys=NULL;tmp->vals=NULL;tmp->order=NULL;tmp->occ=NULL;
}
static sp_RbVal sp_poly_get_str(sp_RbVal v, const char *key) {
  if (v.tag != SP_TAG_OBJ) return sp_box_nil();
  switch (v.cls_id) {
    case SP_BUILTIN_STR_POLY_HASH: return sp_StrPolyHash_get((sp_StrPolyHash*)v.v.p, key);
    case SP_BUILTIN_STR_STR_HASH: { const char *s = sp_StrStrHash_get((sp_StrStrHash*)v.v.p, key); return s ? sp_box_str(s) : sp_box_nil(); }
    case SP_BUILTIN_STR_INT_HASH: { mrb_int i = sp_StrIntHash_get_opt((sp_StrIntHash*)v.v.p, key); return i == SP_INT_NIL ? sp_box_nil() : sp_box_int(i); }
    case SP_BUILTIN_POLY_POLY_HASH: return sp_PolyPolyHash_get((sp_PolyPolyHash*)v.v.p, sp_box_str(key));
    default: return sp_box_nil();
  }
}
/* Extend sp_poly_arr_len for hash types defined after the initial declaration. */
static mrb_int sp_poly_arr_len_ex(sp_RbVal a) {
  if (a.tag != SP_TAG_OBJ) return 0;
  switch (a.cls_id) {
    case SP_BUILTIN_STR_POLY_HASH: return ((sp_StrPolyHash *)a.v.p)->len;
    case SP_BUILTIN_SYM_POLY_HASH: return ((sp_SymPolyHash *)a.v.p)->len;
    case SP_BUILTIN_POLY_POLY_HASH: return ((sp_PolyPolyHash *)a.v.p)->len;
    case SP_BUILTIN_RANGE: { sp_Range *r = (sp_Range *)a.v.p; mrb_int n = r->last - r->first + (r->excl ? 0 : 1); return n > 0 ? n : 0; }
    default: return sp_poly_arr_len(a);
  }
}
/* sp_poly_each_elem: return the i-th element for sequential each-iteration.
   For arrays: element at index i. For hashes: the i-th insertion-order
   key-value pair as a 2-element PolyArray so |k, v| block splat works. */
static sp_RbVal sp_poly_each_elem(sp_RbVal a, mrb_int i) {
  if (a.tag != SP_TAG_OBJ) return sp_box_nil();
  switch (a.cls_id) {
    case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_FLT_ARRAY:
    case SP_BUILTIN_STR_ARRAY: case SP_BUILTIN_POLY_ARRAY:
      return sp_poly_arr_get(a, i);
    case SP_BUILTIN_RANGE: { sp_Range *r = (sp_Range *)a.v.p; return sp_box_int(r->first + i); }
    case SP_BUILTIN_STR_INT_HASH: {
      sp_StrIntHash *h = (sp_StrIntHash*)a.v.p;
      if (!h || i < 0 || i >= h->len) return sp_box_nil();
      sp_PolyArray *pair = sp_PolyArray_new(); SP_GC_ROOT(pair);
      sp_PolyArray_push(pair, sp_box_str(h->order[i]));
      sp_PolyArray_push(pair, sp_box_int(sp_StrIntHash_get(h, h->order[i])));
      return sp_box_poly_array(pair); }
    case SP_BUILTIN_STR_STR_HASH: {
      sp_StrStrHash *h = (sp_StrStrHash*)a.v.p;
      if (!h || i < 0 || i >= h->len) return sp_box_nil();
      sp_PolyArray *pair = sp_PolyArray_new(); SP_GC_ROOT(pair);
      sp_PolyArray_push(pair, sp_box_str(h->order[i]));
      const char *sv = sp_StrStrHash_get(h, h->order[i]);
      sp_PolyArray_push(pair, sv ? sp_box_str(sv) : sp_box_nil());
      return sp_box_poly_array(pair); }
    case SP_BUILTIN_STR_POLY_HASH: {
      sp_StrPolyHash *h = (sp_StrPolyHash*)a.v.p;
      if (!h || i < 0 || i >= h->len) return sp_box_nil();
      sp_PolyArray *pair = sp_PolyArray_new(); SP_GC_ROOT(pair);
      sp_PolyArray_push(pair, sp_box_str(h->order[i]));
      sp_PolyArray_push(pair, sp_StrPolyHash_get(h, h->order[i]));
      return sp_box_poly_array(pair); }
    case SP_BUILTIN_SYM_POLY_HASH: {
      sp_SymPolyHash *h = (sp_SymPolyHash*)a.v.p;
      if (!h || i < 0 || i >= h->len) return sp_box_nil();
      sp_PolyArray *pair = sp_PolyArray_new(); SP_GC_ROOT(pair);
      sp_PolyArray_push(pair, sp_box_sym(h->order[i]));
      sp_PolyArray_push(pair, sp_SymPolyHash_get(h, h->order[i]));
      return sp_box_poly_array(pair); }
    case SP_BUILTIN_INT_STR_HASH: {
      sp_IntStrHash *h = (sp_IntStrHash*)a.v.p;
      if (!h || i < 0 || i >= h->len) return sp_box_nil();
      sp_PolyArray *pair = sp_PolyArray_new(); SP_GC_ROOT(pair);
      sp_PolyArray_push(pair, sp_box_int(h->order[i]));
      const char *iv = sp_IntStrHash_get(h, h->order[i]);
      sp_PolyArray_push(pair, iv ? sp_box_str(iv) : sp_box_nil());
      return sp_box_poly_array(pair); }
    case SP_BUILTIN_POLY_POLY_HASH: {
      sp_PolyPolyHash *h = (sp_PolyPolyHash*)a.v.p;
      if (!h || i < 0 || i >= h->len) return sp_box_nil();
      mrb_int idx = h->order[i];
      sp_PolyArray *pair = sp_PolyArray_new(); SP_GC_ROOT(pair);
      sp_PolyArray_push(pair, h->keys[idx]);
      sp_PolyArray_push(pair, h->vals[idx]);
      return sp_box_poly_array(pair); }
    default: return sp_box_nil();
  }
}
/* poly_arr_get/set for PolyPolyHash with integer index key. */
/* multi-assign element read: `a, b = v` destructures only when the boxed
   value is an Array (Ruby's to_ary semantics); any other runtime kind is a
   scalar -- the first target takes the whole value, the rest nil-fill.
   sp_poly_arr_get_hash is NOT that (Integer#[i] reads a bit, String#[i] a
   char, Hash#[i] a lookup). */
static sp_RbVal sp_poly_massign_get(sp_RbVal v, mrb_int i) {
  if (v.tag == SP_TAG_OBJ) switch (v.cls_id) {
    case SP_BUILTIN_POLY_ARRAY: case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_SYM_ARRAY:
    case SP_BUILTIN_STR_ARRAY: case SP_BUILTIN_FLT_ARRAY:
      return sp_poly_arr_get(v, i);
    default: break;
  }
  return i == 0 ? v : sp_box_nil();
}
static sp_RbVal sp_poly_arr_get_hash(sp_RbVal a, mrb_int i) {
  if (a.tag == SP_TAG_INT) return sp_box_int((a.v.i >> i) & 1);
  /* String#[int]: return the single character at i (a 1-char string), or nil
     when out of range. A String that widened to poly (e.g. a method with
     multiple return paths) reaches this generic index path; without this arm
     it fell through to sp_poly_arr_get and silently returned nil. */
  if (a.tag == SP_TAG_STR) {
    const char *s = a.v.s ? a.v.s : "";
    mrb_int cl = sp_str_length(s);
    if (i < 0) i += cl;
    if (i < 0 || i >= cl) return sp_box_nil();
    return sp_box_str(sp_str_sub_range(s, i, 1));
  }
  if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_POLY_POLY_HASH)
    return sp_PolyPolyHash_get((sp_PolyPolyHash*)a.v.p, sp_box_int(i));
  if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_SYM_POLY_HASH)
    return sp_SymPolyHash_get((sp_SymPolyHash*)a.v.p, (sp_sym)i);
  if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_STR_POLY_HASH)
    return sp_StrPolyHash_get((sp_StrPolyHash*)a.v.p, sp_sym_name_fn ? sp_sym_name_fn((sp_sym)i) : "");
  /* bm[arg]: a boxed bound Method called with the (single) int argument. */
  if (a.tag == SP_TAG_OBJ && a.cls_id == SP_BUILTIN_METHOD) {
    sp_BoundMethod *m = (sp_BoundMethod *)a.v.p;
    return sp_box_int(((mrb_int (*)(void *, mrb_int))(uintptr_t)m->fn)((void *)m->self, i));
  }
  return sp_poly_arr_get(a, i);
}
/* poly[poly_key]: dispatch on key tag at runtime. */
static sp_RbVal sp_poly_index_poly(sp_RbVal recv, sp_RbVal idx) {
  /* heterogeneous-key hash: any key kind (incl. Method) looks up directly. */
  if (recv.tag == SP_TAG_OBJ && recv.cls_id == SP_BUILTIN_POLY_POLY_HASH)
    return sp_PolyPolyHash_get((sp_PolyPolyHash *)recv.v.p, idx);
  if (idx.tag == SP_TAG_STR) return sp_poly_get_str(recv, idx.v.s);
  if (idx.tag == SP_TAG_SYM) return sp_poly_get_sym(recv, (sp_sym)idx.v.i);
  mrb_int i = (idx.tag == SP_TAG_INT) ? idx.v.i : 0;
  return sp_poly_arr_get_hash(recv, i);
}

/* Presence check for a Hash reached through a poly value, keyed by a poly key.
   The dispatch mirrors sp_poly_index_poly's storage kinds so `fetch` can tell a
   present key (return the value) from an absent one (default / KeyError) --
   sp_poly_index_poly alone returns nil on a miss, indistinguishable from a key
   legitimately mapped to nil. A key whose tag does not match the storage's key
   kind can never be present, so it reports FALSE. */
static mrb_bool sp_poly_has_key(sp_RbVal recv, sp_RbVal key) {
  if (recv.tag != SP_TAG_OBJ) return FALSE;
  switch (recv.cls_id) {
    case SP_BUILTIN_POLY_POLY_HASH: return sp_PolyPolyHash_has_key((sp_PolyPolyHash *)recv.v.p, key);
    case SP_BUILTIN_STR_POLY_HASH:  return key.tag == SP_TAG_STR && sp_StrPolyHash_has_key((sp_StrPolyHash *)recv.v.p, key.v.s);
    case SP_BUILTIN_STR_STR_HASH:   return key.tag == SP_TAG_STR && sp_StrStrHash_has_key((sp_StrStrHash *)recv.v.p, key.v.s);
    case SP_BUILTIN_STR_INT_HASH:   return key.tag == SP_TAG_STR && sp_StrIntHash_has_key((sp_StrIntHash *)recv.v.p, key.v.s);
    case SP_BUILTIN_SYM_POLY_HASH:  return key.tag == SP_TAG_SYM && sp_SymPolyHash_has_key((sp_SymPolyHash *)recv.v.p, (sp_sym)key.v.i);
    case SP_BUILTIN_INT_STR_HASH:   return key.tag == SP_TAG_INT && sp_IntStrHash_has_key((sp_IntStrHash *)recv.v.p, key.v.i);
    default: return FALSE;
  }
}

/* Integer-returning counterpart of sp_poly_index_poly for `poly[int]` where
   the poly element holds an int-returning callable/container -- a bound
   method (called with the int arg, int ABI) or an int array. Used when
   inference proves the double index `@table[i][j]` yields an int (a method
   dispatch table). Falls back to coercing the generic poly result. */
/* frozen? on a poly value: scalars (int/float/sym/bool/nil/bigint) are always
   frozen in Ruby; a string checks its frozen flag; a heap object checks its GC
   header bit. Used when a receiver widened to poly under promote. */
static inline mrb_bool sp_poly_frozen(sp_RbVal v) {
  if (v.tag == SP_TAG_STR) return v.v.s ? sp_str_is_frozen_val(v.v.s) : TRUE;
  if (v.tag == SP_TAG_OBJ) return sp_gc_is_frozen(v.v.p);
  return TRUE;
}
/* eql? for a poly value: like == but without cross-kind numeric coercion, so
   1.eql?(1.0) is false while 1 == 1.0 is true. Every other type answers as ==.
   Backs the universal `x.should.eql?(y)` matcher on a poly receiver. */
static mrb_bool sp_poly_eql(sp_RbVal a, sp_RbVal b) {
  int a_int = (a.tag == SP_TAG_INT || a.tag == SP_TAG_BIGINT);
  int b_int = (b.tag == SP_TAG_INT || b.tag == SP_TAG_BIGINT);
  if ((a_int && b.tag == SP_TAG_FLT) || (a.tag == SP_TAG_FLT && b_int)) return FALSE;
  return sp_poly_eq(a, b);
}
/* equal? for a poly value: object identity. Immediates (int, symbol, nil,
   bool, flonum) are their own identity by value; everything heap-backed
   (string buffer, boxed object, bignum) compares by pointer. */
static mrb_bool sp_poly_equal(sp_RbVal a, sp_RbVal b) {
  if (a.tag != b.tag) return FALSE;
  switch (a.tag) {
    case SP_TAG_INT: return a.v.i == b.v.i;
    case SP_TAG_SYM: return a.v.i == b.v.i;
    case SP_TAG_BOOL: return a.v.b == b.v.b;
    case SP_TAG_NIL: return TRUE;
    case SP_TAG_FLT: return a.v.f == b.v.f;
    case SP_TAG_STR: return a.v.s == b.v.s;
    /* Class objects are their own identity: two references to Object are the
       same object. Spinel's classes are name-backed, so compare by name. */
    case SP_TAG_CLASS: { const char *an = sp_class_val_name(a), *bn = sp_class_val_name(b);
                         return (an && bn) ? strcmp(an, bn) == 0 : an == bn; }
    default: return a.v.p == b.v.p;
  }
}
/* is_a?/kind_of? for a poly value against a BUILTIN class named `cn`. The
   caller (codegen) routes here only when `cn` is a known builtin; a user-class
   target is resolved inline via sp_class_le on the boxed object's cls_id. */
static mrb_bool sp_poly_kind_of_builtin(sp_RbVal v, const char *cn) {
  if (!cn) return FALSE;
  if (strcmp(cn, "Object") == 0 || strcmp(cn, "BasicObject") == 0 || strcmp(cn, "Kernel") == 0)
    return TRUE;
  if (strcmp(sp_poly_class_name(v), cn) == 0) return TRUE;  /* exact builtin class */
  int is_int = (v.tag == SP_TAG_INT || v.tag == SP_TAG_BIGINT);
  int is_flt = (v.tag == SP_TAG_FLT);
  int is_rat = (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_RATIONAL);
  int is_cpx = (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_COMPLEX);
  int is_arr = (v.tag == SP_TAG_OBJ && sp_poly_is_array_kind(v.cls_id));
  int is_range = (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_RANGE);
  int is_hash = (v.tag == SP_TAG_OBJ &&
                 (v.cls_id == SP_BUILTIN_POLY_POLY_HASH || v.cls_id == SP_BUILTIN_SYM_POLY_HASH ||
                  v.cls_id == SP_BUILTIN_STR_POLY_HASH || v.cls_id == SP_BUILTIN_STR_STR_HASH ||
                  v.cls_id == SP_BUILTIN_STR_INT_HASH || v.cls_id == SP_BUILTIN_INT_STR_HASH));
  if (strcmp(cn, "Numeric") == 0) return is_int || is_flt || is_rat || is_cpx;
  if (strcmp(cn, "Integer") == 0) return is_int;
  if (strcmp(cn, "Float") == 0) return is_flt;
  if (strcmp(cn, "Comparable") == 0) return is_int || is_flt || is_rat ||
                                             v.tag == SP_TAG_STR || v.tag == SP_TAG_SYM;
  if (strcmp(cn, "Enumerable") == 0) return is_arr || is_range || is_hash;
  return FALSE;
}
/* is_a?/instance_of? for a poly value against a RUNTIME class value `cls` (a
   method param or other non-literal). The class's name drives the check: exact
   name match for instance_of? (and same-class is_a?), plus the builtin-ancestry
   table for is_a?. A dynamic user-class ancestor is out of reach here, so this
   under-reports at worst (never a false positive) -- the safe direction. */
static mrb_bool sp_poly_is_a_dyn(sp_RbVal v, sp_RbVal cls, int exact) {
  const char *cn = sp_poly_to_s(cls);
  if (!cn) return FALSE;
  if (strcmp(sp_poly_class_name(v), cn) == 0) return TRUE;
  return exact ? FALSE : sp_poly_kind_of_builtin(v, cn);
}
static inline mrb_int sp_poly_index_int(sp_RbVal a, mrb_int i) {
  if (a.tag == SP_TAG_INT) return (a.v.i >> i) & 1;
  if (a.tag == SP_TAG_OBJ) {
    if (a.cls_id == SP_BUILTIN_METHOD) {
      sp_BoundMethod *m = (sp_BoundMethod *)a.v.p;
#ifdef SP_INT_OVERFLOW_MODE_PROMOTE
      /* promote: methods are poly-signatured, so invoke through the poly ABI
         and unbox the result rather than the legacy mrb_int ABI. */
      return sp_poly_to_i(((sp_RbVal (*)(void *, sp_RbVal))(uintptr_t)m->fn)((void *)m->self, sp_box_int(i)));
#else
      return ((mrb_int (*)(void *, mrb_int))(uintptr_t)m->fn)((void *)m->self, i);
#endif
    }
    if (a.cls_id == SP_BUILTIN_INT_ARRAY) return sp_IntArray_get((sp_IntArray *)a.v.p, i);
  }
  return sp_poly_to_i(sp_poly_arr_get_hash(a, i));
}
static sp_RbVal sp_poly_arr_set_hash(sp_RbVal v, mrb_int idx, sp_RbVal val) {
  if (v.tag != SP_TAG_OBJ) return val;
  switch (v.cls_id) {
    case SP_BUILTIN_INT_ARRAY:  sp_IntArray_set((sp_IntArray*)v.v.p, idx,
                                                val.tag == SP_TAG_INT ? val.v.i : (mrb_int)val.v.f); break;
    case SP_BUILTIN_FLT_ARRAY:  sp_FloatArray_set((sp_FloatArray*)v.v.p, idx,
                                                   val.tag == SP_TAG_FLT ? val.v.f : (mrb_float)val.v.i); break;
    case SP_BUILTIN_STR_ARRAY:  sp_StrArray_set((sp_StrArray*)v.v.p, idx,
                                                 val.tag == SP_TAG_STR ? val.v.s : NULL); break;
    case SP_BUILTIN_POLY_ARRAY: {
      sp_PolyArray *_pa = (sp_PolyArray*)v.v.p;
      if (_pa && !_pa->frozen) {
        while (_pa->len <= idx) sp_PolyArray_push(_pa, sp_box_nil());
        _pa->data[idx] = val;
      }
      break;
    }
    case SP_BUILTIN_POLY_POLY_HASH: sp_PolyPolyHash_set((sp_PolyPolyHash*)v.v.p, sp_box_int(idx), val); break;
    default: break;
  }
  return val;
}
/* poly_val[str_key] = val: runtime dispatch for poly recv `[]=` with string key. */
static sp_RbVal sp_poly_set_str(sp_RbVal v, const char *key, sp_RbVal val) {
  if (v.tag != SP_TAG_OBJ) return val;
  switch (v.cls_id) {
    case SP_BUILTIN_STR_POLY_HASH: sp_StrPolyHash_set((sp_StrPolyHash*)v.v.p, key, val); break;
    case SP_BUILTIN_STR_STR_HASH:
      if (val.tag == SP_TAG_STR) { sp_StrStrHash_set((sp_StrStrHash*)v.v.p, key, val.v.s); } break;
    case SP_BUILTIN_STR_INT_HASH:
      if (val.tag == SP_TAG_INT) { sp_StrIntHash_set((sp_StrIntHash*)v.v.p, key, val.v.i); } break;
    case SP_BUILTIN_POLY_POLY_HASH: sp_PolyPolyHash_set((sp_PolyPolyHash*)v.v.p, sp_box_str(key), val); break;
    default: break;
  }
  return val;
}
/* poly_val[sym_key] = val: runtime dispatch for poly recv `[]=` with symbol key. */
static sp_RbVal sp_poly_set_sym(sp_RbVal v, sp_sym key, sp_RbVal val) {
  if (v.tag != SP_TAG_OBJ) return val;
  switch (v.cls_id) {
    case SP_BUILTIN_SYM_POLY_HASH:  sp_SymPolyHash_set((sp_SymPolyHash*)v.v.p, key, val); break;
    case SP_BUILTIN_POLY_POLY_HASH: sp_PolyPolyHash_set((sp_PolyPolyHash*)v.v.p, sp_box_sym(key), val); break;
    default: break;
  }
  return val;
}
/* poly_val[int_idx] = val: runtime dispatch for poly recv `[]=` with int index. */
static sp_RbVal sp_poly_arr_set(sp_RbVal v, mrb_int idx, sp_RbVal val) {
  if (v.tag != SP_TAG_OBJ) return val;
  switch (v.cls_id) {
    case SP_BUILTIN_INT_ARRAY:  sp_IntArray_set((sp_IntArray*)v.v.p, idx,
                                                val.tag == SP_TAG_INT ? val.v.i : (mrb_int)val.v.f); break;
    case SP_BUILTIN_FLT_ARRAY:  sp_FloatArray_set((sp_FloatArray*)v.v.p, idx,
                                                   val.tag == SP_TAG_FLT ? val.v.f : (mrb_float)val.v.i); break;
    case SP_BUILTIN_STR_ARRAY:  sp_StrArray_set((sp_StrArray*)v.v.p, idx,
                                                 val.tag == SP_TAG_STR ? val.v.s : NULL); break;
    case SP_BUILTIN_POLY_ARRAY: sp_PolyArray_set((sp_PolyArray*)v.v.p, idx, val); break;
    default: break;
  }
  return val;
}
/* Like sp_poly_arr_set but widens a typed array to a PolyArray when val does not
   match its element kind (int<-non-int incl. float, flt<-non-float, str<-non-str),
   so the value is stored exactly as CRuby does (e.g. a Float into a former int
   array). Returns the (possibly new) boxed array so the caller updates the slot. */
static sp_RbVal sp_poly_arr_widen_and_set(sp_RbVal v, mrb_int idx, sp_RbVal val) {
  if (v.tag == SP_TAG_OBJ &&
      ((v.cls_id == SP_BUILTIN_INT_ARRAY && val.tag != SP_TAG_INT) ||
       (v.cls_id == SP_BUILTIN_FLT_ARRAY && val.tag != SP_TAG_FLT) ||
       (v.cls_id == SP_BUILTIN_STR_ARRAY && val.tag != SP_TAG_STR))) {
    if (sp_typed_arr_frozen(v)) sp_raise_frozen_array();
    SP_GC_ROOT_RBVAL(val);
    sp_PolyArray *pa = sp_poly_to_poly_array(v);
    SP_GC_ROOT(pa);
    sp_PolyArray_set(pa, idx, val);
    return sp_box_poly_array(pa);
  }
  sp_poly_arr_set(v, idx, val);
  return v;
}
/* poly_val[poly_key] = val: fully dynamic dispatch for poly recv + poly key. */
static sp_RbVal sp_poly_set_poly(sp_RbVal v, sp_RbVal key, sp_RbVal val) {
  if (v.tag != SP_TAG_OBJ) return val;
  switch (v.cls_id) {
    case SP_BUILTIN_STR_POLY_HASH:
      if (key.tag == SP_TAG_STR) sp_StrPolyHash_set((sp_StrPolyHash*)v.v.p, key.v.s, val);
      break;
    case SP_BUILTIN_STR_STR_HASH:
      if (key.tag == SP_TAG_STR && val.tag == SP_TAG_STR)
        sp_StrStrHash_set((sp_StrStrHash*)v.v.p, key.v.s, val.v.s);
      break;
    case SP_BUILTIN_STR_INT_HASH:
      if (key.tag == SP_TAG_STR && val.tag == SP_TAG_INT)
        sp_StrIntHash_set((sp_StrIntHash*)v.v.p, key.v.s, val.v.i);
      break;
    case SP_BUILTIN_SYM_POLY_HASH:
      if (key.tag == SP_TAG_SYM) sp_SymPolyHash_set((sp_SymPolyHash*)v.v.p, (sp_sym)key.v.i, val);
      break;
    case SP_BUILTIN_INT_ARRAY:
      if (key.tag == SP_TAG_INT) sp_IntArray_set((sp_IntArray*)v.v.p, key.v.i,
                                                  val.tag == SP_TAG_INT ? val.v.i : (mrb_int)val.v.f);
      break;
    case SP_BUILTIN_POLY_ARRAY:
      if (key.tag == SP_TAG_INT) sp_PolyArray_set((sp_PolyArray*)v.v.p, key.v.i, val);
      break;
    case SP_BUILTIN_POLY_POLY_HASH: sp_PolyPolyHash_set((sp_PolyPolyHash*)v.v.p, key, val); break;
    default: break;
  }
  return val;
}
/* `outer[oidx][start,len] = src` where the splice receiver is itself an index
   expression: read the inner array, promoting-splice it, and store the possibly
   promoted result back into outer's slot so a typed->poly promotion survives.
   outer is a POLY_ARRAY for an array-of-arrays; sp_poly_set_poly also covers a
   hash outer. Codegen yields the rhs separately, so the return is advisory. */
static sp_RbVal sp_poly_slot_splice(sp_RbVal outer, mrb_int oidx, mrb_int start, mrb_int len, sp_RbVal src) {
  sp_RbVal inner = sp_poly_arr_get(outer, oidx);
  sp_RbVal res = sp_poly_splice(inner, start, len, src);
  sp_poly_set_poly(outer, sp_box_int(oidx), res);
  return res;
}
static sp_RbVal sp_poly_slot_splice_range(sp_RbVal outer, mrb_int oidx, sp_Range r, sp_RbVal src) {
  sp_RbVal inner = sp_poly_arr_get(outer, oidx);
  sp_RbVal res = sp_poly_splice_range(inner, r, src);
  sp_poly_set_poly(outer, sp_box_int(oidx), res);
  return res;
}
/* `outer[oidx][ikey] = val` single-index assign through an index-expression
   receiver: read inner, widen-and-set (promoting on element-kind mismatch), and
   store the possibly promoted result back into outer's slot. No GC root spans
   the calls: outer stays live via the caller's rooted local, and src/val are
   rooted inside the callee only where an allocation (promotion) happens, after
   every raise condition has been checked. */
static sp_RbVal sp_poly_slot_set(sp_RbVal outer, mrb_int oidx, mrb_int ikey, sp_RbVal val) {
  sp_RbVal inner = sp_poly_arr_get(outer, oidx);
  sp_RbVal res = sp_poly_arr_widen_and_set(inner, ikey, val);
  sp_poly_set_poly(outer, sp_box_int(oidx), res);
  return val;
}
/* Hash#compare_by_identity? for a poly-carried receiver: spinel hashes are
   always value-keyed (the mutating variant is a compile error), so any hash
   answers false; anything else raises CRuby's NoMethodError. */
static mrb_bool sp_poly_cbi_p(sp_RbVal v) __attribute__((unused));
static mrb_bool sp_poly_cbi_p(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ) {
    switch (v.cls_id) {
    case SP_BUILTIN_STR_INT_HASH: case SP_BUILTIN_STR_STR_HASH:
    case SP_BUILTIN_INT_STR_HASH: case SP_BUILTIN_STR_POLY_HASH:
    case SP_BUILTIN_SYM_POLY_HASH: case SP_BUILTIN_POLY_POLY_HASH:
      return FALSE;
    default: break;
    }
  }
  sp_raise_cls("NoMethodError", "undefined method 'compare_by_identity?' for poly");
  return FALSE;
}
static mrb_int sp_poly_length(sp_RbVal v){if(v.tag==SP_TAG_STR)return v.v.s?(mrb_int)strlen(v.v.s):0;if(v.tag==SP_TAG_SYM)return sp_sym_name_fn?(mrb_int)strlen(sp_sym_name_fn((sp_sym)v.v.i)):0;if(v.tag!=SP_TAG_OBJ)return 0;switch(v.cls_id){case SP_BUILTIN_INT_ARRAY:return sp_IntArray_length((sp_IntArray*)v.v.p);case SP_BUILTIN_FLT_ARRAY:return sp_FloatArray_length((sp_FloatArray*)v.v.p);case SP_BUILTIN_STR_ARRAY:return sp_StrArray_length((sp_StrArray*)v.v.p);case SP_BUILTIN_SYM_ARRAY:return sp_IntArray_length((sp_IntArray*)v.v.p);case SP_BUILTIN_POLY_ARRAY:return sp_PolyArray_length((sp_PolyArray*)v.v.p);case SP_BUILTIN_STR_INT_HASH:return sp_StrIntHash_length((sp_StrIntHash*)v.v.p);case SP_BUILTIN_STR_STR_HASH:return sp_StrStrHash_length((sp_StrStrHash*)v.v.p);case SP_BUILTIN_INT_STR_HASH:return sp_IntStrHash_length((sp_IntStrHash*)v.v.p);case SP_BUILTIN_STR_POLY_HASH:return sp_StrPolyHash_length((sp_StrPolyHash*)v.v.p);case SP_BUILTIN_SYM_POLY_HASH:return sp_SymPolyHash_length((sp_SymPolyHash*)v.v.p);case SP_BUILTIN_POLY_POLY_HASH:return sp_PolyPolyHash_length((sp_PolyPolyHash*)v.v.p);default:return 0;}}

/* Marshal implementation moved to lib/sp_marshal.c. These small wrappers give
   the standalone serializer construction primitives that need sp_runtime.h
   types; sp_re_init (codegen) installs them into sp_marshal_v along with the
   generated sym_intern / obj_dump / obj_load. */
static sp_RbVal sp_marv_arr_new(void) { return sp_box_poly_array(sp_PolyArray_new()); }
static void sp_marv_arr_push(sp_RbVal a, sp_RbVal v) { sp_PolyArray_push((sp_PolyArray *)a.v.p, v); }
static sp_RbVal sp_marv_hash_new(void) { return sp_box_obj(sp_PolyPolyHash_new(), SP_BUILTIN_POLY_POLY_HASH); }
static void sp_marv_hash_set(sp_RbVal h, sp_RbVal k, sp_RbVal v) { sp_PolyPolyHash_set((sp_PolyPolyHash *)h.v.p, k, v); }
static sp_RbVal sp_marv_box_complex(mrb_float re, mrb_float im) { sp_Complex c; c.re = re; c.im = im; return sp_box_complex(c); }
static sp_RbVal sp_marv_box_rational(mrb_int n, mrb_int d) { return sp_box_rational(sp_rational_new(n, d)); }
static void sp_marv_raise(const char *cls, const char *msg) { sp_raise_cls(cls, msg); }
/* Array-reduction methods on a boxed array value -- an element of a poly array,
   e.g. a run produced by chunk_while / slice_when. Each switches on the boxed
   element's cls_id and returns a boxed result, so `runs.map { |r| r.sum }` and
   friends work without statically knowing the run's array type. first/last reuse
   the generic boxed-element accessors. */
static sp_RbVal sp_poly_sum(sp_RbVal v) {
  if (v.tag != SP_TAG_OBJ) return sp_box_int(0);
  switch (v.cls_id) {
    case SP_BUILTIN_INT_ARRAY:  return sp_box_int(sp_IntArray_sum((sp_IntArray *)v.v.p, 0));
    case SP_BUILTIN_FLT_ARRAY:  return sp_box_float(sp_FloatArray_sum((sp_FloatArray *)v.v.p, 0.0));
    case SP_BUILTIN_POLY_ARRAY: return sp_box_int(sp_PolyArray_sum_int((sp_PolyArray *)v.v.p));
    default: return sp_box_int(0);
  }
}
static sp_RbVal sp_poly_min(sp_RbVal v) {
  if (v.tag != SP_TAG_OBJ) return sp_box_nil();
  switch (v.cls_id) {
    case SP_BUILTIN_INT_ARRAY:  { sp_IntArray *a = (sp_IntArray *)v.v.p; return (a && a->len) ? sp_box_int(sp_IntArray_min(a)) : sp_box_nil(); }
    case SP_BUILTIN_FLT_ARRAY:  { sp_FloatArray *a = (sp_FloatArray *)v.v.p; return (a && a->len) ? sp_box_float(sp_FloatArray_min(a)) : sp_box_nil(); }
    case SP_BUILTIN_POLY_ARRAY: return sp_PolyArray_min((sp_PolyArray *)v.v.p);
    default: return sp_box_nil();
  }
}
static sp_RbVal sp_poly_max(sp_RbVal v) {
  if (v.tag != SP_TAG_OBJ) return sp_box_nil();
  switch (v.cls_id) {
    case SP_BUILTIN_INT_ARRAY:  { sp_IntArray *a = (sp_IntArray *)v.v.p; return (a && a->len) ? sp_box_int(sp_IntArray_max(a)) : sp_box_nil(); }
    case SP_BUILTIN_FLT_ARRAY:  { sp_FloatArray *a = (sp_FloatArray *)v.v.p; return (a && a->len) ? sp_box_float(sp_FloatArray_max(a)) : sp_box_nil(); }
    case SP_BUILTIN_POLY_ARRAY: return sp_PolyArray_max((sp_PolyArray *)v.v.p);
    default: return sp_box_nil();
  }
}
static sp_RbVal sp_poly_first(sp_RbVal v) {
  if (v.tag != SP_TAG_OBJ) return sp_box_nil();
  return sp_poly_arr_get(v, 0);
}
static sp_RbVal sp_poly_last(sp_RbVal v) {
  mrb_int n = sp_poly_length(v);
  return n > 0 ? sp_poly_arr_get(v, n - 1) : sp_box_nil();
}
static sp_RbVal sp_poly_sample(sp_RbVal v) {
  mrb_int n = sp_poly_length(v);
  return n > 0 ? sp_poly_arr_get(v, (mrb_int)(rand() % n)) : sp_box_nil();
}
/* Thread#value / #join through a poly slot. A Thread is modelled as a Fiber run
   to completion (single-threaded); when one is carried in a poly value -- e.g.
   an array of Threads, `(1..n).map { Thread.new { ... } }` -- #value/#join must
   dispatch at runtime on the boxed Fiber. value/resume return the block's
   result; join runs it and returns the thread (self). A non-Fiber poly returns
   nil, matching the NoMethodError gate for an unknown poly method. */
static sp_RbVal sp_poly_fiber_value(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_THREAD)
    return sp_Thread_value((sp_thread *)v.v.p);   /* a green thread carried in a poly slot */
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_FIBER) {
    sp_Fiber *f = (sp_Fiber *)v.v.p;
    if (f->state == 3) return f->yielded_value;   /* already run: cached result, idempotent */
    return sp_Fiber_resume(f, sp_box_nil());
  }
  return sp_box_nil();
}
static sp_RbVal sp_poly_fiber_join(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_THREAD) {
    sp_Thread_join((sp_thread *)v.v.p);
    return v;
  }
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_FIBER) {
    sp_Fiber *f = (sp_Fiber *)v.v.p;
    if (f->state != 3) sp_Fiber_resume(f, sp_box_nil());
  }
  return v;
}
static sp_PolyArray*sp_PolyPolyHash_keys(sp_PolyPolyHash*h){SP_GC_ROOT(h);sp_PolyArray*a=sp_PolyArray_new();SP_GC_ROOT(a);for(mrb_int i=0;i<h->len;i++)sp_PolyArray_push(a,h->keys[h->order[i]]);return a;}
static sp_PolyArray*sp_PolyPolyHash_values(sp_PolyPolyHash*h){SP_GC_ROOT(h);sp_PolyArray*a=sp_PolyArray_new();SP_GC_ROOT(a);for(mrb_int i=0;i<h->len;i++)sp_PolyArray_push(a,h->vals[h->order[i]]);return a;}

/* Hash#keys / #values on a poly receiver -- e.g. an evidence-free empty `{}`
   that stayed poly, or a hash read back out of a poly slot. Dispatch on the
   runtime hash variant, returning a poly array of the (boxed) keys or values.
   A non-hash poly value raises NoMethodError, as CRuby does for keys/values.
   Each typed intermediate is rooted before the converter allocates. */
static sp_PolyArray *sp_poly_keys(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ) switch (v.cls_id) {
    case SP_BUILTIN_STR_INT_HASH:  { sp_StrArray *k = sp_StrIntHash_keys((sp_StrIntHash*)v.v.p); SP_GC_ROOT(k); return sp_PolyArray_from_str_array(k); }
    case SP_BUILTIN_STR_STR_HASH:  { sp_StrArray *k = sp_StrStrHash_keys((sp_StrStrHash*)v.v.p); SP_GC_ROOT(k); return sp_PolyArray_from_str_array(k); }
    case SP_BUILTIN_INT_STR_HASH:  { sp_IntArray *k = sp_IntStrHash_keys((sp_IntStrHash*)v.v.p); SP_GC_ROOT(k); return sp_PolyArray_from_int_array(k); }
    case SP_BUILTIN_STR_POLY_HASH: { sp_StrArray *k = sp_StrPolyHash_keys((sp_StrPolyHash*)v.v.p); SP_GC_ROOT(k); return sp_PolyArray_from_str_array(k); }
    case SP_BUILTIN_SYM_POLY_HASH: { sp_IntArray *k = sp_SymPolyHash_keys((sp_SymPolyHash*)v.v.p); SP_GC_ROOT(k); sp_PolyArray *a = sp_PolyArray_new(); SP_GC_ROOT(a); for (mrb_int i = 0; i < k->len; i++) sp_PolyArray_push(a, sp_box_sym((sp_sym)k->data[k->start + i])); return a; }
    case SP_BUILTIN_POLY_POLY_HASH: return sp_PolyPolyHash_keys((sp_PolyPolyHash*)v.v.p);
  }
  sp_raise_cls("NoMethodError", "undefined method 'keys'");
  return NULL;  /* unreachable: sp_raise_cls is noreturn */
}
static sp_PolyArray *sp_poly_values(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ) switch (v.cls_id) {
    case SP_BUILTIN_STR_INT_HASH:  { sp_IntArray *vv = sp_StrIntHash_values((sp_StrIntHash*)v.v.p); SP_GC_ROOT(vv); return sp_PolyArray_from_int_array(vv); }
    case SP_BUILTIN_STR_STR_HASH:  { sp_StrArray *vv = sp_StrStrHash_values((sp_StrStrHash*)v.v.p); SP_GC_ROOT(vv); return sp_PolyArray_from_str_array(vv); }
    case SP_BUILTIN_INT_STR_HASH:  { sp_StrArray *vv = sp_IntStrHash_values((sp_IntStrHash*)v.v.p); SP_GC_ROOT(vv); return sp_PolyArray_from_str_array(vv); }
    case SP_BUILTIN_STR_POLY_HASH: return sp_StrPolyHash_values((sp_StrPolyHash*)v.v.p);
    case SP_BUILTIN_SYM_POLY_HASH: return sp_SymPolyHash_values((sp_SymPolyHash*)v.v.p);
    case SP_BUILTIN_POLY_POLY_HASH: return sp_PolyPolyHash_values((sp_PolyPolyHash*)v.v.p);
  }
  sp_raise_cls("NoMethodError", "undefined method 'values'");
  return NULL;  /* unreachable: sp_raise_cls is noreturn */
}
static sp_PolyPolyHash*sp_PolyPolyHash_dup(sp_PolyPolyHash*h){sp_PolyPolyHash*r=sp_PolyPolyHash_new();r->default_v=h->default_v;for(mrb_int i=0;i<h->len;i++)sp_PolyPolyHash_set(r,h->keys[h->order[i]],h->vals[h->order[i]]);return r;}
/* Issue #738: poly_poly_hash inspect using sp_poly_inspect on each
   k,v. Output mirrors Ruby's `{k => v, ...}` for non-symbol keys and
   `{k: v, ...}` shorthand for symbol keys. */
static const char *sp_poly_inspect(sp_RbVal v);
static const char*sp_PolyPolyHash_inspect(sp_PolyPolyHash*h){return h?sp_inspect_container(sp_box_obj(h,SP_BUILTIN_POLY_POLY_HASH)):"{}";}
/* Issue #738: Hash#invert -- swap keys and values. Returns a
   poly_poly_hash so any (key, value) pair shape is uniformly
   representable. str_str_hash_invert lives above (line ~1132)
   and stays as a same-type round-trip. */
static sp_PolyPolyHash*sp_StrIntHash_invert_poly(sp_StrIntHash*h){sp_PolyPolyHash*r=sp_PolyPolyHash_new();if(!h)return r;for(mrb_int i=0;i<h->len;i++)sp_PolyPolyHash_set(r,sp_box_int(sp_StrIntHash_get(h,h->order[i])),sp_box_str(h->order[i]));return r;}
static sp_PolyPolyHash*sp_IntStrHash_invert(sp_IntStrHash*h){sp_PolyPolyHash*r=sp_PolyPolyHash_new();if(!h)return r;for(mrb_int i=0;i<h->len;i++)sp_PolyPolyHash_set(r,sp_box_str(sp_IntStrHash_get(h,h->order[i])),sp_box_int(h->order[i]));return r;}
static mrb_bool sp_PolyPolyHash_eq(sp_PolyPolyHash*a,sp_PolyPolyHash*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++){sp_RbVal k=a->keys[a->order[i]];if(!sp_PolyPolyHash_has_key(b,k))return FALSE;if(!sp_poly_eq(sp_PolyPolyHash_get(a,k),sp_PolyPolyHash_get(b,k)))return FALSE;}return TRUE;}

/* JSON serialization now lives in lib/sp_json.c. It owns no container types and
   reaches them only through these generic readers, registered into the sp_json_*
   hooks below. sp_json_kind classifies a boxed value (1=array, 2=hash, 0=other);
   sp_poly_hash_pair yields a hash's boxed (key,value) at insertion index i.
   SYM_INT/SYM_STR hashes are not listed -> kind 0 -> null, as before. */
static int sp_json_kind(sp_RbVal v) {
  if (v.tag != SP_TAG_OBJ) return 0;
  switch (v.cls_id) {
    case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_FLT_ARRAY:
    case SP_BUILTIN_STR_ARRAY: case SP_BUILTIN_SYM_ARRAY:
    case SP_BUILTIN_POLY_ARRAY: return 1;
    case SP_BUILTIN_STR_INT_HASH: case SP_BUILTIN_STR_STR_HASH:
    case SP_BUILTIN_INT_STR_HASH: case SP_BUILTIN_STR_POLY_HASH:
    case SP_BUILTIN_SYM_POLY_HASH: case SP_BUILTIN_POLY_POLY_HASH:
      return 2;
    default: return 0;
  }
}
static void sp_poly_hash_pair(sp_RbVal v, mrb_int i, sp_RbVal *k, sp_RbVal *out) {
  *k = sp_box_nil(); *out = sp_box_nil();
  if (v.tag != SP_TAG_OBJ) return;
  switch (v.cls_id) {
    case SP_BUILTIN_STR_INT_HASH: { sp_StrIntHash *h=(sp_StrIntHash*)v.v.p; const char *key=h->order[i]; *k=sp_box_str(key); *out=sp_box_int(sp_StrIntHash_get(h,key)); break; }
    case SP_BUILTIN_STR_STR_HASH: { sp_StrStrHash *h=(sp_StrStrHash*)v.v.p; const char *key=h->order[i]; *k=sp_box_str(key); *out=sp_box_str(sp_StrStrHash_get(h,key)); break; }
    case SP_BUILTIN_INT_STR_HASH: { sp_IntStrHash *h=(sp_IntStrHash*)v.v.p; mrb_int key=h->order[i]; *k=sp_box_int(key); *out=sp_box_str(sp_IntStrHash_get(h,key)); break; }
    case SP_BUILTIN_STR_POLY_HASH: { sp_StrPolyHash *h=(sp_StrPolyHash*)v.v.p; const char *key=h->order[i]; *k=sp_box_str(key); *out=sp_StrPolyHash_get(h,key); break; }
    case SP_BUILTIN_SYM_POLY_HASH: { sp_SymPolyHash *h=(sp_SymPolyHash*)v.v.p; sp_sym key=h->order[i]; *k=sp_box_sym(key); *out=sp_SymPolyHash_get(h,key); break; }
    case SP_BUILTIN_POLY_POLY_HASH: { sp_PolyPolyHash *h=(sp_PolyPolyHash*)v.v.p; mrb_int oi=h->order[i]; *k=h->keys[oi]; *out=h->vals[oi]; break; }
    default: break;
  }
}
/* JSON.parse object builders: a string-keyed poly hash (the parser owns the
   key strings; StrPolyHash stores the pointer without copying). */
static sp_RbVal sp_json_new_strhash(void) {
  return sp_box_obj(sp_StrPolyHash_new(), SP_BUILTIN_STR_POLY_HASH);
}
static void sp_json_strhash_set(sp_RbVal h, const char *k, sp_RbVal v) {
  sp_StrPolyHash_set((sp_StrPolyHash *)h.v.p, k, v);
}
__attribute__((constructor)) static void sp_json_install_hooks(void) {
  sp_json_kind_fn = sp_json_kind;
  sp_json_len_fn = sp_poly_length;
  sp_json_aref_fn = sp_poly_arr_get;
  sp_json_hpair_fn = sp_poly_hash_pair;
  sp_poly_inspect_fn = sp_poly_inspect;
  /* JSON.parse builds objects as string-keyed hashes (CRuby returns String
     keys, and this matches a `{ "k" => v }` literal for equality). */
  sp_json_mk_hash_fn = sp_json_new_strhash;
  sp_json_hash_set_fn = sp_json_strhash_set;
}

#include <setjmp.h>
#define SP_EXC_STACK_MAX 64
/* Per-worker (SP_TLS) in the threaded build: this is the active exception/ensure
   handler stack of the thread currently executing. It is swapped per fiber by
   sp_exc_ctx_save/load, which assumes a single active stack -- true at N=1, but
   with N>1 every OS worker runs a fiber concurrently, so each needs its own.
   Empty (plain static, byte-identical) in the single-threaded build. */
static SP_TLS jmp_buf sp_exc_stack[SP_EXC_STACK_MAX];
static SP_TLS const char *sp_exc_msg[SP_EXC_STACK_MAX];
/* GC-root watermark at each handler's entry: a raise longjmps past the
   __attribute__((cleanup)) pops of SP_GC_ROOT locals in the unwound frames,
   so the landing restores sp_gc_nroots from the popped slot. A side array
   (not a per-region C local) so protected regions add no stack locals --
   an extra local per region measurably shifts hot-function frames. */
static SP_TLS int sp_exc_rootmark[SP_EXC_STACK_MAX];
static SP_TLS volatile int sp_exc_top = 0;
static SP_TLS const char *sp_exc_cls[SP_EXC_STACK_MAX];
static SP_TLS volatile const char *sp_last_exc_cls = sp_str_empty;
/* The raised exception OBJECT, carried alongside (cls,msg) so a user
   exception subclass keeps its ivars across raise -> rescue (#1415).
   NULL for a bare string/builtin raise, which reconstructs on catch.
   sp_pending_exc_obj is set by sp_raise_exc just before the longjmp and
   consumed into the per-frame slot by sp_raise_cls. */
static SP_TLS void *sp_exc_obj[SP_EXC_STACK_MAX];
static SP_TLS void *sp_pending_exc_obj = NULL;
/* The exception currently being handled (set by a rescue body), and the cause
   captured for the next raised exception -- Exception#cause threads the former
   into a newly raised exception's `cause` field. */
static SP_TLS void *sp_pending_cause = NULL;
/* The exception handled at each active rescue-body depth (CRuby's per-rescue
   errinfo). The "currently handled" exception -- what Exception#cause threads --
   is the innermost: sp_rescue_sp>0 ? sp_exc_handling[sp_rescue_sp-1] : NULL. A
   depth stack, NOT indexed by sp_exc_top: a rescue body runs with its begin frame
   already popped, so nested rescue bodies share an sp_exc_top. Pushed on rescue
   entry; popped on every exit -- fall-through, return/break/next/retry (codegen
   pops sp_rescue_sp), and raise-out (the landing restores sp_rescue_mark). */
static SP_TLS void *sp_exc_handling[SP_EXC_STACK_MAX];
static SP_TLS int sp_rescue_sp = 0;
/* sp_rescue_sp captured at each begin frame's arm, restored on that frame's
   exception landing so a rescue body that exits by raising doesn't leak its push
   (a side array beside the handler stack, mirroring sp_exc_rootmark). */
static SP_TLS int sp_rescue_mark[SP_EXC_STACK_MAX];
#define sp_cur_handled() (sp_rescue_sp > 0 ? sp_exc_handling[sp_rescue_sp-1] : NULL)
/* Push a handled exception. sp_rescue_sp grows with recursion *through* rescue
   bodies (the handler stays pushed across the recursive call it makes, unlike a
   begin frame which is popped first), so a fixed SP_EXC_STACK_MAX can be reached;
   fail loudly rather than write past sp_exc_handling. */
static void sp_rescue_push(void *e) {
  if (sp_rescue_sp >= SP_EXC_STACK_MAX) {
    fprintf(stderr, "rescue nesting too deep (> %d)\n", SP_EXC_STACK_MAX);
    exit(1);
  }
  sp_exc_handling[sp_rescue_sp++] = e;
}
/* ---- Native backtrace formatting (spinel --debug) ---------------------- */
/* True for sp_<name> symbols that are runtime helpers, not user Ruby methods.
   A denylist of the lowercase runtime prefixes; user methods are sp_<rubyname>
   (top-level) or sp_<Class>_<method>, which don't match. Heuristic — a leaf
   runtime frame may occasionally slip through; refine with an emitted
   user-method allowlist later. */
#if SP_BT_AVAILABLE
static int sp_bt_is_runtime(const char *n) {
  static const char *pfx[] = {
    "int_", "str_", "float_", "sym_", "gc_", "bigint", "sprintf", "raise",
    "exc_", "range", "utf8", "oom", "bt_", "backtrace", "caller", "StrArray",
    "IntArray", "FloatArray", "PtrArray", "PolyArray", "Str", "Int", "Float",
    "Hash", "Range", "Complex", "Rational", "Sym", "alloc", "free", "to_s",
    "dup", "new", "pack", "unpack", "regex", "re_",
    /* arithmetic/runtime helpers that can raise and sit between the raise
       and the user frame (ZeroDivisionError via sp_idiv/sp_imod, etc.) */
    "idiv", "imod", "gcd", "fdiv", "ipow", "iclamp", "div_", "mod_", 0
  };
  for (int i = 0; pfx[i]; i++) {
    size_t l = strlen(pfx[i]);
    if (strncmp(n, pfx[i], l) == 0) return 1;
  }
  return 0;
}

/* Extract the symbol token from a backtrace_symbols line. Two formats:
     - glibc/Linux: "<module>(<symbol>+0x<off>) [0x<addr>]". The symbol is
       empty for unresolved frames (static or stripped fns) -> skip.
     - macOS:       "<idx> <image> <addr> <symbol> + <off>".
   Returns NULL if it isn't a keepable user frame. Detect Linux by the '('
   that delimits the symbol (the macOS format has none). */
static const char *sp_bt_symbol(const char *line) {
  char sym[256];
  const char *lp = strchr(line, '(');
  if (lp) {                                       /* glibc/Linux paren form */
    const char *p = lp + 1;
    const char *end = p;
    while (*end && *end != '+' && *end != ')') end++;
    size_t len = (size_t)(end - p);
    if (len == 0 || len > 250) return 0;          /* unresolved (static/stripped) */
    memcpy(sym, p, len); sym[len] = 0;
  }
else {                                        /* macOS: "<idx> <image> <addr> <symbol> + <off>" */
    /* The symbol is the token just before the " + <off>" delimiter. Parse
       backward from the last " + " rather than forward from "0x" — an image
       path containing "0x" (e.g. /path/0x_proj/bin) would otherwise misparse. */
    const char *plus = 0, *q = line;
    while ((q = strstr(q, " + ")) != 0) { plus = q; q += 3; }
    if (!plus) return 0;
    const char *end = plus;
    const char *p = end;
    while (p > line && p[-1] != ' ') p--;          /* back up to the symbol's start */
    size_t len = (size_t)(end - p);
    if (len == 0 || len > 250) return 0;
    memcpy(sym, p, len); sym[len] = 0;
  }
  if (strcmp(sym, "main") == 0) return strdup("<main>");
  if (strncmp(sym, "sp_", 3) != 0) return 0;     /* skip non-Spinel frames */
  const char *name = sym + 3;
  if (sp_bt_is_runtime(name)) return 0;
  /* De-mangle sp_<Class>_<method> back to Ruby. A Spinel symbol is a path of
     CamelCase class segments (each from a `::`, joined by `_`) followed by the
     method; the method is the first segment that starts lowercase. A literal
     `cls` segment marks a singleton method:
       sp_Helper_cls_boom          -> Helper.boom
       sp_Tep_Url_parse_query      -> Tep::Url#parse_query
       sp_Tep_AuthOAuth2_cls_find  -> Tep::AuthOAuth2.find
       sp_toplevel                 -> toplevel   (top-level method, no class)
     (Method names stay sanitized — e.g. enabled? is enabled_p; reversing that
     needs the emitted name table, a separate refinement.) */
  const char *mstart = 0;   /* first lowercase-starting segment = the method */
  for (const char *p = name; *p; p++) {
    int seg_start = (p == name) || (p[-1] == '_');
    if (seg_start && *p >= 'a' && *p <= 'z') { mstart = p; break; }
  }
  if (!mstart) return strdup(name);            /* all-uppercase: leave as-is */
  if (mstart == name) {                        /* no class path: top-level */
    if (strncmp(name, "cls_", 4) == 0) return strdup(name + 4);  /* top-level singleton */
    return strdup(name);
  }
  char out[256]; size_t o = 0;
  const char *meth; char sep;
  if (strncmp(mstart, "cls_", 4) == 0) { meth = mstart + 4; sep = '.'; }  /* singleton */
  else                                 { meth = mstart;     sep = '#'; }  /* instance */
  for (const char *p = name; p < mstart - 1 && o + 2 < sizeof(out); p++) {
    if (*p == '_') { out[o++] = ':'; out[o++] = ':'; }   /* class-path `_` was a `::` */
    else out[o++] = *p;
  }
  if (o + 1 < sizeof(out)) out[o++] = sep;
  size_t ml = strlen(meth);
  if (o + ml < sizeof(out)) { memcpy(out + o, meth, ml); o += ml; }
  out[o] = 0;
  return strdup(out);
}

static sp_StrArray *sp_bt_format(void **buf, int n) {
  sp_StrArray *a = sp_StrArray_new();
  if (!sp_bt_enabled || n <= 0) return a;
  char **syms = backtrace_symbols(buf, n);
  if (!syms) return a;
  const char *src = (sp_bt_srcfile && sp_bt_srcfile[0]) ? sp_bt_srcfile : "(spinel)";
  for (int i = 0; i < n; i++) {
    char *name = (char *)sp_bt_symbol(syms[i]);  /* always strdup'd; free after use */
    if (!name) continue;
    sp_StrArray_push(a, sp_sprintf("%s:in `%s'", src, name));
    free(name);
  }
  free(syms);
  return a;
}
#endif

/* Backtrace captured at the most recent raise (for Exception#backtrace). */
static sp_StrArray *sp_backtrace_captured(void) {
#if SP_BT_AVAILABLE
  return sp_bt_format(sp_bt_buf, sp_bt_n);
#else
  return sp_StrArray_new();
#endif
}

/* The current stack, captured now (for Kernel#caller). */
static sp_StrArray *sp_caller_now(void) {
#if SP_BT_AVAILABLE
  if (!sp_bt_enabled) return sp_StrArray_new();
  void *buf[256];
  int n = backtrace(buf, 256);
  return sp_bt_format(buf, n);
#else
  return sp_StrArray_new();
#endif
}

/* Kernel#caller(start=1, length=nil): the current call stack, dropping the
   first `start` frames (the running method itself at index 0, like CRuby's
   `caller` == `caller(1)`), then up to `length` frames. Method-granularity
   (no :lineno:), same substrate as Exception#backtrace; release builds (no
   frame symbols) return []. */
static sp_StrArray *sp_caller(mrb_int start, mrb_bool have_len, mrb_int len) {
  sp_StrArray *full = sp_caller_now();
  SP_GC_ROOT(full);
  sp_StrArray *r = sp_StrArray_new();
  SP_GC_ROOT(r);
  mrb_int from = start < 0 ? 0 : start;
  mrb_int to = have_len ? from + (len < 0 ? 0 : len) : full->len;
  for (mrb_int i = from; i < to && i < full->len; i++) sp_StrArray_push(r, full->data[i]);
  return r;
}

/* Non-local-unwind state (a proc `return` or `throw` in flight while it runs the
   ensures it passes over). Declared here, before sp_raise_cls, so a real raise
   can clear it -- an exception raised inside an ensure during an unwind
   supersedes that unwind. The machinery that uses it lives further down. */
enum { SP_UNWIND_NONE, SP_UNWIND_PROCRET, SP_UNWIND_THROW, SP_UNWIND_BREAK };
struct sp_proc_home;
static SP_TLS int sp_unwind_kind = SP_UNWIND_NONE, sp_unwind_target = -1, sp_unwind_exc_top = 0;  /* per-worker (see sp_exc_stack) */
static SP_TLS struct sp_proc_home *sp_unwind_home = NULL;  /* PROCRET target (THROW uses sp_unwind_target) */
/* forward: materialize the exception object for a raise that carried none
   (a bare `raise "msg"` / `raise Cls, msg` / builtin runtime raise), so `$!`
   and `rescue => e` bind one real object with a stable identity. The struct
   tag is used because the sp_Exception typedef is defined further down; the
   result is stored as a void* in sp_exc_obj[]. */
struct sp_Exception_s;
static struct sp_Exception_s *sp_exc_new_for_catch(const char *cls, const char *msg);
SP_NORETURN SP_COLD void sp_raise_cls(const char *cls, const char *msg) {
#if SP_BT_AVAILABLE
  if (sp_bt_enabled) sp_bt_n = backtrace(sp_bt_buf, 256);
#endif
  /* A real exception supersedes any non-local unwind in flight (e.g. raised from
     inside an `ensure` running during a proc-return / throw): clear the unwind so
     an outer handler treats this as an exception, not a continued unwind. */
  sp_unwind_kind = SP_UNWIND_NONE;
  if (sp_exc_top > 0) { sp_exc_msg[sp_exc_top-1] = msg; sp_exc_cls[sp_exc_top-1] = cls; sp_exc_obj[sp_exc_top-1] = sp_pending_exc_obj; sp_pending_exc_obj = NULL; sp_pending_cause = sp_cur_handled(); sp_last_exc_cls = cls; longjmp(sp_exc_stack[sp_exc_top-1], 1); } fprintf(stderr, "unhandled exception: %s\n", msg); exit(1); }
static void sp_raise(const char *msg) { sp_raise_cls("RuntimeError", msg); }

/* Issue #781: bridge between the regex compile-error path (which lives
   in the .a library and can't see the user program's static-inline
   sp_raise_cls) and the user's Ruby-level exception machinery. The
   library calls sp_re_set_error_handler at startup -- codegen emits
   the install call after the exception infrastructure is set up. */
/* Issue #846: during sp_re_init (before main()'s setjmp scope is
   active), a bad literal `Regexp.new("[invalid")` pattern would
   route through sp_raise_cls -> "unhandled exception" + exit
   because sp_exc_top is 0. Install a startup handler that longjmps
   back to sp_re_init's wrapping setjmp; the codegen-emitted loop
   then stashes the error per slot for a deferred raise from the
   first use site (where the user's begin/rescue is active). The
   re_compile contract requires the error callback to not return
   (otherwise the library's fall-through fprintf+exit fires) so
   we longjmp out. */
static jmp_buf sp_re_startup_jmp;
static void sp_re_startup_error_handler(const char *msg) {
  if (msg) {
    size_t n = strlen(msg);
    char *buf = sp_str_alloc_raw(n + 1);
    memcpy(buf, msg, n);
    buf[n] = 0;
    sp_re_startup_err = buf;
  }
  longjmp(sp_re_startup_jmp, 1);
}
extern void sp_re_set_error_handler(void (*fn)(const char *msg));
static void sp_mark_in_flight_exceptions(void) {
  for (int i = 0; i < sp_exc_top; i++) {
    sp_mark_string(sp_exc_msg[i]);
    if (sp_exc_obj[i]) sp_gc_mark(sp_exc_obj[i]);  /* carried subclass object + its ivars */
  }
  if (sp_pending_exc_obj) sp_gc_mark(sp_pending_exc_obj);
  if (sp_pending_cause) sp_gc_mark(sp_pending_cause);
  for (int i = 0; i < sp_rescue_sp; i++)
    if (sp_exc_handling[i]) sp_gc_mark(sp_exc_handling[i]);  /* handled exceptions */
}

/* sp_Exception: first-class exception object. cls_name is a pointer
   to the per-class const string literal emitted by codegen
   (sp_class_names[] entry; not GC-managed). msg is GC-managed
   (sp_str_alloc'd). */
typedef struct sp_Exception_s {
  const char *cls_name;
  const char *parent_cls_name; /* builtin ancestor for user subclasses, or NULL */
  const char *msg;
  struct sp_Exception_s *cause; /* the exception being handled when this was raised, or NULL */
} sp_Exception;
/* Registered by the generated program to provide user exception hierarchy. */
static const char *(*sp_user_exc_parent_fn)(const char *) = NULL;
static void sp_exc_gc_scan(void *p) {
  sp_Exception *e = (sp_Exception *)p;
  if (e->msg) sp_mark_string(e->msg);
  if (e->cause) sp_gc_mark(e->cause);
  /* cls_name/parent_cls_name point into rodata -- not GC-managed strings */
}
static sp_Exception *sp_exc_new(const char *cls_name, const char *msg) {
  sp_Exception *e = (sp_Exception *)sp_gc_alloc(sizeof(sp_Exception), NULL, sp_exc_gc_scan);
  e->cls_name = cls_name ? cls_name : "RuntimeError";
  e->parent_cls_name = NULL;
  e->msg = (msg && msg[0]) ? msg : (cls_name ? cls_name : "RuntimeError");
  e->cause = NULL;  /* set all fields explicitly; sp_exc_gc_scan reads cause */
  return e;
}
static sp_Exception *sp_exc_new_sub(const char *cls_name, const char *parent_cls, const char *msg) {
  sp_Exception *e = sp_exc_new(cls_name, msg);
  e->parent_cls_name = parent_cls;
  if (!msg || !msg[0]) e->msg = cls_name ? cls_name : "RuntimeError";
  return e;
}
/* Allocate a zeroed exception-subclass struct of `sz` bytes with the base
   {cls_name, parent_cls_name, msg} prefix set, for the degenerate catch path
   where a user subclass with ivars was raised without a carried object
   (#1415). Its ivar fields stay zero (nil/0). msg is the only heap field, so
   the base scan suffices. */
static void *sp_exc_new_sub_sized(size_t sz, const char *cls_name, const char *msg) {
  sp_Exception *e = (sp_Exception *)sp_gc_alloc(sz, NULL, sp_exc_gc_scan);
  memset(e, 0, sz);
  e->cls_name = cls_name ? cls_name : "RuntimeError";
  e->msg = (msg && msg[0]) ? msg : e->cls_name;
  if (sp_user_exc_parent_fn) e->parent_cls_name = sp_user_exc_parent_fn(e->cls_name);
  return e;
}
/* Accept `volatile` pointers: LV slots holding sp_Exception * are
   declared volatile when they live across setjmp, so callers may
   pass volatile-qualified pointers in. The pointee itself isn't
   volatile (cls_name/msg are stable post-construction), so we
   strip volatile internally for one access. */
static const char *sp_exc_class_name(volatile sp_Exception *ve) {
  sp_Exception *e = (sp_Exception *)ve;
  return e ? e->cls_name : "RuntimeError";
}
static const char *sp_exc_message(volatile sp_Exception *ve) {
  sp_Exception *e = (sp_Exception *)ve;
  return e ? e->msg : sp_str_empty;
}
static void sp_raise_exc(volatile sp_Exception *ve) {
  sp_Exception *e = (sp_Exception *)ve;
  if (!e) sp_raise("(nil exception)");
  /* Carry the object so a user subclass keeps its ivars across the
     longjmp; sp_raise_cls moves it into the current frame's slot. */
  sp_pending_exc_obj = (void *)e;
  sp_raise_cls(e->cls_name, e->msg);
}
/* Create an exception for a `rescue => e` binding: like sp_exc_new but
   also looks up the parent class via the user hierarchy callback. */
static sp_Exception *sp_exc_new_for_catch(const char *cls, const char *msg) {
  sp_Exception *e = sp_exc_new(cls, msg);
  if (sp_user_exc_parent_fn) {
    const char *par = sp_user_exc_parent_fn(cls);
    if (par) e->parent_cls_name = par;
  }
  return e;
}
/* Exception#cause: the exception active when this one was raised, or NULL. */
static sp_Exception *sp_exc_cause(volatile sp_Exception *ve) {
  sp_Exception *e = (sp_Exception *)ve;
  return e ? e->cause : NULL;
}
/* Exception#is_a?(ClassName): checks class name and known hierarchy. */
static mrb_int sp_exc_is_a(volatile sp_Exception *ve, const char *cn) {
  sp_Exception *e = (sp_Exception *)ve;
  if (!e || !cn) return 0;
  if (!strcmp(e->cls_name, cn)) return 1;
  /* Walk the well-known exception hierarchy */
  static const char *const HIER[][2] = {
    {"RuntimeError",          "StandardError"},
    {"ArgumentError",         "StandardError"},
    {"TypeError",             "StandardError"},
    {"NameError",             "StandardError"},
    {"NoMethodError",         "NameError"},
    {"IndexError",            "StandardError"},
    {"KeyError",              "IndexError"},
    {"RangeError",            "StandardError"},
    {"IOError",               "StandardError"},
    {"EOFError",              "IOError"},
    {"ZeroDivisionError",     "StandardError"},
    {"NotImplementedError",   "StandardError"},
    {"StopIteration",         "IndexError"},
    {"FloatDomainError",      "RangeError"},
    {"Math_DomainError",      "StandardError"},
    {"FrozenError",           "RuntimeError"},
    {"EncodingError",         "StandardError"},
    {"LoadError",             "StandardError"},
    {"RegexpError",           "StandardError"},
    {"StringScanner_Error",   "StandardError"},
    {"FiberError",            "StandardError"},
    {"UncaughtThrowError",    "ArgumentError"},
    {"SyntaxError",           "ScriptError"},
    {"ScriptError",           "Exception"},
    {"StandardError",         "Exception"},
    {NULL, NULL}
  };
  /* find the exception's class chain and check if cn appears in it */
  const char *cls = e->cls_name;
  int used_parent = 0;
  for (int depth = 0; depth < 20 && cls; depth++) {
    if (!strcmp(cls, cn)) return 1;
    const char *parent = NULL;
    for (int i = 0; HIER[i][0]; i++)
      if (!strcmp(cls, HIER[i][0])) { parent = HIER[i][1]; break; }
    if (!parent) {
      /* unknown (user) class: try user hierarchy first */
      if (sp_user_exc_parent_fn) { parent = sp_user_exc_parent_fn(cls); }
      if (!parent) {
        if (!used_parent && e->parent_cls_name) {
          cls = e->parent_cls_name;
          used_parent = 1;
          continue;
        }
        if (!strcmp(cn, "Exception")) return 1;
        if (!strcmp(cn, "Object") || !strcmp(cn, "BasicObject")) return 1;
        break;
      }
    }
    cls = parent;
  }
  if (!strcmp(cn, "Object") || !strcmp(cn, "BasicObject") || !strcmp(cn, "Kernel")) return 1;
  return 0;
}

/* Check if exception class name `raised` is the same as or a subclass of
   `target`, using both the built-in hierarchy and the user hierarchy callback. */
static int sp_exc_cls_matches(const char *raised, const char *target) {
  if (!raised || !target) return 0;
  static const char *const HIER2[][2] = {
    {"RuntimeError",         "StandardError"},
    {"ArgumentError",        "StandardError"},
    {"TypeError",            "StandardError"},
    {"NameError",            "StandardError"},
    {"NoMethodError",        "NameError"},
    {"IndexError",           "StandardError"},
    {"KeyError",             "IndexError"},
    {"RangeError",           "StandardError"},
    {"IOError",              "StandardError"},
    {"EOFError",             "IOError"},
    {"ZeroDivisionError",    "StandardError"},
    {"NotImplementedError",  "StandardError"},
    {"StopIteration",        "IndexError"},
    {"FloatDomainError",     "RangeError"},
    {"Math_DomainError",     "StandardError"},
    {"FrozenError",          "RuntimeError"},
    {"EncodingError",        "StandardError"},
    {"LoadError",            "StandardError"},
    {"RegexpError",          "StandardError"},
    {"StringScanner_Error",  "StandardError"},
    {"FiberError",           "StandardError"},
    {"UncaughtThrowError",   "ArgumentError"},
    {"SyntaxError",          "ScriptError"},
    {"ScriptError",          "Exception"},
    {"StandardError",        "Exception"},
    {NULL, NULL}
  };
  const char *cls = raised;
  for (int depth = 0; depth < 30 && cls; depth++) {
    if (!strcmp(cls, target)) return 1;
    const char *parent = NULL;
    /* user hierarchy first */
    if (sp_user_exc_parent_fn) parent = sp_user_exc_parent_fn(cls);
    if (!parent) {
      for (int i = 0; HIER2[i][0]; i++)
        if (!strcmp(cls, HIER2[i][0])) { parent = HIER2[i][1]; break; }
    }
    if (!parent) {
      if (!strcmp(target, "Exception") || !strcmp(target, "Object") || !strcmp(target, "BasicObject")) return 1;
      break;
    }
    cls = parent;
  }
  if (!strcmp(target, "Object") || !strcmp(target, "BasicObject") || !strcmp(target, "Kernel")) return 1;
  return 0;
}

/* Cross-TU bridge for sp_bigint.c (compiled as a separate translation
   unit; can't see static helpers in this header). Defined non-static
   so sp_bigint.c's mrb_raise macro can dispatch into spinel's
   longjmp-based rescue net rather than fprintf+exit. */
void sp_bigint_raise_zerodiv(const char *msg) { sp_raise_cls("ZeroDivisionError", msg); }
/* sp_exc_is_a: see earlier definition (takes volatile sp_Exception *) */

/* A non-local control-flow unwind -- a proc `return` or a `throw` -- runs the
   `ensure` blocks it passes over before delivering to its target, like an
   exception does. It first longjmps through the sp_exc_stack handler chain (each
   handler runs its ensure), bounded by the exception depth recorded at the
   target's entry, then delivers to the target. sp_unwind_resume drives each step;
   the codegen-emitted exception handlers call it when an unwind is in flight.
   The SP_UNWIND_* enum and sp_unwind_* state are declared earlier (before
   sp_raise_cls, which clears them when a real exception supersedes an unwind). */

#define SP_CATCH_STACK_MAX 64
static SP_TLS jmp_buf sp_catch_stack[SP_CATCH_STACK_MAX];   /* per-worker (see sp_exc_stack) */
static SP_TLS const char *sp_catch_tag[SP_CATCH_STACK_MAX];
static SP_TLS mrb_int sp_catch_val[SP_CATCH_STACK_MAX];
static SP_TLS int sp_catch_exc_top[SP_CATCH_STACK_MAX];  /* exception depth at each catch's entry */
static SP_TLS int sp_catch_rootmark[SP_CATCH_STACK_MAX]; /* GC-root watermark at entry (see sp_exc_rootmark) */
static SP_TLS volatile int sp_catch_top = 0;
static void sp_throw(const char *tag, mrb_int val) {
  int i = sp_catch_top - 1;
  while (i >= 0) {
    if (strcmp(sp_catch_tag[i], tag) == 0) {
      sp_catch_val[i] = val; sp_catch_top = i + 1;
      if (sp_exc_top > sp_catch_exc_top[i]) {       /* run intervening ensures first */
        sp_unwind_kind = SP_UNWIND_THROW; sp_unwind_target = i; sp_unwind_exc_top = sp_catch_exc_top[i];
        longjmp(sp_exc_stack[sp_exc_top - 1], 1);
      }
      longjmp(sp_catch_stack[i], 1);
    }
    i--;
  }
  sp_raise_cls("UncaughtThrowError", sp_sprintf("uncaught throw :%s", tag));
}

/* ---- valued `break` from a block (CRuby's TAG_BREAK) ----------------------
   A `break <v>` inside a block makes the call that received the block return
   <v>, through any number of intervening frames. Every wrapped block-taking
   call pushes a break scope inside a setjmp; the scope's SERIAL (a fresh,
   never-reused id, like the proc-return home ids) is what a break addresses,
   because nested live scopes make top-of-stack targeting ambiguous: in
   `def m; [1].map { yield }; end; m { break 5 }` the spliced outer block
   executes inside the inner map's scope, yet its break belongs to m's call.
   Non-lambda procs capture their creation scope's serial the same way; a miss
   (dead or foreign scope) is CRuby's LocalJumpError "break from proc-closure".
   A break with intervening exception frames routes through sp_exc_stack first
   (SP_UNWIND_BREAK) so `ensure` bodies run, exactly like sp_throw and
   sp_proc_return above. The value channel is poly so any break value carries
   faithfully. Per-worker (SP_TLS) and fiber-context-saved like the catch
   arrays; the push is unchecked like sp_exc_arm / the catch push.
   The backing storage is heap-allocated on the first push (like
   sp_gc_mark_stack), NOT inline TLS arrays: ~14KB of TLS (a jmp_buf is
   ~200 bytes) shifts every hot TLS variable's layout and cost optcarrot
   ~8% fps in a program that never breaks from a block. TLS holds only
   the pointers, so break-free programs pay nothing. */
#define SP_BRK_STACK_MAX 64
static SP_TLS jmp_buf *sp_brk_stack;              /* lazily allocated */
static SP_TLS sp_RbVal *sp_brk_val;
static SP_TLS mrb_int *sp_brk_serial;
static SP_TLS int *sp_brk_exc_top;                /* sp_exc_top at scope entry */
static SP_TLS volatile int sp_brk_top = 0;
/* shared counter (not SP_TLS) so serials are globally unique; see
   sp_proc_home_seq for the same shape */
static mrb_int sp_brk_seq = 1;
static mrb_int sp_brk_push(void) {
  /* Fixed-depth stack like sp_exc / sp_catch: guard the push so pathological
     nesting (e.g. deep recursion through .each/.map) fails loudly instead of
     writing past the array. CRuby's SystemStackError message. */
  if (sp_brk_top >= SP_BRK_STACK_MAX) {
    fputs("unhandled exception: stack level too deep\n", stderr);
    exit(1);
  }
  if (!sp_brk_stack) {
    sp_brk_stack = (jmp_buf *)malloc(sizeof(jmp_buf) * SP_BRK_STACK_MAX);
    sp_brk_val = (sp_RbVal *)malloc(sizeof(sp_RbVal) * SP_BRK_STACK_MAX);
    sp_brk_serial = (mrb_int *)malloc(sizeof(mrb_int) * SP_BRK_STACK_MAX);
    sp_brk_exc_top = (int *)malloc(sizeof(int) * SP_BRK_STACK_MAX);
    if (!sp_brk_stack || !sp_brk_val || !sp_brk_serial || !sp_brk_exc_top)
      sp_oom_die();
  }
#ifdef SP_THREADS
  mrb_int s = __atomic_fetch_add(&sp_brk_seq, 1, __ATOMIC_RELAXED);
#else
  mrb_int s = sp_brk_seq++;
#endif
  sp_brk_serial[sp_brk_top] = s;
  sp_brk_exc_top[sp_brk_top] = sp_exc_top;
  sp_brk_val[sp_brk_top] = sp_box_nil();
  sp_brk_top++;
  return s;
}
static void __attribute__((noreturn)) sp_brk_throw(mrb_int serial, sp_RbVal v) {
  for (int i = sp_brk_top - 1; i >= 0; i--) {
    if (sp_brk_serial[i] != serial) continue;
    sp_brk_val[i] = v;
    sp_brk_top = i + 1;                    /* drop scopes above the target */
    if (sp_exc_top > sp_brk_exc_top[i]) {  /* run intervening ensures first */
      sp_unwind_kind = SP_UNWIND_BREAK; sp_unwind_target = i;
      sp_unwind_exc_top = sp_brk_exc_top[i];
      longjmp(sp_exc_stack[sp_exc_top - 1], 1);
    }
    longjmp(sp_brk_stack[i], 1);
  }
  /* no live scope carries the serial: an escaped/foreign proc's break. An
     inlined block's serial is always live, so this is unreachable for those. */
  sp_raise_cls("LocalJumpError", "break from proc-closure");
}
/* GC: the in-flight break values -- ensures between the throw and the landing
   can allocate and collect. Called from sp_re_mark_globals. */
static void sp_mark_brk_vals(void) {
  for (int i = 0; i < sp_brk_top; i++) sp_mark_rbval(sp_brk_val[i]);
}

/* ---- non-lambda proc `return` (non-local return to the home method) -------
   A non-lambda proc's `return` returns from the method that created the proc.
   The home method declares a `sp_proc_home` node on its own C stack and links it
   onto a per-fiber chain (sp_proc_ret_head), capturing the node's fresh,
   never-reused id into every returning proc it creates. The proc's `return`
   walks the chain for that id and longjmps to the node's setjmp target, so the
   home method returns the boxed value.

   This mirrors CRuby's EC_PUSH_TAG tag chain: each node rides the home method's
   C-stack frame (so depth is bounded by the C stack, like ordinary recursion,
   not a fixed array), and the single chain head is swapped per fiber via
   sp_exc_ctx, so fibers are isolated by construction. An escaped proc whose home
   has returned -- or one called from another fiber, whose home node is on a
   different (inactive) chain -- finds no matching id and raises LocalJumpError,
   matching CRuby. Like sp_throw, a proc return runs the `ensure` blocks it passes
   over (see the unwind machinery above); when there are none it longjmps straight
   home. The `exc_top` field records the exception-handler depth at the method's
   entry so intervening ensures run and so an exception that unwinds the home pops
   the node (sp_proc_homes_unwind). */
typedef struct sp_proc_home {
  jmp_buf jb;                 /* the home method's setjmp target (on its C stack) */
  sp_RbVal val;               /* the in-flight return value (nil until delivered) */
  int exc_top;                /* sp_exc_top at the method's entry */
  int catch_top;              /* sp_catch_top at the method's entry */
  mrb_int id;                 /* fresh id captured by the home's returning procs */
  struct sp_proc_home *prev;  /* enclosing home, forming the per-fiber chain */
} sp_proc_home;
/* sp_proc_ret_head is the current fiber's chain of in-flight proc-return homes,
   swapped per fiber by sp_exc_ctx_save/load -- per-worker (SP_TLS) under N>1 like
   the exception stack. sp_proc_home_seq stays a single shared counter so home ids
   are globally unique across workers (a lambda may be called on a different
   worker than it was created on); the bump is atomic in the threaded build. */
static SP_TLS sp_proc_home *sp_proc_ret_head = NULL;
static mrb_int sp_proc_home_seq = 0;
static mrb_int sp_proc_home_next(void) {
#ifdef SP_THREADS
  return __atomic_fetch_add(&sp_proc_home_seq, 1, __ATOMIC_RELAXED);
#else
  return sp_proc_home_seq++;
#endif
}
static void sp_proc_return(mrb_int id, sp_RbVal v) {
  for (sp_proc_home *h = sp_proc_ret_head; h; h = h->prev) {
    if (h->id == id) {
      h->val = v;
      if (sp_exc_top > h->exc_top) {   /* run intervening ensures first */
        sp_unwind_kind = SP_UNWIND_PROCRET; sp_unwind_home = h; sp_unwind_exc_top = h->exc_top;
        longjmp(sp_exc_stack[sp_exc_top - 1], 1);
      }
      longjmp(h->jb, 1);
    }
  }
  sp_raise_cls("LocalJumpError", "unexpected return");
}
/* Pop home nodes whose method an exception has unwound past (recorded exc_top now
   above the live exception depth), so a later proc-return to a dead home misses
   and raises LocalJumpError rather than longjmping into a freed C frame. Called
   wherever a caught exception drops sp_exc_top. A no-op during a proc-return /
   throw unwind: the target's exc_top is at or below the new depth. */
static void sp_proc_homes_unwind(void) {
  while (sp_proc_ret_head && sp_proc_ret_head->exc_top > sp_exc_top)
    sp_proc_ret_head = sp_proc_ret_head->prev;
  /* pop break scopes the exception unwound past too, so a later proc-break
     addressing a dead scope misses (LocalJumpError) instead of longjmping
     into a freed C frame */
  while (sp_brk_top > 0 && sp_brk_exc_top[sp_brk_top - 1] > sp_exc_top)
    sp_brk_top--;
}
/* GC: mark the current fiber's chain of in-flight return values (suspended fibers
   are handled by sp_exc_ctx_mark). Each node's val is sp_box_nil() until a return
   is delivered, so this is cheap and safe when no return is in flight. */
static void sp_mark_proc_homes(void) {
  for (sp_proc_home *h = sp_proc_ret_head; h; h = h->prev) sp_mark_rbval(h->val);
}

#ifdef SP_THREADS
/* Stop-the-world support: push this worker's per-worker in-flight GC roots --
   pending exception objects and proc-return home values, both thread-local --
   onto its shadow stack so the collector marks them while the worker is parked
   (sp_safepoint_publish_hook, sp_sched.c). The caller snapshots and then restores
   the root-stack depth, exactly as sp_re_push_match_roots does for the regex
   globals. Only in the threaded build (the single-threaded one never parks). */
static void sp_publish_worker_roots(void) {
  for (int i = 0; i < sp_exc_top; i++) if (sp_exc_obj[i]) _sp_gc_root_push((void **)&sp_exc_obj[i]);
  if (sp_pending_exc_obj) _sp_gc_root_push((void **)&sp_pending_exc_obj);
  if (sp_pending_cause) _sp_gc_root_push((void **)&sp_pending_cause);
  for (int i = 0; i < sp_rescue_sp; i++)
    if (sp_exc_handling[i]) _sp_gc_root_push((void **)&sp_exc_handling[i]);
  for (sp_proc_home *h = sp_proc_ret_head; h; h = h->prev)
    _sp_gc_root_push((void **)((uintptr_t)&h->val | (uintptr_t)1));   /* sp_RbVal root */
  for (int i = 0; i < sp_brk_top; i++)
    _sp_gc_root_push((void **)((uintptr_t)&sp_brk_val[i] | (uintptr_t)1));
}
__attribute__((constructor)) static void sp_install_safepoint_publish(void) {
  sp_safepoint_publish_hook = sp_publish_worker_roots;
}
#endif
/* Continue a non-local unwind after a handler has run its ensure: if more ensure
   handlers lie between here and the target, longjmp to the next; otherwise
   deliver to the target (the proc-return home node, or the matched catch slot). */
static void sp_unwind_resume(void) {
  if (sp_exc_top > sp_unwind_exc_top) longjmp(sp_exc_stack[sp_exc_top - 1], 1);
  int kind = sp_unwind_kind;
  sp_unwind_kind = SP_UNWIND_NONE;
  if (kind == SP_UNWIND_PROCRET) longjmp(sp_unwind_home->jb, 1);
  if (kind == SP_UNWIND_BREAK) longjmp(sp_brk_stack[sp_unwind_target], 1);
  longjmp(sp_catch_stack[sp_unwind_target], 1);
}

/* ---- Per-fiber exception/catch handler context (#1474) -------------------
   The handler stacks above are process-global, but begin/rescue handlers and
   catch tags are stack-frame-bound: a fiber that yields while holding one must
   not let another fiber's raise/throw longjmp into its suspended frame. So
   sp_fiber.c saves the live prefix [0..top] of every handler array into the
   outgoing fiber's context and loads the incoming fiber's at each switch; the
   arrays then never alias across fibers. Only the active prefix is copied
   (top == 0 for a fiber that never rescues, e.g. optcarrot's PPU, so it is
   free there). These are non-static: reached by name from libspinel_rt.a. */
typedef struct {
  jmp_buf *es; const char **em; const char **ec; void **eo; int en, ecap;
  jmp_buf *cs; const char **ct; mrb_int *cv; int *cet;    int cn, ccap;
  jmp_buf *bs; sp_RbVal *bv; mrb_int *bser; int *bet;     int bn, bcap;  /* break scopes */
  sp_proc_home *prhead;  /* this fiber's proc-return chain head (nodes on its C stack) */
  int uk, ut, ue; sp_proc_home *uh;  /* transient unwind state (in flight only while running ensures) */
  void **shand; int rn, rcap;        /* sp_exc_handling prefix [0..sp_rescue_sp) */
  void *pcause;                      /* sp_pending_cause */
} sp_exc_ctx_t;

void *sp_exc_ctx_new(void) { return calloc(1, sizeof(sp_exc_ctx_t)); }
void sp_exc_ctx_free(void *p) {
  sp_exc_ctx_t *x = (sp_exc_ctx_t *)p;
  if (!x) return;
  free(x->es); free(x->em); free(x->ec); free(x->eo);
  free(x->cs); free(x->ct); free(x->cv); free(x->cet);
  free(x->bs); free(x->bv); free(x->bser); free(x->bet); free(x->shand); free(x);
}
void sp_exc_ctx_save(void *p) {            /* current globals -> ctx */
  sp_exc_ctx_t *x = (sp_exc_ctx_t *)p;
  int n = sp_exc_top;
  if (n > x->ecap) { x->ecap = n;
    x->es = (jmp_buf *)realloc(x->es, sizeof(jmp_buf) * n);
    x->em = (const char **)realloc(x->em, sizeof(char *) * n);
    x->ec = (const char **)realloc(x->ec, sizeof(char *) * n);
    x->eo = (void **)realloc(x->eo, sizeof(void *) * n); }
  for (int i = 0; i < n; i++) { memcpy(x->es[i], sp_exc_stack[i], sizeof(jmp_buf));
    x->em[i] = sp_exc_msg[i]; x->ec[i] = sp_exc_cls[i]; x->eo[i] = sp_exc_obj[i]; }
  x->en = n;
  int m = sp_catch_top;
  if (m > x->ccap) { x->ccap = m;
    x->cs = (jmp_buf *)realloc(x->cs, sizeof(jmp_buf) * m);
    x->ct = (const char **)realloc(x->ct, sizeof(char *) * m);
    x->cv = (mrb_int *)realloc(x->cv, sizeof(mrb_int) * m);
    x->cet = (int *)realloc(x->cet, sizeof(int) * m); }
  for (int i = 0; i < m; i++) { memcpy(x->cs[i], sp_catch_stack[i], sizeof(jmp_buf));
    x->ct[i] = sp_catch_tag[i]; x->cv[i] = sp_catch_val[i]; x->cet[i] = sp_catch_exc_top[i]; }
  x->cn = m;
  int bn = sp_brk_top;
  if (bn > x->bcap) { x->bcap = bn;
    x->bs = (jmp_buf *)realloc(x->bs, sizeof(jmp_buf) * bn);
    x->bv = (sp_RbVal *)realloc(x->bv, sizeof(sp_RbVal) * bn);
    x->bser = (mrb_int *)realloc(x->bser, sizeof(mrb_int) * bn);
    x->bet = (int *)realloc(x->bet, sizeof(int) * bn);
    if (!x->bs || !x->bv || !x->bser || !x->bet) sp_oom_die(); }
  for (int i = 0; i < bn; i++) { memcpy(x->bs[i], sp_brk_stack[i], sizeof(jmp_buf));
    x->bv[i] = sp_brk_val[i]; x->bser[i] = sp_brk_serial[i]; x->bet[i] = sp_brk_exc_top[i]; }
  x->bn = bn;
  x->prhead = sp_proc_ret_head;
  x->uk = sp_unwind_kind; x->ut = sp_unwind_target; x->ue = sp_unwind_exc_top; x->uh = sp_unwind_home;
  int rn = sp_rescue_sp;
  if (rn > x->rcap) { x->rcap = rn;
    x->shand = (void **)realloc(x->shand, sizeof(void *) * rn);
    if (!x->shand) sp_oom_die(); }
  for (int i = 0; i < rn; i++) x->shand[i] = sp_exc_handling[i];
  x->rn = rn; x->pcause = sp_pending_cause;
}
void sp_exc_ctx_load(void *p) {            /* ctx -> current globals */
  sp_exc_ctx_t *x = (sp_exc_ctx_t *)p;
  for (int i = 0; i < x->en; i++) { memcpy(sp_exc_stack[i], x->es[i], sizeof(jmp_buf));
    sp_exc_msg[i] = x->em[i]; sp_exc_cls[i] = x->ec[i]; sp_exc_obj[i] = x->eo[i]; }
  sp_exc_top = x->en;
  for (int i = 0; i < x->cn; i++) { memcpy(sp_catch_stack[i], x->cs[i], sizeof(jmp_buf));
    sp_catch_tag[i] = x->ct[i]; sp_catch_val[i] = x->cv[i]; sp_catch_exc_top[i] = x->cet[i]; }
  sp_catch_top = x->cn;
  for (int i = 0; i < x->bn; i++) { memcpy(sp_brk_stack[i], x->bs[i], sizeof(jmp_buf));
    sp_brk_val[i] = x->bv[i]; sp_brk_serial[i] = x->bser[i]; sp_brk_exc_top[i] = x->bet[i]; }
  sp_brk_top = x->bn;
  sp_proc_ret_head = x->prhead;
  sp_unwind_kind = x->uk; sp_unwind_target = x->ut; sp_unwind_exc_top = x->ue; sp_unwind_home = x->uh;
  for (int i = 0; i < x->rn; i++) sp_exc_handling[i] = x->shand[i];
  sp_rescue_sp = x->rn; sp_pending_cause = x->pcause;
}
void sp_exc_ctx_mark(void *p) {            /* GC: mark a suspended fiber's carried exc objects */
  sp_exc_ctx_t *x = (sp_exc_ctx_t *)p;
  if (!x) return;
  for (int i = 0; i < x->en; i++) if (x->eo[i]) sp_gc_mark(x->eo[i]);
  /* a suspended fiber's proc-return chain (nodes on its preserved C stack) may
     carry an in-flight return value; mark each so it survives a GC during yield. */
  for (sp_proc_home *h = x->prhead; h; h = h->prev) sp_mark_rbval(h->val);
  for (int i = 0; i < x->bn; i++) sp_mark_rbval(x->bv[i]);   /* carried break scopes */
  for (int i = 0; i < x->rn; i++) if (x->shand[i]) sp_gc_mark(x->shand[i]);  /* handled excs */
}
/* Trampoline base handler (#1474): the fiber trampoline arms a copy of its own
   setjmp buffer as the fiber's lowest handler, so an otherwise-unhandled raise
   in the fiber body unwinds back to the trampoline (on the fiber's own stack)
   instead of exiting or long-jumping across to the resumer. */
void sp_exc_arm(jmp_buf b)     { memcpy(sp_exc_stack[sp_exc_top], b, sizeof(jmp_buf)); sp_exc_top++; }
void sp_exc_disarm(void)       { if (sp_exc_top > 0) sp_exc_top--; }
const char *sp_exc_cur_cls(void) { return sp_exc_top > 0 ? sp_exc_cls[sp_exc_top-1] : sp_str_empty; }
const char *sp_exc_cur_msg(void) { return sp_exc_top > 0 ? sp_exc_msg[sp_exc_top-1] : sp_str_empty; }
void *sp_exc_cur_obj(void)       { return sp_exc_top > 0 ? sp_exc_obj[sp_exc_top-1] : NULL; }
/* Re-raise a fiber's unhandled exception in the resumer's context (the fiber
   trampoline caught it on the fiber's stack, then returned cooperatively). */
void sp_fiber_reraise(const char *cls, const char *msg, void *obj) {
  if (obj) sp_pending_exc_obj = obj;
  sp_raise_cls(cls, msg);
}

/* Kernel#sleep with sub-second precision. Argument is seconds as a
   double so `sleep(0.5)` actually waits 500ms; the legacy `sleep((unsigned)0.5)`
   cast truncated to 0 and returned immediately. POSIX uses
   nanosleep(); Windows uses Sleep() (milliseconds). Negative or NaN
   inputs no-op. */
static void sp_sleep(mrb_float s) {
  if (!(s > 0.0)) return;
#ifdef SP_THREADS
  /* Scheduler-aware: park the green thread and free its OS worker for others; a
     monitor thread wakes it after the duration (see lib/sp_sched.c). */
  sp_sched_sleep((double)s);
#else
  struct timespec req;
  req.tv_sec = (time_t)s;
  req.tv_nsec = (long)((s - (double)req.tv_sec) * 1e9);
  if (req.tv_nsec < 0) req.tv_nsec = 0;
  if (req.tv_nsec >= 1000000000L) req.tv_nsec = 999999999L;
  while (nanosleep(&req, &req) == -1 && errno == EINTR) {}
#endif
}

/* File metadata predicates (sp_file_directory/file/exist/delete) moved to
   lib/sp_io.c (libspinel_rt.a); prototypes come from sp_io.h. */

/* Text mode ("r") matches CRuby's File.read: on Windows, CRLF is
   normalized to LF on read, which cancels out fopen("w")'s
   LF→CRLF on write. Without this, content from File.read passed to
   puts goes through stdout's text-mode translation a second time
   and `\r\n` becomes `\r\r\n`. fread's actual byte count drives
   null-termination because text mode shrinks the byte count below
   ftell's raw-file size. */
static const char *sp_file_read(const char *path) {
  if (sp_file_directory(path)) {
    sp_raise_cls("Errno::EISDIR", sp_sprintf("Is a directory @ io_fread - %s", path));
  }
  FILE *f = fopen(path, "r");
  if (!f) {
    sp_raise_cls(errno == ENOENT ? "Errno::ENOENT" : errno == EACCES ? "Errno::EACCES" : "RuntimeError",
                 sp_sprintf("%s @ rb_sysopen - %s", strerror(errno), path));
    return &("\xff" "")[1];
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = sp_str_alloc(sz);
  size_t n = 0;
  if (sz > 0) {
    n = fread(buf, 1, sz, f);
  }
  buf[n] = 0;
  sp_str_set_len(buf, n);  /* a short read must not leave the size as length */
  fclose(f);
  return buf;
}

static void sp_file_write(const char *path, const char *data) {
  if (sp_file_directory(path)) {
    sp_raise_cls("Errno::EISDIR", sp_sprintf("Is a directory @ rb_sysopen - %s", path));
  }
  FILE *f = fopen(path, "wb");
  if (!f) {
    sp_raise_cls(errno == ENOENT ? "Errno::ENOENT" : errno == EACCES ? "Errno::EACCES" : "RuntimeError",
                 sp_sprintf("%s @ rb_sysopen - %s", strerror(errno), path));
    return;
  }
  fwrite(data, 1, sp_str_byte_len(data), f);
  fclose(f);
}
static sp_Time sp_file_mtime(const char *path) {
  if (!path) {
    sp_raise_cls("TypeError", "no implicit conversion of nil into String");
    return (sp_Time){0, 0, 0};
  }
  struct stat st;
  if (stat(path, &st) == -1) {
    sp_raise_cls(errno == ENOENT ? "Errno::ENOENT" : "RuntimeError", sp_sprintf("%s @ File.mtime - %s", strerror(errno), path));
    return (sp_Time){0, 0, 0};
  }
#if defined(__APPLE__)
  return (sp_Time){(int64_t)st.st_mtimespec.tv_sec, (int32_t)st.st_mtimespec.tv_nsec, 0};
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
  return (sp_Time){(int64_t)st.st_mtimespec.tv_sec, (int32_t)st.st_mtimespec.tv_nsec, 0};
#else
  /* Linux / others with st_mtim */
  return (sp_Time){(int64_t)st.st_mtim.tv_sec, (int32_t)st.st_mtim.tv_nsec, 0};
#endif
}
/* File.size(path) -> byte size. Raises Errno::ENOENT on a missing path,
   matching MRI (and sp_file_read / sp_file_mtime, which stat/open the
   same way). */
static mrb_int sp_file_size(const char *path) {
  if (!path) {
    sp_raise_cls("TypeError", "no implicit conversion of nil into String");
    return 0;
  }
  struct stat st;
  if (stat(path, &st) == -1) {
    int err = errno;  /* capture once: strerror() may clobber errno */
    sp_raise_cls(err == ENOENT ? "Errno::ENOENT" : "RuntimeError", sp_sprintf("%s @ File.size - %s", strerror(err), path));
    return 0;
  }
  /* off_t (typically 64-bit) into mrb_int (intptr_t -> 32-bit on a 32-bit
     build): guard the narrowing, as spinel does for int arithmetic. */
  if ((off_t)(mrb_int)st.st_size != st.st_size) {
    sp_raise_cls("RangeError", "file size out of range for Integer");
    return 0;
  }
  return (mrb_int)st.st_size;
}
static const char *sp_backtick(const char *cmd) {
  FILE *p = popen(cmd, "r");
  if (!p) { sp_last_status = -1; return sp_str_empty; }
  char *buf = sp_str_alloc_raw(4096);
  size_t n = fread(buf, 1, 4095, p);
  buf[n] = 0;
  int st = pclose(p);
  /* Mirror sp_system_args' $? layout: POSIX pclose returns a wait-status,
     MSVCRT _pclose returns the plain exit code (shift to match). */
  sp_last_status = st;
  return buf;
}
static const char *sp_file_basename(const char *path) {
  const char *s = strrchr(path, '/');
  const char *base = s ? s + 1 : path;
  /* sp_gc_mark looks at byte[-1] to distinguish heap strings (`\xfe`)
     from literals (`\xff`). A `s+1` mid-path pointer has whatever the
     '/' was before it — and for an arbitrary string that's not a tag,
     so the GC tries to dereference it as a heap header and segfaults.
     Return a fresh sp_str_alloc'd copy so the prefix marker is right. */
  size_t n = strlen(base);
  char *buf = sp_str_alloc(n);
  memcpy(buf, base, n + 1);
  return buf;
}
/* Issue #892: File.dirname / File.extname / Dir.pwd. */
static const char *sp_file_dirname(const char *path) {
  const char *s = strrchr(path, '/');
  if (!s) { char *r = sp_str_alloc(1); r[0] = '.'; r[1] = 0; return r; }
  if (s == path) { char *r = sp_str_alloc(1); r[0] = '/'; r[1] = 0; return r; }
  size_t n = (size_t)(s - path);
  char *buf = sp_str_alloc(n);
  memcpy(buf, path, n); buf[n] = 0;
  return buf;
}
static const char *sp_file_extname(const char *path) {
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  const char *dot = strrchr(base, '.');
  /* CRuby: leading-dot files (".bashrc") return "". Trailing-dot
     paths ("foo.") keep the dot since Ruby 2.7. */
  if (!dot || dot == base) return sp_str_empty;
  size_t n = strlen(dot);
  char *buf = sp_str_alloc(n);
  memcpy(buf, dot, n + 1);
  return buf;
}
static const char *sp_dir_pwd(void) {
  char tmp[4096];
  if (!getcwd(tmp, sizeof(tmp))) { return sp_str_empty; }
  size_t n = strlen(tmp);
  char *buf = sp_str_alloc(n);
  memcpy(buf, tmp, n + 1);
  return buf;
}
/* Dir singleton methods. mkdir/rmdir/chdir use the platform call (the
   Windows _-prefixed variants take a single path argument); each returns
   0 on success, matching CRuby's `Dir.mkdir` etc. */
static mrb_int sp_dir_mkdir(const char *path) {
  return (mrb_int)mkdir(path, 0777);
}
static mrb_int sp_dir_rmdir(const char *path) {
  return (mrb_int)rmdir(path);
}
static mrb_int sp_dir_chdir(const char *path) {
  return (mrb_int)chdir(path);
}
static const char *sp_dir_home(void) {
  const char *h = getenv("HOME");
  if (!h) return sp_str_empty;
  return sp_str_dup_external(h);
}
/* Wildcard match for a single path component: `*` (any run, no `/`),
   `?` (one char). Recursive over `*`; adequate for the common
   single-directory glob patterns. */
static int sp_fnmatch1(const char *pat, const char *str) {
  while (*pat) {
    if (*pat == '*') {
      pat++;
      if (!*pat) return 1;
      while (*str) { if (sp_fnmatch1(pat, str)) return 1; str++; }
      return sp_fnmatch1(pat, str);
    }
else if (*pat == '?') {
      if (!*str) return 0;
      pat++; str++;
    }
else {
      if (*pat != *str) return 0;
      pat++; str++;
    }
  }
  return *str == 0;
}
/* Recursive helper for a double-star glob: visit `fsdir` and every descendant,
   and in each match `tail` (a single-component pattern) against the directory's
   entries. `outprefix` is prepended to a match to reproduce the path shape Ruby
   returns (the text before the double-star is preserved verbatim; a cwd-anchored
   pattern yields bare names). Symlinked directories are not traversed, matching
   CRuby's default and avoiding cycles. Hidden entries are skipped unless `tail`
   starts with a dot; hidden directories are never descended. */
static void sp_dir_glob_rec(const char *fsdir, const char *outprefix,
                            const char *tail, sp_StrArray *a) {
  DIR *d = opendir(fsdir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    const char *name = e->d_name;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
    char fspath[2048], outpath[2048];
    snprintf(fspath, sizeof fspath, "%s/%s", fsdir, name);
    snprintf(outpath, sizeof outpath, "%s%s", outprefix, name);
    if (!(name[0] == '.' && tail[0] != '.') && sp_fnmatch1(tail, name)) {
      char *copy = sp_str_alloc(strlen(outpath));
      strcpy(copy, outpath);
      sp_StrArray_push(a, copy);
    }
    if (name[0] != '.') {
      struct stat st;
      if (lstat(fspath, &st) == 0 && S_ISDIR(st.st_mode)) {
        char subprefix[2048];
        snprintf(subprefix, sizeof subprefix, "%s%s/", outprefix, name);
        sp_dir_glob_rec(fspath, subprefix, tail, a);
      }
    }
  }
  closedir(d);
}
/* Dir.glob(pattern): list directory entries matching the last component
   of `pattern` (an optional leading `dir/` selects the directory). A recursive
   double-star component walks that subtree (the tail after it matches per
   directory). Hidden entries match only when the pattern itself begins with
   `.`. Results are sorted, matching Ruby 3.0+ default glob ordering. */
static sp_StrArray *sp_dir_glob(const char *pattern) {
  sp_StrArray *a = sp_StrArray_new();
  if (!pattern) return a;
  /* Recursive double-star form: split at the double-star component. Everything
     before it is the output prefix (and, minus the trailing slash, the directory
     to walk); the component after it is the per-directory tail pattern. */
  const char *ss = strstr(pattern, "**");
  if (ss) {
    size_t plen = (size_t)(ss - pattern);
    if (plen >= 1024) return a;
    const char *after = ss + 2;
    if (*after == '/') after++;
    const char *tail = after;
    char outprefix[1024];
    memcpy(outprefix, pattern, plen);
    outprefix[plen] = 0;
    char fsdir[1024];
    if (plen == 0) {
      strcpy(fsdir, ".");
    }
    else {
      memcpy(fsdir, pattern, plen);
      fsdir[plen] = 0;
      if (fsdir[plen - 1] == '/') fsdir[plen - 1] = 0;
      if (fsdir[0] == 0) strcpy(fsdir, "/");
    }
    sp_dir_glob_rec(fsdir, outprefix, tail, a);
    sp_StrArray_sort_bang(a);
    return a;
  }
  const char *slash = strrchr(pattern, '/');
  char dirbuf[1024];
  const char *dirpath;
  const char *base_pat;
  if (slash) {
    size_t dl = (size_t)(slash - pattern);
    if (dl >= sizeof(dirbuf)) return a;
    memcpy(dirbuf, pattern, dl);
    dirbuf[dl] = 0;
    dirpath = (dl == 0) ? "/" : dirbuf;
    base_pat = slash + 1;
  }
else {
    dirpath = ".";
    base_pat = pattern;
  }
  DIR *d = opendir(dirpath);
  if (!d) return a;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    const char *name = e->d_name;
    if (name[0] == '.' && base_pat[0] != '.') continue;
    if (sp_fnmatch1(base_pat, name)) {
      char full[2048];
      if (slash) snprintf(full, sizeof(full), "%s/%s", dirbuf, name);
      else snprintf(full, sizeof(full), "%s", name);
      char *copy = sp_str_alloc(strlen(full));
      strcpy(copy, full);
      sp_StrArray_push(a, copy);
    }
  }
  closedir(d);
  sp_StrArray_sort_bang(a);
  return a;
}
/* Dir.entries / Dir.children: every entry of one directory, dotfiles
   included; children drops "." / "..". Sorted for determinism (CRuby
   leaves readdir order unspecified). A missing directory raises like
   CRuby, not the glob-style empty result. */
static sp_StrArray *sp_dir_entries_impl(const char *path, int children) {
  if (!path) sp_raise_cls("TypeError", "no implicit conversion of nil into String");
  DIR *d = opendir(path);
  if (!d) sp_raise_cls("Errno::ENOENT", sp_sprintf("No such file or directory @ dir_initialize - %s", path));
  sp_StrArray *a = sp_StrArray_new();
  SP_GC_ROOT(a);
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    const char *name = e->d_name;
    if (children && name[0] == '.' &&
        (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
    char *copy = sp_str_alloc(strlen(name));
    strcpy(copy, name);
    sp_StrArray_push(a, copy);
  }
  closedir(d);
  sp_StrArray_sort_bang(a);
  return a;
}
static sp_StrArray *sp_dir_entries(const char *path) { return sp_dir_entries_impl(path, 0); }
static sp_StrArray *sp_dir_children(const char *path) { return sp_dir_entries_impl(path, 1); }

/* File.expand_path(path[, base]) -- CRuby-compatible pure-string
   expansion (does NOT require the path to exist). A leading `~` / `~/`
   becomes $HOME; a relative path is resolved against `base` (itself
   expanded; NULL means the current working directory); and `.` / `..` /
   duplicate-slash segments are collapsed. `~user` is unsupported and is
   left as-is. */
static const char *sp_file_expand_path(const char *path, const char *base) {
  char raw[8192];
  char cwd[4096];
  const char *home = getenv("HOME");
  if (!home) home = "";
  if (!path) path = "";

  /* The `%.4000s` precision caps each component so the compiler can
     prove the combined output (<= 8001) fits in the 8192 buffer -- this
     silences -Wformat-truncation (an error under the test harness's
     -Werror). 4000 chars/component is well past PATH_MAX, so real paths
     never truncate. */
  if (path[0] == '~' && (path[1] == '\0' || path[1] == '/')) {
    snprintf(raw, sizeof(raw), "%.4000s%.4000s", home, path + 1);
  }
else if (path[0] == '/') {
    snprintf(raw, sizeof(raw), "%.4000s", path);
  }
else {
    char basebuf[8192];
    const char *b;
    if (base && base[0]) {
      if (base[0] == '~' && (base[1] == '\0' || base[1] == '/')) {
        snprintf(basebuf, sizeof(basebuf), "%.4000s%.4000s", home, base + 1);
        b = basebuf;
      }
else if (base[0] == '/') {
        b = base;
      }
else {
        if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
        snprintf(basebuf, sizeof(basebuf), "%.4000s/%.4000s", cwd, base);
        b = basebuf;
      }
    }
else {
      if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
      b = cwd;
    }
    snprintf(raw, sizeof(raw), "%.4000s/%.4000s", b, path);
  }

  /* Normalize: walk segments, collapsing `.`/`..`/`//`. seg_start[k]
     records the output length to roll back to when a `..` pops the
     k-th kept segment. */
  size_t rawlen = strlen(raw);
  char *out = sp_str_alloc(rawlen + 1);
  size_t seg_start[1024];
  int nseg = 0;
  size_t olen = 0;
  out[olen++] = '/';
  const char *p = raw;
  while (*p) {
    if (*p == '/') { p++; continue; }
    const char *q = p;
    while (*q && *q != '/') q++;
    size_t slen = (size_t)(q - p);
    if (slen == 1 && p[0] == '.') {
      /* current dir -- skip */
    }
else if (slen == 2 && p[0] == '.' && p[1] == '.') {
      if (nseg > 0) { nseg--; olen = seg_start[nseg]; }
    }
else {
      size_t mark = olen;
      if (olen > 1) out[olen++] = '/';
      memcpy(out + olen, p, slen);
      olen += slen;
      if (nseg < 1024) seg_start[nseg++] = mark;
    }
    p = q;
  }
  out[olen] = 0;
  /* The result was sized to the pre-normalized rawlen+1 (a safe capacity
     bound), but normalization can shrink it, so the actual content is olen
     bytes. Record olen as the length; otherwise the string carries trailing
     NUL/garbage bytes past the path (e.g. File.expand_path("x") returns a
     string whose byte length includes a stray "\0"). */
  sp_str_set_len(out, olen);
  return out;
}

/* Read a file's bytes into a fresh IntArray. Distinct from
   `sp_str_bytes(sp_file_read(path))` because plain sp_str_bytes uses
   null-termination and stops at the first 0x00 byte — wrong for
   binary data (e.g. .nes ROM files). */
static sp_IntArray *sp_file_binread_bytes(const char *path) {
  if (sp_file_directory(path)) {
    sp_raise_cls("Errno::EISDIR", sp_sprintf("Is a directory @ io_fread - %s", path));
  }
  FILE *f = fopen(path, "rb");
  sp_IntArray *a = sp_IntArray_new();
  if (!f) {
    sp_raise_cls(errno == ENOENT ? "Errno::ENOENT" : errno == EACCES ? "Errno::EACCES" : "RuntimeError",
                 sp_sprintf("%s @ rb_sysopen - %s", strerror(errno), path));
    return a;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *buf = (unsigned char *)malloc(sz > 0 ? (size_t)sz : 1);
  if (buf && sz > 0) {
    /* Use fread's actual byte count, not the raw file size — a
       partial read otherwise pushes uninitialized memory. */
    size_t r = fread(buf, 1, (size_t)sz, f);
    for (size_t i = 0; i < r; i++) sp_IntArray_push(a, (mrb_int)buf[i]);
  }
  free(buf);
  fclose(f);
  return a;
}

/* `arr.slice!(from, n)` — returns a fresh array of `n` elements
   starting at `from` and removes them from `a`. IntArray uses its
   `start` field for an O(1) head peel (from == 0); the others
   shift the tail down to fill the hole. */
/* at_exit hooks: a static LIFO of registered procs. Initialized
   zero-len in BSS; main()'s tail walks it in reverse-registration
   order before returning. */
#define SP_AT_EXIT_MAX 256
struct sp_Proc;
static struct sp_Proc *sp_at_exit_hooks[SP_AT_EXIT_MAX];
static mrb_int sp_at_exit_count = 0;

typedef struct sp_Proc { void *fn; void *cap; void (*cap_scan)(void *); mrb_int arity; mrb_bool lambda_p; mrb_int param_count; const sp_sym *param_kinds; const sp_sym *param_names; } sp_Proc;
static void sp_Proc_scan(void *p) { sp_Proc *pr = (sp_Proc *)p; if (pr->cap && pr->cap_scan) pr->cap_scan(pr->cap); }
static sp_Proc *sp_proc_new_meta(void *fn, void *cap, void (*cap_scan)(void *), mrb_int arity, mrb_bool lambda_p, mrb_int param_count, const sp_sym *param_kinds, const sp_sym *param_names) { sp_Proc *p = (sp_Proc *)sp_gc_alloc(sizeof(sp_Proc), NULL, sp_Proc_scan); p->fn = fn; p->cap = cap; p->cap_scan = cap_scan; p->arity = arity; p->lambda_p = lambda_p; p->param_count = param_count; p->param_kinds = param_kinds; p->param_names = param_names; return p; }
static sp_Proc *sp_proc_new(void *fn, void *cap, void (*cap_scan)(void *)) { return sp_proc_new_meta(fn, cap, cap_scan, 0, FALSE, 0, NULL, NULL); }

/* Bound Method object: `obj.method(:foo)` / `method(:foo)`. `self` is the
   bound receiver (NULL for a top-level method), `fn` the function address
   (cast to the right signature at the call site), `name` the method name
   (a string literal). Only `self` is GC-managed. */
static void sp_BoundMethod_scan(void *p) { sp_BoundMethod *m = (sp_BoundMethod *)p; if (m->self) sp_gc_mark(m->self); }
static sp_BoundMethod *sp_bound_method_new(void *self, mrb_int fn, const char *name) { sp_BoundMethod *m = (sp_BoundMethod *)sp_gc_alloc(sizeof(sp_BoundMethod), NULL, sp_BoundMethod_scan); m->self = self; m->fn = fn; m->name = name; return m; }

/* External Enumerator: a cursor over a snapshot of a collection's elements
   (boxed into a PolyArray at creation). #next / #peek walk the cursor and raise
   StopIteration past the end; #rewind resets it. Block-form chains (each.map,
   each.with_index, ...) are handled by codegen and never build this object. */
/* Two flavors: a materialized snapshot (items + cursor, from a collection's
   blockless #each) or a fiber-backed generator (Enumerator.new { |y| ... },
   where `y << v` is a Fiber.yield). The fiber is created lazily on first #next
   and re-created on #rewind. */
typedef struct {
  sp_PolyArray *items; mrb_int cursor;   /* materialized mode (items != NULL) */
  void (*gen)(sp_Fiber *);                /* generator body (fiber mode, gen != NULL) */
  void *gen_cap;                          /* captures, passed via fiber user_data */
  sp_Fiber *fib;                          /* current generator fiber (lazy) */
  mrb_bool peeked; sp_RbVal peek_val;     /* #peek lookahead cache */
} sp_Enumerator;
static sp_PolyArray *sp_enum_items_from(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ) {
    void *p = v.v.p;
    switch (v.cls_id) {
      case SP_BUILTIN_INT_ARRAY:  return sp_IntArray_to_poly((sp_IntArray *)p);
      case SP_BUILTIN_STR_ARRAY:  return sp_StrArray_to_poly_fmt((sp_StrArray *)p);
      case SP_BUILTIN_POLY_ARRAY: { sp_PolyArray *a = (sp_PolyArray *)p; sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r); if (a) for (mrb_int i = 0; i < a->len; i++) sp_PolyArray_push(r, a->data[i]); return r; }
      case SP_BUILTIN_FLT_ARRAY:  { sp_FloatArray *a = (sp_FloatArray *)p; sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r); if (a) for (mrb_int i = 0; i < a->len; i++) sp_PolyArray_push(r, sp_box_float(a->data[i])); return r; }
      case SP_BUILTIN_SYM_ARRAY:  { sp_IntArray *a = (sp_IntArray *)p; sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r); if (a) for (mrb_int i = 0; i < a->len; i++) sp_PolyArray_push(r, sp_box_sym((sp_sym)a->data[a->start + i])); return r; }
    }
  }
  return sp_PolyArray_new();
}
static void sp_Enumerator_scan(void *p) {
  sp_Enumerator *e = (sp_Enumerator *)p;
  if (e->items) sp_gc_mark(e->items);
  if (e->fib) sp_gc_mark(e->fib);
  if (e->gen_cap) sp_gc_mark(e->gen_cap);
  if (e->peeked) sp_mark_rbval(e->peek_val);
}
static sp_Enumerator *sp_Enumerator_new_from(sp_RbVal arr) {
  sp_PolyArray *items = sp_enum_items_from(arr);
  SP_GC_ROOT(items);
  sp_Enumerator *e = (sp_Enumerator *)sp_gc_alloc(sizeof(sp_Enumerator), NULL, sp_Enumerator_scan);
  e->items = items; e->cursor = 0; e->gen = NULL; e->gen_cap = NULL; e->fib = NULL; e->peeked = FALSE;
  return e;
}
/* Like sp_Enumerator_new_from but over a reversed snapshot, for a blockless
   Array#reverse_each. sp_enum_items_from always returns a fresh, owned poly
   array (every arm allocates a new one), so reversing it in place does not
   touch the receiver's backing store. (The reverse_each-doesn't-mutate test
   guards this invariant if sp_enum_items_from is ever changed to share.) */
static sp_Enumerator *sp_Enumerator_new_from_rev(sp_RbVal arr) {
  sp_PolyArray *items = sp_enum_items_from(arr);
  SP_GC_ROOT(items);
  if (items) {
    for (mrb_int i = 0, j = items->len - 1; i < j; i++, j--) {
      sp_RbVal t = items->data[i]; items->data[i] = items->data[j]; items->data[j] = t;
    }
  }
  sp_Enumerator *e = (sp_Enumerator *)sp_gc_alloc(sizeof(sp_Enumerator), NULL, sp_Enumerator_scan);
  e->items = items; e->cursor = 0; e->gen = NULL; e->gen_cap = NULL; e->fib = NULL; e->peeked = FALSE;
  return e;
}
/* Wrap an already-built poly array as an Enumerator, taking ownership of it
   (no re-snapshot). Lets callers that already hold a fresh array skip the
   sp_enum_items_from copy in sp_Enumerator_new_from. */
static sp_Enumerator *sp_Enumerator_new_from_items(sp_PolyArray *items) {
  SP_GC_ROOT(items);
  sp_Enumerator *e = (sp_Enumerator *)sp_gc_alloc(sizeof(sp_Enumerator), NULL, sp_Enumerator_scan);
  e->items = items; e->cursor = 0; e->gen = NULL; e->gen_cap = NULL; e->fib = NULL; e->peeked = FALSE;
  return e;
}
/* A string's characters as a fresh poly array of one-char Strings, built
   directly. Used by a blockless String#each_char enumerator, avoiding the
   intermediate sp_StrArray that sp_str_chars + sp_enum_items_from would
   allocate and then copy. */
static sp_PolyArray *sp_str_chars_poly(const char *s) {
  sp_PolyArray *a = sp_PolyArray_new();
  SP_GC_ROOT(a);
  if (!s) sp_nil_recv("chars");
  for (const char *p = s; *p; ) {
    int n = sp_utf8_advance(p);
    char *c = sp_str_alloc(n); memcpy(c, p, n); c[n] = 0;
    sp_PolyArray_push(a, sp_box_str(c));
    p += n;
  }
  return a;
}
static sp_Enumerator *sp_Enumerator_new_gen(void (*gen)(sp_Fiber *), void *cap) {
  sp_Enumerator *e = (sp_Enumerator *)sp_gc_alloc(sizeof(sp_Enumerator), NULL, sp_Enumerator_scan);
  e->items = NULL; e->cursor = 0; e->gen = gen; e->gen_cap = cap; e->fib = NULL; e->peeked = FALSE;
  return e;
}
/* Pull the next value from the generator fiber, or raise StopIteration when it
   has run to completion. A resume that ends the body terminates the fiber and
   returns the body value, which is discarded in favor of StopIteration. */
static sp_RbVal sp_enum_gen_pull(sp_Enumerator *e) {
  if (!e->fib) {
    e->fib = sp_Fiber_new(e->gen);
    if (e->gen_cap) e->fib->user_data = e->gen_cap;
  }
  if (!sp_Fiber_alive(e->fib)) sp_raise_cls("StopIteration", "iteration reached an end");
  sp_RbVal v = sp_Fiber_resume(e->fib, sp_box_nil());
  if (!sp_Fiber_alive(e->fib)) sp_raise_cls("StopIteration", "iteration reached an end");
  return v;
}
static sp_RbVal sp_Enumerator_next(sp_Enumerator *e) {
  if (e->gen) {
    if (e->peeked) { e->peeked = FALSE; return e->peek_val; }
    return sp_enum_gen_pull(e);
  }
  if (!e->items || e->cursor >= e->items->len) sp_raise_cls("StopIteration", "iteration reached an end");
  return e->items->data[e->cursor++];
}
static sp_RbVal sp_Enumerator_peek(sp_Enumerator *e) {
  if (e->gen) {
    if (!e->peeked) { e->peek_val = sp_enum_gen_pull(e); e->peeked = TRUE; }
    return e->peek_val;
  }
  if (!e->items || e->cursor >= e->items->len) sp_raise_cls("StopIteration", "iteration reached an end");
  return e->items->data[e->cursor];
}
static sp_Enumerator *sp_Enumerator_rewind(sp_Enumerator *e) {
  if (e->gen) { e->fib = NULL; e->peeked = FALSE; }
  else e->cursor = 0;
  return e;
}
static mrb_int sp_Enumerator_size(sp_Enumerator *e) { return e && e->items ? e->items->len : 0; }
/* Enumerator#take(n) / #first(n): collect up to n values from a fresh run of the
   source (independent of the #next cursor), matching CRuby. */
static sp_PolyArray *sp_Enumerator_take(sp_Enumerator *e, mrb_int n) {
  sp_PolyArray *r = sp_PolyArray_new();
  SP_GC_ROOT(r);
  if (n <= 0) return r;
  if (e->gen) {
    sp_Fiber *f = sp_Fiber_new(e->gen);
    SP_GC_ROOT(f);
    if (e->gen_cap) f->user_data = e->gen_cap;
    for (mrb_int i = 0; i < n; i++) {
      if (!sp_Fiber_alive(f)) break;
      sp_RbVal v = sp_Fiber_resume(f, sp_box_nil());
      if (!sp_Fiber_alive(f)) break;
      sp_PolyArray_push(r, v);
    }
    return r;
  }
  mrb_int lim = e->items ? e->items->len : 0;
  if (n < lim) lim = n;
  for (mrb_int i = 0; i < lim; i++) sp_PolyArray_push(r, e->items->data[i]);
  return r;
}
/* Enumerator#to_a / #entries: drain the whole source into an array (a fresh run
   of the generator, independent of the #next cursor), matching CRuby. */
static sp_PolyArray *sp_Enumerator_to_a(sp_Enumerator *e) {
  sp_PolyArray *r = sp_PolyArray_new();
  SP_GC_ROOT(r);
  if (!e) return r;
  if (e->gen) {
    sp_Fiber *f = sp_Fiber_new(e->gen);
    SP_GC_ROOT(f);
    if (e->gen_cap) f->user_data = e->gen_cap;
    while (sp_Fiber_alive(f)) {
      sp_RbVal v = sp_Fiber_resume(f, sp_box_nil());
      if (!sp_Fiber_alive(f)) break;
      sp_PolyArray_push(r, v);
    }
    return r;
  }
  if (e->items) for (mrb_int i = 0; i < e->items->len; i++) sp_PolyArray_push(r, e->items->data[i]);
  return r;
}
/* Universal proc return channel: every first-class proc publishes its result
   here, boxed, and the .call site unboxes it back to the call's inferred type
   (CRuby's uniform boxed-VALUE proc ABI). A file-static per TU, like the
   sp_exc_stack machinery -- the compose/curry trampolines below and every
   generated proc body live in the same TU and share this slot. Per-worker
   (SP_TLS): a concurrent Proc#call would otherwise race, and no safepoint poll
   lies between a body's store and the call site's read. */
static SP_TLS sp_RbVal _sp_proc_poly_ret;
static mrb_int sp_proc_arity(sp_Proc *p) { return p ? p->arity : 0; }
static mrb_bool sp_proc_lambda_p(sp_Proc *p) { return p ? p->lambda_p : FALSE; }
static mrb_int sp_proc_call(sp_Proc *p, mrb_int argc, mrb_int *args) { if (!p || !p->fn) return 0; if (!args) { mrb_int noargs[16] = {0}; return ((mrb_int (*)(void *, mrb_int, mrb_int *))p->fn)(p->cap, 0, noargs); } return ((mrb_int (*)(void *, mrb_int, mrb_int *))p->fn)(p->cap, argc, args); }
/* Lambda strict-arity check: raise ArgumentError if argc is outside
   [req, req+opt] (no upper bound with a rest param). Procs are lenient. */
static void sp_proc_lambda_arity_check(mrb_int argc, mrb_int req, mrb_int opt, mrb_bool has_rest) {
  if (argc < req || (!has_rest && argc > req + opt)) sp_raise_cls("ArgumentError", "wrong number of arguments");
}
static sp_PolyArray *sp_proc_parameters(sp_Proc *p) { sp_PolyArray *r = sp_PolyArray_new(); if (!p || p->param_count <= 0 || !p->param_kinds) return r; SP_GC_ROOT(r); for (mrb_int i = 0; i < p->param_count; i++) { sp_PolyArray *pair = sp_PolyArray_new(); sp_PolyArray_push(pair, sp_box_sym(p->param_kinds[i])); if (p->param_names && p->param_names[i] >= 0) sp_PolyArray_push(pair, sp_box_sym(p->param_names[i])); sp_PolyArray_push(r, sp_box_poly_array(pair)); } return r; }

/* Proc#<< / Proc#>> composition. The composed proc captures the two
   operands and, on call, threads its single argument through inner
   then outer: `(f << g).call(x)` == f(g(x)). For `>>` the codegen
   swaps the operands so `(f >> g).call(x)` == g(f(x)). */
typedef struct { sp_Proc *outer; sp_Proc *inner; } sp_ProcCompose;
static void sp_proc_compose_scan(void *p) { sp_ProcCompose *c = (sp_ProcCompose *)p; if (c->outer) sp_gc_mark(c->outer); if (c->inner) sp_gc_mark(c->inner); }
static mrb_int sp_proc_compose_fn(void *cap, mrb_int argc, mrb_int *args) {
  sp_ProcCompose *c = (sp_ProcCompose *)cap;
  mrb_int inner_args[16] = {0};
  if (args && argc > 0) inner_args[0] = args[0];
  /* the inner proc publishes its (boxed) result through the return slot; read
     it back to thread through the outer proc's mrb_int arg channel. */
  sp_proc_call(c->inner, 1, inner_args);
  mrb_int mid = sp_poly_to_i(_sp_proc_poly_ret);
  mrb_int outer_args[16] = {0};
  outer_args[0] = mid;
  /* the outer proc publishes the composed result into the slot; our own raw
     return is unread (the call site reads the slot). */
  return sp_proc_call(c->outer, 1, outer_args);
}
static sp_Proc *sp_proc_compose(sp_Proc *outer, sp_Proc *inner) {
  sp_ProcCompose *c = (sp_ProcCompose *)sp_gc_alloc(sizeof(sp_ProcCompose), NULL, sp_proc_compose_scan);
  c->outer = outer;
  c->inner = inner;
  return sp_proc_new_meta((void *)sp_proc_compose_fn, c, sp_proc_compose_scan, 1, TRUE, 1, NULL, NULL);
}
/* Proc#curry: an immutable argument accumulator over an sp_Proc target.
   `proc.curry` makes an empty accumulator; each `[arg]` returns a fresh
   accumulator with `arg` appended; the fully-applied value is realized
   by calling the target with the collected (mrb_int) arguments. Spinel
   defers the call to the point of use (sp_curry_to_int), so a partial
   curry behaves as a deferred call rather than auto-invoking at arity. */
typedef struct { sp_Proc *target; mrb_int nargs; mrb_int args[16]; } sp_Curry;
static void sp_curry_scan(void *p) { sp_Curry *c = (sp_Curry *)p; if (c->target) sp_gc_mark(c->target); }
static sp_Curry *sp_curry_new(sp_Proc *p) {
  sp_Curry *c = (sp_Curry *)sp_gc_alloc(sizeof(sp_Curry), NULL, sp_curry_scan);
  c->target = p; c->nargs = 0;
  return c;
}
static sp_Curry *sp_curry_apply(sp_Curry *c, mrb_int arg) {
  sp_Curry *n = (sp_Curry *)sp_gc_alloc(sizeof(sp_Curry), NULL, sp_curry_scan);
  *n = *c;
  if (n->nargs < 16) n->args[n->nargs++] = arg;
  return n;
}
static mrb_int sp_curry_to_int(sp_Curry *c) {
  if (!c || !c->target) return 0;
  /* the target publishes its (boxed) result through the return slot */
  sp_proc_call(c->target, c->nargs, c->args);
  return sp_poly_to_i(_sp_proc_poly_ret);
}

/* Hash#to_proc cap-scan: the proc's `cap` field IS the source hash
   (a single GC pointer), so marking it keeps the hash alive for the
   proc's lifetime. The per-variant lookup fn is emitted by codegen
   alongside the hash type it closes over. */
static void sp_hashproc_cap_scan(void *p) { sp_gc_mark(p); }

/* Random — per-instance PRNG. CRuby uses MT19937; spinel uses a
   portable xorshift64 (rand_r is POSIX-only, absent on MinGW), so
   the *sequence* differs from MRI but each Random object keeps its
   own reproducible stream from its seed. Issue #898. */
typedef struct { uint64_t state; } sp_Random;
static uint64_t sp_random_next(sp_Random *r) {
  uint64_t x = r->state ? r->state : 0x9E3779B97F4A7C15ULL;
  x ^= x << 13; x ^= x >> 7; x ^= x << 17;
  r->state = x;
  return x;
}
static sp_Random *sp_Random_new(mrb_int seed) {
  sp_Random *r = (sp_Random *)sp_gc_alloc(sizeof(sp_Random), NULL, NULL);
  r->state = (uint64_t)seed ^ 0x9E3779B97F4A7C15ULL;
  return r;
}
static mrb_int sp_Random_rand_int(sp_Random *r, mrb_int n) {
  if (!r || n <= 0) return 0;
  return (mrb_int)(sp_random_next(r) % (uint64_t)n);
}
static mrb_float sp_Random_rand_float(sp_Random *r) {
  if (!r) return 0.0;
  return (mrb_float)(sp_random_next(r) >> 11) / (mrb_float)(1ULL << 53);
}
/* Class-method forms (`Random.rand` / `Random.bytes`) share one
   lazily-seeded default instance, mirroring CRuby's Random::DEFAULT. */
/* Per-worker (SP_TLS) in the threaded build: the default xorshift PRNG has no
   internal lock, so a shared copy would race across workers (libc rand(), used
   by the bare rand()/rand(int)/shuffle/sample paths, is glibc-thread-safe and so
   needs no such treatment). Each worker seeds its own from time + its TLS address
   so the sequences differ. Byte-identical (single static, unchanged seed) in the
   single-threaded build. */
static SP_TLS sp_Random sp_random_default = { 0 };
static sp_Random *sp_random_default_get(void) {
  if (sp_random_default.state == 0) {
    sp_random_default.state = (uint64_t)time(NULL) ^ 0x9E3779B97F4A7C15ULL;
#ifdef SP_THREADS
    sp_random_default.state ^= (uint64_t)(uintptr_t)&sp_random_default;  /* distinct per worker */
#endif
  }
  return &sp_random_default;
}
/* Random#bytes(n) — n random bytes as a String. Uses sp_str_set_len
   so embedded NULs are preserved and #length reports n. */
static const char *sp_Random_bytes(sp_Random *r, mrb_int n) {
  if (n < 0) n = 0;
  char *b = sp_str_alloc((size_t)n);
  for (mrb_int i = 0; i < n; i++) b[i] = (char)(sp_random_next(r) & 0xff);
  b[n] = 0;
  sp_str_set_len(b, (size_t)n);
  return b;
}

/* StringIO is a native-bound spin package (packages/stringio). */

/* (the unused sp_Val lambda-closure runtime was removed: it was dead code   and its `sp_Val` typedef collided with a user class named Val -- issue #1774) */


/* Bigint (linked from sp_bigint.o) */
typedef struct sp_Bigint sp_Bigint;
sp_Bigint *sp_bigint_new_int(int64_t v);
sp_Bigint *sp_bigint_new_str(const char *s, int base);
sp_Bigint *sp_bigint_add(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_sub(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mul(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_div(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mod(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_pow(sp_Bigint *base, int64_t exp);
int sp_bigint_cmp(sp_Bigint *a, sp_Bigint *b);
int64_t sp_bigint_to_int(sp_Bigint *b);
const char *sp_bigint_to_s(sp_Bigint *b);
void sp_bigint_free(sp_Bigint *b);

/* Bitwise ops on bigint operands. Used by --int-overflow=promote
   mode where all int slots widen to sp_Bigint *. Implemented via
   int64 round-trip: bigint values produced by promotion are
   almost always derived from small ints (counters, masks, bit-
   width-sized values <= 64 bits), so the int64 path preserves
   the full Ruby-side semantics for any value that fits. Values
   exceeding int64 lose precision through these helpers -- those
   are extremely rare in practice (Ruby bitops on integers >
   2^63), and proper mpz_and / mpz_or / mpz_xor support can be
   added later in lib/sp_bigint.c if a real workload needs it. */
sp_Bigint *sp_bigint_and(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_or(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_xor(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_shl(sp_Bigint *a, int64_t n);
sp_Bigint *sp_bigint_shr(sp_Bigint *a, int64_t n);
sp_Bigint *sp_bigint_not(sp_Bigint *a);


/* Bigint (linked from libspinel_rt.a) */
typedef struct sp_Bigint sp_Bigint;
sp_Bigint *sp_bigint_new_int(int64_t v);
sp_Bigint *sp_bigint_new_str(const char *s, int base);
sp_Bigint *sp_bigint_add(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_sub(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mul(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_div(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_mod(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_pow(sp_Bigint *base, int64_t exp);
int sp_bigint_cmp(sp_Bigint *a, sp_Bigint *b);
int64_t sp_bigint_to_int(sp_Bigint *b);
const char *sp_bigint_to_s(sp_Bigint *b);
void sp_bigint_free(sp_Bigint *b);
sp_Bigint *sp_bigint_and(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_or(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_xor(sp_Bigint *a, sp_Bigint *b);
sp_Bigint *sp_bigint_shl(sp_Bigint *a, int64_t n);
sp_Bigint *sp_bigint_shr(sp_Bigint *a, int64_t n);
sp_Bigint *sp_bigint_not(sp_Bigint *a);

/* ---- Pack / Unpack (linked from sp_pack.o) ----
   Implementation lives in libspinel_rt.a; the entry points
   below call into the static GC helpers in this header via the
   sp_ext_* shims defined further down. */
const char *sp_IntArray_pack(sp_IntArray *arr, const char *fmt);
const char *sp_FloatArray_pack(sp_FloatArray *arr, const char *fmt);
const char *sp_PolyArray_pack(sp_PolyArray *arr, const char *fmt);
const char *sp_StrArray_pack(sp_StrArray *arr, const char *fmt);
sp_PolyArray *sp_str_unpack(const char *str, const char *fmt);
sp_PolyArray *sp_str_unpack_off(const char *str, const char *fmt, mrb_int byteoff);

/* Array#pack on a poly (nullable-array) receiver: dispatch on the runtime tag.
   A nil/non-array recv packs to the empty string. */
static inline const char *sp_poly_pack(sp_RbVal recv, const char *fmt) {
  if (recv.tag == SP_TAG_OBJ && recv.cls_id == SP_BUILTIN_INT_ARRAY)
    return sp_IntArray_pack((sp_IntArray *)recv.v.p, fmt);
  if (recv.tag == SP_TAG_OBJ && recv.cls_id == SP_BUILTIN_FLT_ARRAY)
    return sp_FloatArray_pack((sp_FloatArray *)recv.v.p, fmt);
  if (recv.tag == SP_TAG_OBJ && recv.cls_id == SP_BUILTIN_POLY_ARRAY)
    return sp_PolyArray_pack((sp_PolyArray *)recv.v.p, fmt);
  if (recv.tag == SP_TAG_OBJ && recv.cls_id == SP_BUILTIN_STR_ARRAY)
    return sp_StrArray_pack((sp_StrArray *)recv.v.p, fmt);
  return "";
}

/* StringScanner is a native-bound spin package (packages/strscan). */

/* The sp_ext_* shim wrappers are gone: string/object allocation, sp_box_*, and
   sp_PolyArray now live in the shared headers (sp_alloc.h / sp_gc.h), so lib C
   files (sp_pack.c, sp_strscan.c, sp_marshal.c, ...) allocate directly. */

#endif /* SP_RUNTIME_H */
