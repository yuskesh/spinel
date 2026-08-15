/* sp_types.h -- core value-type definitions split out of spinel_rt.h.
 *
 * Holds the primitive typedefs, the small leaf value structs, the GC
 * header, and the typed array / non-poly hash structs. Both
 * spinel_rt.h (which includes this near the top) and libspinel_rt.a
 * sources can include it to see the layouts without pulling in the
 * header's static/inline function bodies. Pure type/macro definitions
 * only -- no function definitions, no global state.
 *
 * Reorganisation step toward slimming spinel_rt.h; sp_RbVal, the poly
 * containers, and the conditional Proc/Fiber/etc. types still live in
 * spinel_rt.h pending a later pass.
 */
#ifndef SP_TYPES_H
#define SP_TYPES_H

#ifdef __APPLE__
/* _DARWIN_C_SOURCE re-enables Darwin extensions (MAP_ANON, used by the fiber
   stack mmap in sp_fiber.c) that a strict POSIX build would otherwise hide.
   Define it here -- the lowest shared base header -- so every TU that pulls
   sp_types.h in sets it before the first system header.
   No _XOPEN_SOURCE / -Wdeprecated-declarations gate is needed: the portable
   asm context switch runs on every Darwin arch (x86_64 / arm64), so the only
   <ucontext.h> include (sp_fiber_ctx.h's non-asm fallback) is never compiled
   there, and PR #1563 removed the stale unconditional include from
   spinel_rt.h. */
#define _DARWIN_C_SOURCE
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <float.h>   /* DBL_MAX / DBL_MIN / DBL_EPSILON for Float::* constants */

/* Per-worker storage under true parallelism. In the -DSP_THREADS runtime
   variant (and the generated TU when the program uses threads, compiled with
   the same define) the per-thread execution state -- the GC root stack, the
   current fiber, the match registers -- is thread-local so each OS worker keeps
   its own. The single-threaded build leaves these plain globals, byte-identical
   to before. Both sides must agree on the storage class, so SP_TLS lives in
   this shared base header. */
#ifdef SP_THREADS
# define SP_TLS __thread
#else
# define SP_TLS
#endif

/* Worker identity, shared by the allocators (per-worker string/object heaps) and
   the scheduler that owns it. In the base header so sp_gc.h's SP_GC_HEAP_PUSH --
   included ahead of sp_alloc.h -- can index the per-worker object heap. */
#ifdef SP_THREADS
# ifndef SP_MAX_WORKERS
#  define SP_MAX_WORKERS 256
# endif
extern SP_TLS int sp_worker_id;      /* this OS worker's slot (0 = main); sp_sched.c */
extern int sp_active_workers;        /* live worker count; sp_sched.c */
#endif

/* Branch-hint / hot-cold attributes. Static approximation of PGO: marking
   rare paths (raise, dispatch fallbacks) cold lets the C compiler split
   them out of the hot path's i-cache footprint. No-op on non-GCC/clang. */
#if defined(__GNUC__) || defined(__clang__)
# define SP_LIKELY(x)   __builtin_expect(!!(x), 1)
# define SP_UNLIKELY(x) __builtin_expect(!!(x), 0)
# define SP_COLD        __attribute__((cold))
# define SP_NOINLINE    __attribute__((noinline))
# define SP_INLINE      inline __attribute__((always_inline))
# define SP_NORETURN    __attribute__((noreturn))
#else
# define SP_LIKELY(x)   (x)
# define SP_UNLIKELY(x) (x)
# define SP_COLD
# define SP_NOINLINE
# define SP_INLINE      inline
# define SP_NORETURN
#endif

/* mrb_int follows pointer width (decided at compile time via intptr_t):
   int64_t on 64-bit hosts -- PCs, no behavior change -- and int32_t on
   32-bit embedded targets, where it gives native-word arithmetic, half
   the memory for every integer/array/hash slot, and a pointer-width
   sp_RbVal union. The two paths differ only in that the 32-bit build
   overflows / narrows foreign 64-bit values (e.g. Time#to_i) at
   INT32 limits; see the overflow-mode handling. */
/* spinel-llvm Patch 4 (E010): SP_INT64 -- decouple mrb_int from pointer width.
   Opt-in, default OFF; with the flag unset every line below is the upstream
   text and the build is byte-identical.

   Motivation (wasm32/WASI, Cloudflare Workers). On a 32-bit target the
   pointer-width rule above makes Ruby's Integer 32-bit, but Spinel's *type
   inference runs on the 64-bit host*: the parser saturates literals at
   LLONG_MAX and the `**`/`<<` overflow oracles accumulate in host `long long`
   (src/spinel_parse.c, src/analyze_infer.c), so `2**40` is typed TY_INT and
   lowered into an int32_t slot -- RangeError at run time where CRuby gives
   1099511627776. Unlike a real 32-bit CPU, wasm32 has a native i64 ALU, and
   sp_RbVal is already 16 bytes there (the mrb_float member forces an 8-byte
   union), so widening mrb_int costs no layout change and no soft-int
   emulation. Setting SP_INT64 restores host==target integer semantics.

   Two things have to move together:
     (a) the typedef, and
     (b) INTPTR_MAX / INTPTR_MIN, which the tree uses throughout as the
         *mrb_int* limits, not as pointer limits: the beginless/endless Range
         sentinels (sp_range.h, sp_re.c, spinel_rt.h), SP_INT_NIL, the
         overflow guards in sp_core.c / sp_array.c / sp_format.c, and the
         `#if INTPTR_MAX == INT64_MAX` width selectors (spinel_rt.h's
         sp_i64_to_int, sp_core.c's SP_INT_POW10_LIMIT, sp_format.c). Every
         one of the ~45 uses is an integer limit; none is about a pointer.
         Re-pointing the two spellings keeps them in step with the typedef
         instead of silently disagreeing with it.
   The companion block in sp_core.h does the same for the helper ABI that
   spells mrb_int as `intptr_t`. */
#if defined(SP_INT64)
#undef  INTPTR_MAX
#define INTPTR_MAX INT64_MAX
#undef  INTPTR_MIN
#define INTPTR_MIN INT64_MIN
typedef int64_t  mrb_int;
typedef uint64_t mrb_uint;
typedef double   mrb_float;
typedef bool     mrb_bool;
#else
#if INTPTR_MAX == INT64_MAX
#elif INTPTR_MAX == INT32_MAX
#else
#error "spinel: unsupported intptr_t width (need 32- or 64-bit)"
#endif
typedef intptr_t mrb_int;
typedef double mrb_float;
typedef bool mrb_bool;
#endif

/* Sentinel value reserved by the int? (scalar-nullable int) type. An
   int? slot is bit-compatible with mrb_int; SP_INT_NIL marks the
   "nil" inhabitant. The pattern is INTPTR_MIN -- INT64_MIN on 64-bit
   (unchanged), INT32_MIN on 32-bit.
   `sp_int_is_nil(v)` is the canonical predicate; treat any int? value
   produced by runtime helpers as opaque outside this macro.

   KNOWN LIMITATION (32-bit builds only). The reservation is a single
   bit pattern, so a *genuine* integer equal to the sentinel is
   indistinguishable from nil. On 64-bit, INT64_MIN is effectively
   unreachable in practice (CRuby would have promoted it to Bignum), so
   this never bites. On 32-bit, INT32_MIN (-2147483648) is an ordinary
   reachable Integer, so a real -2147483648 flowing into an int? slot
   reads back as nil -- e.g. `[-2147483648].pop` yields nil instead of
   the value. This affects ONLY int? (nullable-int) slots; a plain
   (non-nullable) int holding -2147483648 is fine, since it never
   consults sp_int_is_nil. The integer-overflow helpers deliberately do
   NOT reserve this value (checking every add/sub/mul result against it
   would cost the hot path the embedded build is trying to save). Code that
   must store -2147483648 nullably on 32-bit should box it (poly) rather
   than use a flat int? slot. */
#define SP_INT_NIL ((mrb_int)INTPTR_MIN)
#define sp_int_is_nil(v) ((v) == SP_INT_NIL)

/* Nullable float (float?) sentinel: a quiet NaN with a reserved payload.
   NaN != NaN, so nil is detected by bit pattern, not ==. The payload is
   chosen so the canonical NaN (0x7FF8000000000000) and ordinary
   arithmetic NaNs don't collide; a real Float element with this exact
   bit pattern reads back as nil -- the same documented compromise
   SP_INT_NIL makes for INTPTR_MIN. mrb_float is double (8 bytes). */
#define SP_FLOAT_NIL_BITS ((uint64_t)0x7FF8000000000001ULL)
static inline mrb_float sp_float_nil(void) {
  union { uint64_t u; mrb_float d; } x; x.u = SP_FLOAT_NIL_BITS; return x.d;
}
static inline int sp_float_is_nil(mrb_float v) {
  union { mrb_float d; uint64_t u; } x; x.d = v; return x.u == SP_FLOAT_NIL_BITS;
}

/* sp_sym is defined per-program in emit_sym_runtime, but poly helpers
   below need to reference it by forward declaration. */
typedef mrb_int sp_sym;

#ifndef TRUE
#define TRUE true
#endif
#ifndef FALSE
#define FALSE false
#endif

/* ---- Leaf value structs ---- */
/* `step` carries the iteration direction/stride for the enumerator forms that a
   plain ascending range cannot express: n.downto(m) is {first:n,last:m,step:-1}.
   A zero step (every range built by the literal `a..b` / sp_range_new path) is
   treated as +1, so existing constructions need no change. */
typedef struct{mrb_int first;mrb_int last;mrb_int excl;mrb_int step;}sp_Range;
/* A Float range (1.0..3.0): endpoints kept as mrb_float so cover?/include?/begin/
   end are exact (an int-backed sp_Range truncated them). Iteration is a TypeError
   in Ruby (only #step traverses a Float range), so no step/iteration state here.
   beginless/endless ends use -HUGE_VAL / +HUGE_VAL. */
/* `omitted` records which bound was WRITTEN as absent (`(..5.0)`, `(1.0..)`),
   as opposed to an explicit infinity (`1..Float::INFINITY`): both are +/-HUGE_VAL
   in the value, and only #inspect / #to_s tell them apart -- CRuby prints
   "1.0.." for the first and "1.0..Infinity" for the second (#3670).
   bit 1 = begin omitted, bit 2 = end omitted. */
typedef struct{mrb_float first;mrb_float last;mrb_int excl;mrb_int omitted;}sp_FloatRange;
#define SP_FRANGE_NO_BEGIN 1
#define SP_FRANGE_NO_END   2
/* A mixed literal (1.5..5) is a Float range whose END was written as an
   Integer -- and CRuby renders each endpoint as the user wrote it. The bits
   record that, so #inspect prints "1.5..5" rather than "1.5..5.0" (#3896). */
#define SP_FRANGE_INT_BEGIN 4
#define SP_FRANGE_INT_END   8
/* A String range ("a".."e"): endpoints kept as GC-managed strings so #begin,
   #end, #to_s and #inspect answer as Ruby does. Every traversal (each/to_a/
   include?/...) materializes the element array through
   sp_StrArray_from_string_range, which is how the range behaved before it was
   a value of its own. */
typedef struct{const char *first;const char *last;mrb_int excl;}sp_StrRange;
/* A class value. `name`, when non-NULL, is a rodata class name carried by a
   class whose cls_id table entry may not exist (an exception's class -- the
   Errno:: family and many builtin error classes have no assigned cls_id). It
   takes precedence over cls_id for to_s / boxing / equality. */
typedef struct{mrb_int cls_id;const char *name;}sp_Class;
/* The nil-class sentinel: a TY_CLASS value can be nil (BasicObject#superclass),
   carried in-band via a reserved cls_id with no name -- the same nullable-scalar
   convention as SP_INT_NIL for int?. It is distinct from every real class id
   (user >= 0, builtins -100..-146, SP_CLASS_BY_NAME 0x7F000000). It MUST stay
   negative: the class-chain walks route a non-negative cls_id to the user-class
   table (`cur.cls_id>=0 ? sp_class_superclass : sp_builtin_superclass`), so a
   positive sentinel would be looked up as a user class. */
#define SP_CLASS_NIL_ID ((mrb_int)-900)
#define sp_class_nil_p(c) ((c).name == NULL && (c).cls_id == SP_CLASS_NIL_ID)
#define SP_CLASS_NIL ((sp_Class){SP_CLASS_NIL_ID, NULL})
/* A class known only by name (see sp_box_class_name in sp_alloc.h): marks a
   boxed SP_TAG_CLASS value whose v.s carries the name instead of a resolved
   cls_id. */
#define SP_CLASS_BY_NAME 0x7F000000
/* fl marks a component as Float-classed (renders "2.0"); clear bits keep the
   Integer-style rendering for whole values. Zero-init (every positional
   compound literal) is the Integer-classed default. */
typedef struct{mrb_float re;mrb_float im;unsigned char fl;}sp_Complex;
#define SP_CPLX_RE_F 1
#define SP_CPLX_IM_F 2
typedef struct{mrb_int num;mrb_int den;}sp_Rational;
typedef struct{const char *name;}sp_Encoding;

/* ---- GC headers ---- */
/* `marked` is a mark-generation STAMP, not a boolean: an object is live in
   the current collection iff marked == sp_gc_mark_gen. Bumping the generation
   at collect entry unmarks the whole heap in O(1), removing the two
   full-list walks (old unmark + minor re-mark) that made every minor
   collection O(live) even when the live set was untouched. 0 never equals a
   generation (the counter skips it), so freshly-calloc'd headers are unmarked. */
typedef struct sp_gc_hdr { struct sp_gc_hdr *next; void (*finalize)(void *); void (*scan)(void *); size_t size; unsigned marked : 29; unsigned frozen : 1;
   /* `old` says the object has survived a sweep and lives on the old list, so
      a reference stored into it may point at a younger object a minor mark
      would not reach on its own; `dirty` says it is already on the remembered
      set, so the write barrier's push happens once per collection rather than
      once per store. `marked` gives up a bit for them: it is a wrapping
      generation counter whose wrap path re-clears every stamp, and 2^29
      collections between wraps is far past anything real. */
   unsigned old : 1; unsigned dirty : 1; void (*recycle)(struct sp_gc_hdr *); } sp_gc_hdr;
/* size/len packed to uint32 (4 GB per-string cap, far beyond any real
   string) so the cached FNV `hash` fits without growing the 24-byte
   header -- i.e. zero per-string RSS cost vs the pre-cache layout.
   `size` is vestigial (written by sp_str_alloc, never read back; the
   str-heap sweep no longer folds string bytes into sp_gc_bytes).
   `hash` caches sp_str_hash for heap/heap-frozen keys; 0 == not yet
   computed, invalidated to 0 on any in-place mutation (setbyte/set_len). */
typedef struct sp_str_hdr { struct sp_str_hdr *next; uint32_t size; uint32_t len; uint64_t hash; } sp_str_hdr;

/* ---- Typed arrays ---- */
#define SP_STRARR_INLINE 4
typedef struct{mrb_int*data;mrb_int start;mrb_int len;mrb_int cap;mrb_int frozen;}sp_IntArray;
typedef struct{mrb_float*data;mrb_int len;mrb_int cap;mrb_int frozen;}sp_FloatArray;
typedef struct{void**data;mrb_int len;mrb_int cap;void(*scan_elem)(void*);mrb_int frozen;}sp_PtrArray;
typedef struct{const char**data;mrb_int len;mrb_int cap;mrb_int frozen;const char*inline_data[SP_STRARR_INLINE];}sp_StrArray;

/* ---- Non-poly typed hashes ---- */
typedef struct{const char**keys;mrb_int*vals;const char**order;mrb_int len;mrb_int cap;mrb_int mask;mrb_int default_v;}sp_StrIntHash;
typedef struct{const char**keys;const char**vals;const char**order;mrb_int len;mrb_int cap;mrb_int mask;const char*default_v;}sp_StrStrHash;
typedef struct{mrb_int*keys;const char**vals;mrb_int*order;mrb_bool*used;mrb_int len;mrb_int cap;mrb_int mask;const char*default_v;}sp_IntStrHash;
typedef struct{mrb_int*keys;mrb_int*vals;mrb_int*order;mrb_bool*used;mrb_int len;mrb_int cap;mrb_int mask;mrb_int default_v;}sp_IntIntHash;

/* Signal table bound (0..64): shared by the trap state in the generated TU
   and the trap machinery in lib/sp_cold.c. */
#define SP_SIG_MAX 65

#endif
