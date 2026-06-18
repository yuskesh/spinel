/* sp_gc.h -- the mark/sweep collector's shared surface.
 *
 * Included by both the generated translation unit (via sp_runtime.h) and
 * lib/sp_gc.c, which holds the collector's non-inline machinery (mark,
 * sweep, collect, the memory-limit governor, and the SPINEL_GC_VERIFY
 * support). The hot inline mark helpers stay here so both sides inline
 * them -- moving the cold collector body to a single compiled unit must
 * not de-inline the per-object mark path. The collector globals are
 * declared extern here and defined once in lib/sp_gc.c.
 */
#ifndef SP_GC_H
#define SP_GC_H

#include "sp_types.h"

/* ---- Value tag constants + the boxed value (sp_RbVal) ----
 * The mark helpers below dispatch on the tag, so the type lives here
 * rather than in the generated TU. */
#define SP_TAG_INT 0
#define SP_TAG_STR 1
#define SP_TAG_FLT 2
#define SP_TAG_BOOL 3
#define SP_TAG_NIL 4
#define SP_TAG_OBJ 5
#define SP_TAG_SYM 6
#define SP_TAG_CLASS 7
#define SP_TAG_ENCODING 8
/* SP_TAG_OBJ cls_id sentinel for an opaque foreign/FFI pointer (e.g. a
   ffi_read_ptr / ffi func ptr return). It is NOT a sp_gc_alloc allocation, so
   the collector must not trace it -- sp_mark_rbval skips it. Kept here (not with
   the other SP_BUILTIN_* in sp_runtime.h) so the inline mark helper can see it.
   Value is the next free slot below SP_BUILTIN_METHOD (-24). */
#define SP_BUILTIN_FOREIGN_PTR (-25)
typedef struct { int tag; int cls_id; union { mrb_int i; const char *s; mrb_float f; mrb_bool b; void *p; } v; } sp_RbVal;

/* ---- Collector globals shared with the generated TU ----
 * Only the globals touched by both the kept hot path (sp_gc_alloc, the
 * SP_GC_ROOT macros, GC.stat, the fiber root hook) and the moved cold
 * body are extern; the rest stay static on whichever side owns them. */
#define SP_GC_STACK_MAX 65536
#define SP_GC_FULL_INTERVAL 8
/* Ractor isolation: every per-execution-unit mutable collector global is
   thread-local, so each Ractor pthread owns a private young/old heap, root
   stack, and byte counters and collects independently with no global GC
   lock. The mark/sweep hooks below are write-once shared state (installed
   on the main thread at startup, read by every Ractor), so they stay
   process-global. See lib/sp_ractor.c. */
extern __thread void **sp_gc_roots[SP_GC_STACK_MAX];
extern __thread int sp_gc_nroots;
extern __thread sp_gc_hdr *sp_gc_heap;
extern __thread size_t sp_gc_bytes;
extern __thread size_t sp_gc_old_bytes;
extern __thread int sp_gc_cycle;
extern void (*sp_gc_mark_suspended_fibers_hook)(void);

/* ---- Collector entry points (defined in lib/sp_gc.c) ---- */
void sp_gc_mark(void *obj);
void sp_gc_mark_all(void);
void sp_gc_collect(void);
void sp_gc_enforce_mem_limit(void);
void sp_oom_die(void);
/* Free this thread's private GC heaps on Ractor exit (see lib/sp_gc.c). */
void sp_gc_thread_teardown(void);
/* Installed by the generated TU: frees this thread's string heap on Ractor
   exit (sp_str_heap is static to the TU, so the collector reaches it by hook). */
extern void (*sp_gc_str_teardown_hook)(void);

/* ---- Embedder callbacks supplied by the generated TU ----
 * The collector cannot own the program's roots or string heap (they are
 * static state in the generated TU: the regexp match globals, ARGV, the
 * in-flight exception stack, and the heap-string free list). The TU
 * installs its mark-roots and string-sweep callbacks here at startup;
 * sp_gc_mark_all / sp_gc_collect invoke them through these pointers, the
 * same way fibers register sp_gc_mark_suspended_fibers_hook. */
extern void (*sp_gc_mark_globals_hook)(void);
extern void (*sp_gc_str_sweep_hook)(void);

/* ---- Hot inline mark helpers (inlined into both sides) ----
 * String tag bytes: 0xfe heap-unmarked -> 0xfc marked; others skipped. */
static inline void sp_mark_string(const char *s) {
  if (!s) return;
  if ((unsigned char)s[-1] == 0xfe) {
    ((char *)s)[-1] = (char)0xfc;
  }
}
static inline void sp_mark_rbval(sp_RbVal v) {
  if (v.tag == SP_TAG_STR) sp_mark_string(v.v.s);
  else if (v.tag == SP_TAG_OBJ && v.cls_id != SP_BUILTIN_FOREIGN_PTR) sp_gc_mark(v.v.p);
}
/* Closure-cell content markers. A captured non-int local is laundered into the
   pointer-sized mrb_int cell as (uintptr_t)<ptr>; the cell's GC scan marks the
   referent so it survives as long as the capturing proc does. */
static inline void sp_cell_scan_str(void *p) { sp_mark_string(*(const char **)p); }
static inline void sp_cell_scan_ptr(void *p) { sp_gc_mark(*(void **)p); }
/* A low-bit-tagged root entry is an sp_RbVal* (see SP_GC_ROOT_RBVAL);
   an untagged entry is a plain void** to a direct GC pointer. */
static inline void sp_gc_mark_root_entry(void **e) {
  uintptr_t u = (uintptr_t)e;
  if (u & (uintptr_t)1) { sp_mark_rbval(*(sp_RbVal *)(u & ~(uintptr_t)1)); }
  else { void *o = *e; if (o) sp_gc_mark(o); }
}

#endif
