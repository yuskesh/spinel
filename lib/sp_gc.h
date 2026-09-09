/* sp_gc.h -- the mark/sweep collector's shared surface.
 *
 * Included by both the generated translation unit (via spinel_rt.h) and
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
#define SP_TAG_BIGINT 9   /* v.p is a GC-allocated sp_Bigint* */
/* SP_TAG_OBJ cls_id sentinel for an opaque foreign/FFI pointer (e.g. a
   ffi_read_ptr / ffi func ptr return). It is NOT a sp_gc_alloc allocation, so
   the collector must not trace it -- sp_mark_rbval skips it. Kept here (not with
   the other SP_BUILTIN_* in spinel_rt.h) so the inline mark helper can see it.
   Value is the next free slot below SP_BUILTIN_METHOD (-24). */
#define SP_BUILTIN_FOREIGN_PTR (-25)
/* a compiled Regexp: malloc-owned (never GC heap), so like FOREIGN_PTR the
   collector must not trace it */
#define SP_BUILTIN_REGEX       (-33)
/* Wide value types (heap-copied crossing into a poly slot). Shared here so
   lib/sp_marshal.c can recognize them by cls_id. */
#define SP_BUILTIN_COMPLEX  (-26)
#define SP_BUILTIN_RATIONAL (-27)
/* A Rational whose numerator/denominator exceed sp_int: a boxed object with
   two sp_Bigint* fields, distinct from the by-value int Rational (#2469). */
#define SP_BUILTIN_BIG_RATIONAL (-35)
/* A Float range (1.0..3.0): a boxed sp_FloatRange, distinct from the int-backed
   by-value Range so its endpoints are not truncated. */
#define SP_BUILTIN_FLOAT_RANGE (-36)
#define SP_BUILTIN_STR_RANGE   (-42)  /* ("a".."e"): sp_StrRange. Distinct from
                                         STRBUF/ADDRINFO, which -40 already
                                         names: a boxed string range answered
                                         String from every tag-keyed switch,
                                         so it was neither a Range nor
                                         enumerable through a poly slot (#3619) */
#define SP_BUILTIN_OPENSTRUCT  (-41)  /* OpenStruct: dynamic symbol->value members */
typedef struct { int tag; int cls_id; union { sp_int i; const char *s; sp_float f; sp_bool b; void *p; } v; } sp_RbVal;

/* ---- Collector globals shared with the generated TU ----
 * Only the globals touched by both the kept hot path (sp_gc_alloc, the
 * SP_GC_ROOT macros, GC.stat, the fiber root hook) and the moved cold
 * body are extern; the rest stay static on whichever side owns them. */
/* Capacity of the stack-resident GC root array (sp_gc_roots). At 8 bytes/entry
   the default is a 512 KB static buffer -- ample for desktop, but the dominant
   static allocation in a minimal binary. Embedded targets can shrink it with
   -DSP_GC_STACK_MAX=<n> (pass the SAME value when building lib/sp_gc.c and the
   generated TU -- both consult this for the array and the SP_GC_ROOT bound).
   Too small overflows silently into a dropped root (UAF), so size it to the
   program's deepest live-root nesting. */
/* SP_PROCESS_ARENA: the process-lifetime arena has no collection
   point, so no root is ever pushed and the 512 KB static array below is dead
   weight -- in a 128 MB Workers isolate it is real linear memory, and it is the
   dominant static allocation of a minimal binary (the note under the #ifndef).
   One entry keeps every declaration and bound check well-formed. */
/* The root stack keeps its normal size: the roots are still pushed (see the
   note on the macros above), so a smaller array would overflow and
   _sp_gc_root_push would start dropping entries. */
#ifndef SP_GC_STACK_MAX
#define SP_GC_STACK_MAX 65536
#endif
#define SP_GC_FULL_INTERVAL 8
/* Per-worker root stack (SP_TLS): each OS worker carries the active roots of
   the green thread it runs, swapped with the fiber's saved_roots on a context
   switch. Plain globals in the single-threaded build. */
extern SP_TLS void **sp_gc_roots[SP_GC_STACK_MAX];
extern SP_TLS int sp_gc_nroots;

/* GC root tracking. SP_GC_ROOT registers a stack-resident root with a
   cleanup-attribute sentinel so it auto-pops when its declaring scope ends.
   Shared here (was in spinel_rt.h) so standalone lib C files -- e.g. the
   Marshal loader, which builds GC arrays/hashes across a recursive parse --
   can root their in-flight objects too. Helpers touch only the extern root
   stack above, so relocating them is layout-neutral. */
static inline int _sp_gc_root_push(void **p) {
  if (sp_gc_nroots < SP_GC_STACK_MAX) { sp_gc_roots[sp_gc_nroots++] = p; return 1; }
  return 0;
}
static inline void _sp_gc_root_pop(int *added) { if (*added) sp_gc_nroots--; }
static inline void sp_gc_cleanup(int *p) { sp_gc_nroots = *p; }
#define _SP_GC_CONCAT2(a,b) a##b
#define _SP_GC_CONCAT(a,b) _SP_GC_CONCAT2(a,b)
/* ---- SP_PROCESS_ARENA: the root machinery, disabled ----
   In this configuration reclamation happens once, when the process exits, so
   sp_gc_alloc and sp_str_alloc never collect (see sp_alloc.c) and the collector
   never walks a root. A root that is never read is a store nobody loads.
   Removing it also removes the &local that forced the local into memory for the
   whole function.

   The speed effect of that second part was measured by the predecessor project
   on its own tree and hardware, not here, so no figure is quoted.

   sp_gc_nroots itself is kept (it stays 0): the generated TU reads it directly
   for the exception-landing watermark (sp_exc_rootmark[...] = sp_gc_nroots),
   which is not spelled through any macro here. */
/* The root machinery is NOT disabled under the arena, and that is deliberate.
   Turning SP_GC_ROOT into ((void)0) is an optimisation, not part of what makes
   this mode safe -- the safety comes from the collector never running. It also
   changes observable behaviour: the macros take &v, which forces the local into
   memory, and a local that lives in a register instead is indeterminate after a
   longjmp. catch/throw and raise/rescue are built on setjmp/longjmp, and the
   generated C does not mark every such local volatile, so removing the macros
   made `i = 0; catch(:done) { i += 1; throw :done }; p i` print 0 rather than 1.
   The roots are pushed and popped as usual here; nothing ever walks them. */
#if 0
#define SP_GC_SAVE()        ((void)0)
#define SP_GC_ROOT(v)       ((void)0)
#define SP_GC_ROOT_RBVAL(v) ((void)0)
#define SP_GC_ROOT_STR(v)   ((void)0)
#define SP_GC_RESTORE()     ((void)0)
#else
#define SP_GC_SAVE() int __attribute__((cleanup(sp_gc_cleanup))) _gc_saved = sp_gc_nroots
#define SP_GC_ROOT(v) int __attribute__((cleanup(_sp_gc_root_pop))) _SP_GC_CONCAT(_sp_gcr_, __COUNTER__) = _sp_gc_root_push((void**)&(v))
/* Root a poly (sp_RbVal) local: tag the stored slot's low bit so the mark
   walker routes it through sp_mark_rbval (the object pointer sits in a union at
   a nonzero offset, only for STR/OBJ tags). */
#define SP_GC_ROOT_RBVAL(v) int __attribute__((cleanup(_sp_gc_root_pop))) _SP_GC_CONCAT(_sp_gcr_, __COUNTER__) = _sp_gc_root_push((void**)((uintptr_t)&(v) | (uintptr_t)1))
/* Root a string slot that may hold a NON-spinel pointer (a stack line
   buffer from sp_File_gets_buf, an external char*): tag bit 2 routes the
   mark through sp_mark_string, which touches nothing unless the marker
   byte is exactly 0xfe -- safe on arbitrary memory, unlike sp_gc_mark's
   header walk. Use this for string parameters in runtime helpers. */
#define SP_GC_ROOT_STR(v) int __attribute__((cleanup(_sp_gc_root_pop))) _SP_GC_CONCAT(_sp_gcr_, __COUNTER__) = _sp_gc_root_push((void**)((uintptr_t)&(v) | (uintptr_t)2))
#define SP_GC_RESTORE() sp_gc_nroots = _gc_saved
#endif  /* SP_PROCESS_ARENA */

/* ---- write barrier ----
   A generational mark walks the young objects and whatever the roots reach; an
   old object it does not walk can still be the only thing holding a young one.
   The barrier records those: when a reference is stored into an object that has
   already been promoted, that object joins the remembered set, which a minor
   collection treats as an extra root.

   Cost is one load of a header bit already on the store's own cache line and a
   branch that steady state does not take -- measured at 0.5% on optcarrot when
   it fires on reference stores only, and 14% when it fires on every ivar store
   including the scalar ones, which is why the emitter discriminates.

   The set is a plain array with a dirty bit for deduplication. Overflow is
   safe rather than fatal: a full set means the next collection marks whole-heap
   (sp_gc_rem_overflow), which is exactly what today's collector always does. */
/* Wrap the OBJECT of a reference store: runs the barrier and yields the object
   itself, so it works wherever the store appears -- a statement, or an
   assignment inside a larger expression. The statement expression evaluates the
   object once, which a comma form would not. */
#if defined(SP_PROCESS_ARENA)
/* No mark ever runs, so the remembered set has no reader (E064). */
#define SP_WBO(x) (x)
#else
#define SP_WBO(x) ({ __typeof__(x) _sp_wbo = (x); sp_gc_wb((void *)_sp_wbo); _sp_wbo; })
#endif
#define SP_GC_REMEMBERED_MAX 65536
extern void *sp_gc_remembered[SP_GC_REMEMBERED_MAX];
extern int sp_gc_nremembered;
extern int sp_gc_rem_overflow;
extern int sp_gc_minor_on;   /* read by sp_gc_wb below; set once before main */
/* SPINEL_GC_OBJ_BUDGET=walk: the object collection budget is priced off the
   whole set a mark walks (objects + strings) rather than the object heap
   alone. Set once before main, beside the modes above; read by
   sp_gc_retune_object, which is where the reasoning lives. */
extern int sp_gc_obj_budget_walk;
/* Set for the duration of the string sweep hook on a minor cycle: only the
   young string list may be swept, because the mark that just ran did not
   walk old objects and so did not reach the strings they hold. */
extern int sp_gc_str_minor_only;
/* The barrier proper. Out of line and behind the mode test: with the minor
   mark off, which is the default, every store site pays one predictable
   branch instead of carrying the tag protocol and the remembered-set push
   inline. Inlining all of it cost ~5% on optcarrot, whose inner loops write
   object references per scanline. */
void sp_gc_wb_slow(void *obj);
#if defined(SP_PROCESS_ARENA)
static inline void sp_gc_wb(void *obj) { (void)obj; }
#else
static inline void sp_gc_wb(void *obj) {
  /* Nothing reads the remembered set unless a minor mark runs, and whether one
     can is decided once, from the environment, before main. So with the
     generational mark off -- the default -- the whole barrier is bookkeeping
     for a reader that never comes. rubys observed the other half of this from
     the source: `old` is set on every survivor regardless of the mode, so the
     barrier was doing its full work in both. */
  if (__builtin_expect(sp_gc_minor_on, 0)) sp_gc_wb_slow(obj);
}
#endif  /* SP_PROCESS_ARENA */
/* Young object heap. Threaded build: per-worker lists (one pusher each, since a
   started thread is pinned to its worker), so allocation pushes without the
   CAS-on-shared-head that made object-heavy parallel workloads bounce a cache
   line every alloc. Removals happen only under stop-the-world (every mutator
   parked). Survivors promote into the single shared old heap during the sweep,
   under stop-the-world, so the old list stays lock-free and shared. The live-
   byte counter sp_gc_bytes stays a single (relaxed-atomic) total -- array data
   buffers adjust it from whichever worker mutates them, which a per-worker split
   could not attribute correctly. */
#ifdef SP_THREADS
/* One cache-line-padded slot per worker holding the two fields written on every
   object allocation: the young list head and the unflushed live-byte delta.
   Padding is essential -- without it adjacent workers' 8-byte slots share a
   cache line, so a per-worker heap still bounced that line every alloc (false
   sharing), which kept object-heavy parallel allocation from scaling despite the
   lock/CAS removal. One line per worker isolates them completely. */
#define SP_CACHELINE 64
typedef struct {
  sp_gc_hdr *young;     /* per-worker young list head */
  size_t flush_delta;   /* per-worker unflushed live-byte delta (see below) */
  char _pad[SP_CACHELINE - sizeof(sp_gc_hdr*) - sizeof(size_t)];
} sp_gc_wslot_t;
extern sp_gc_wslot_t sp_gc_wslot[SP_MAX_WORKERS];
#else
extern sp_gc_hdr *sp_gc_heap;
#endif
/* Current mark generation (see sp_gc_hdr.marked in sp_types.h). */
extern unsigned sp_gc_mark_gen;
extern void (*sp_gc_obj_retune_hook)(size_t before);
extern size_t sp_gc_bytes;
extern size_t sp_gc_old_bytes;
extern int sp_gc_cycle;
/* SPINEL_GC_STATS=1 (report in sp_alloc.c): how many collections ran and what
   they cost. A program whose GC share of CPU climbs with concurrency looks
   from outside the process exactly like one collecting more often, and there
   was no counter to tell the two apart -- which is where the diagnosis in
   #4352 stopped. One clock pair and two increments per COLLECTION, so they
   are always kept; only the printing is gated. */
extern unsigned long long sp_gc_stat_collections;
extern unsigned long long sp_gc_stat_fulls;
extern double sp_gc_stat_seconds;
extern void (*sp_gc_mark_suspended_fibers_hook)(void);

/* Heap byte-counter accounting. The container growth paths (sp_array.h,
   sp_alloc.h's PolyArray, the string builder) adjust sp_gc_bytes inline
   WITHOUT the heap lock -- at N>1 workers that is a data race against the
   locked allocators and against each other (torn counters skew the GC
   trigger; TSan flags it). Under SP_THREADS every update and every
   trigger-decision read goes through a relaxed atomic: the counter is a
   heuristic (collection thresholds), so relaxed ordering is enough -- no
   other data is published through it. Collector-side code that runs under
   stop-the-world (sweep, retune) keeps plain accesses: every mutator is
   parked at the barrier, which already gives the happens-before edge.
   The single-threaded build expands to the exact plain +=/-= it had, so
   that archive stays byte-identical. */
#ifdef SP_THREADS
#define SP_GC_CTR_ADD(ctr, n) __atomic_fetch_add(&(ctr), (size_t)(n), __ATOMIC_RELAXED)
#define SP_GC_CTR_SUB(ctr, n) __atomic_fetch_sub(&(ctr), (size_t)(n), __ATOMIC_RELAXED)
#define SP_GC_CTR_GET(ctr)    __atomic_load_n(&(ctr), __ATOMIC_RELAXED)
#define SP_GC_CTR_SET(ctr, n) __atomic_store_n(&(ctr), (size_t)(n), __ATOMIC_RELAXED)
#else
#define SP_GC_CTR_ADD(ctr, n) ((ctr) += (size_t)(n))
#define SP_GC_CTR_SUB(ctr, n) ((ctr) -= (size_t)(n))
#define SP_GC_CTR_GET(ctr)    (ctr)
#define SP_GC_CTR_SET(ctr, n) ((ctr) = (size_t)(n))
#endif

/* Live-byte accounting for the object heap. Every allocation and every array-
   buffer resize adjusts sp_gc_bytes; under SP_THREADS a shared atomic RMW per
   op bounces one cache line across workers and dominated object-heavy parallel
   allocation (measured ~13x on the counter alone at 4 workers). Batch it: each
   worker accumulates its delta in a private (non-atomic) slot and flushes to the
   shared total only every SP_GC_FLUSH_QUANTUM, cutting the atomic frequency by
   ~quantum/alloc-size. sp_gc_bytes stays the authoritative shared total -- every
   read (threshold trigger, GC.stat) and the collector's recompute are unchanged;
   the trigger merely lags the true total by at most quantum*workers, bounded
   overshoot for a heuristic. The collector resets the deltas after its recompute
   (which already counts every live object's size, so the pending deltas are
   subsumed). The single-threaded build is the plain +=/-= it always was. */
#ifdef SP_THREADS
#define SP_GC_FLUSH_QUANTUM (16u * 1024u)
static inline void sp_gc_bytes_add(size_t n) {
  size_t *d = &sp_gc_wslot[sp_worker_id].flush_delta;
  size_t v = *d + n;
  if (v >= SP_GC_FLUSH_QUANTUM) { SP_GC_CTR_ADD(sp_gc_bytes, v); *d = 0; }
  else *d = v;
}
static inline void sp_gc_bytes_sub(size_t n) {
  size_t *d = &sp_gc_wslot[sp_worker_id].flush_delta;
  if (*d >= n) { *d -= n; }
  else { size_t rem = n - *d; *d = 0; SP_GC_CTR_SUB(sp_gc_bytes, rem); }
}
#else
static inline void sp_gc_bytes_add(size_t n) { sp_gc_bytes += n; }
/* Floored: the counter is a heuristic the retune divides by, and a wrapped
   value there is not a large heap but a collector that never triggers again
   (#4073). */
/* Floored. The counter is a heuristic the retune divides by and multiplies, and
   an array-growth path can subtract a capacity larger than the counter holds
   (measured: sp_array.h's StrArray grow, 20472 against 10344) -- wrapping it
   makes the trigger fire on every allocation until the next collection resets
   it, and used to make the retune set a threshold that never fires at all. */
static inline void sp_gc_bytes_sub(size_t n) {
  sp_gc_bytes = sp_gc_bytes >= n ? sp_gc_bytes - n : 0;
}
#endif

/* Push a header onto the shared sp_gc_heap list. Under SP_THREADS this is a
   lock-free CAS push so callers that hold no lock (the pool-hit relink) stay
   off the heap mutex; the allocators, which hold the mutex anyway for the
   collect trigger, use the same push so every writer to the list head agrees
   on one protocol (a plain locked store racing an unlocked CAS would itself
   be a race). The `next` store is atomic: a node being re-linked from a
   shared pool free list may still have a stale pool popper reading its
   `next` (that popper's CAS then fails and discards the value, but the read
   itself must be defined). Removals happen only in the stop-the-world sweep
   with every mutator parked, so the push never races a pop and needs no ABA
   defense. Release order publishes the node's initialized header to the
   collector. */
#ifdef SP_THREADS
/* Per-worker young list: only this worker's M pushes here (started threads are
   pinned and a worker pumps one green thread at a time), so a plain store is
   race-free -- no CAS, no shared-head cache-line bounce. */
#define SP_GC_HEAP_PUSH(hdr) do { \
    sp_gc_hdr **_sp_head = &sp_gc_wslot[sp_worker_id].young; \
    (hdr)->next = *_sp_head; *_sp_head = (hdr); \
  } while (0)
#else
#define SP_GC_HEAP_PUSH(hdr) do { (hdr)->next = sp_gc_heap; sp_gc_heap = (hdr); } while (0)
#endif

/* ---- SP_PROCESS_ARENA: the process-lifetime bump allocator ----
   One monotonically growing region, never reclaimed inside the process. The
   chunks are calloc'd, so every slice handed out is already zero and
   sp_gc_alloc keeps its calloc semantics at no per-object cost. Growth and the
   exhaustion policy (loud death, with the usage in the message) live in
   sp_gc_arena_more, out of line, in lib/sp_alloc.c. */
#if defined(SP_PROCESS_ARENA)
extern char *sp_gc_arena_cur;
extern char *sp_gc_arena_end;
void *sp_gc_arena_more(size_t n);          /* new chunk, or die loud */
size_t sp_gc_arena_reserved_bytes(void);   /* chunk bytes taken from libc */
size_t sp_gc_arena_used_bytes(void);       /* bytes handed out (chunk granularity) */
static inline void *sp_gc_arena_alloc(size_t n) {
  n = (n + (size_t)15) & ~(size_t)15;   /* calloc's alignment, preserved */
  char *p = sp_gc_arena_cur;
  if ((size_t)(sp_gc_arena_end - p) < n) return sp_gc_arena_more(n);
  sp_gc_arena_cur = p + n;
  return p;
}
#endif

/* ---- Collector entry points (defined in lib/sp_gc.c) ---- */
int  sp_gc_verify_on(void);   /* SPINEL_GC_VERIFY is set (diagnostics only) */
extern const char *sp_gc_dbg_phase;   /* which root group the mark walk is in */
extern void *sp_gc_dbg_ctx;
void sp_gc_mark(void *obj);
void sp_gc_mark_all(void);
void sp_gc_mark_drain(void);
#if defined(SP_PROCESS_ARENA)
/* Never called (sp_gc_collect returns immediately), so route every caller's
   copy to nothing. lib/sp_gc.c #undefs these three right after including this
   header, so the real definitions still compile and stay linkable. */
#define sp_gc_mark(o)      ((void)(o))
#define sp_gc_mark_all()   ((void)0)
#define sp_gc_mark_drain() ((void)0)
#endif
extern int sp_gc_minor;
extern int sp_gc_minor_on;
extern int sp_gc_verify_gen;
extern int sp_gc_verify_gen_fail;
extern int sp_gc_verify_probe_on, sp_gc_verify_probe_hit;
extern unsigned sp_gc_verify_probe;
/* Per-phase collector time, in seconds, cumulative (SPINEL_GC_PHASES=1; all
   zero when it is off). sp_gc_stat_seconds is their sum plus the bookkeeping
   between them. Reported by sp_alloc.c, which is where the stats line lives. */
extern double sp_gc_ph_mark, sp_gc_ph_oldsweep, sp_gc_ph_slotsweep,
              sp_gc_ph_rembclear, sp_gc_ph_strsweep, sp_gc_ph_trim;
/* The mark, split the way sp_gc_mark_all walks: this worker's own root stack,
   every live fiber's saved roots, the globals hook, then the trace that drains
   what those three found. Named as sp_gc_dbg_phase names them under verify, so
   a number leads to the code. They sum to sp_gc_ph_mark.

   The split exists because "mark grew" has two causes that need different
   answers: more ROOTS to scan (the fibers row, which grows with in-flight
   fibers) and more GRAPH to trace (the scan row, which grows because those
   fibers hold live objects). Only the first is what slicing the fiber list to
   the parked workers would address (#4384). */
extern double sp_gc_ph_mk_roots, sp_gc_ph_mk_fibers,
              sp_gc_ph_mk_globals, sp_gc_ph_mk_scan;
extern int sp_gc_ph_on;
void sp_gc_collect(void);
#ifdef SP_THREADS
/* Sweep one worker's young list on that worker (see sp_gc.c). Survivors come
   back as a local list for the collector to splice into the old heap. */
void sp_gc_sweep_slot(int wid, sp_gc_hdr **out_head, sp_gc_hdr **out_tail, size_t *out_bytes);
extern void (*sp_gc_par_sweep_hook)(void);
/* Splice one worker's survivors onto the shared old heap. Collector-only. */
void sp_gc_promote_slot(sp_gc_hdr *head, sp_gc_hdr *tail, size_t bytes);
#endif
void sp_gc_enforce_mem_limit(void);
/* Collect + re-tune the threshold, assuming exclusive heap access (see
   sp_alloc.c). sp_stw_collect (sp_sched.c, threaded build) stops the world then
   runs sp_gc_collect_retune; the single-threaded allocator calls it directly
   under the heap lock. */
void sp_gc_collect_retune(void);
void sp_stw_collect(void);
void sp_gc_collect_request(void);   /* explicit GC.start: same barrier, forced */
void sp_oom_die(void);

/* ---- Embedder callbacks supplied by the generated TU ----
 * The collector cannot own the program's roots or string heap (they are
 * static state in the generated TU: the regexp match globals, ARGV, the
 * in-flight exception stack, and the heap-string free list). The TU
 * installs its mark-roots and string-sweep callbacks here at startup;
 * sp_gc_mark_all / sp_gc_collect invoke them through these pointers, the
 * same way fibers register sp_gc_mark_suspended_fibers_hook. */
extern void (*sp_gc_mark_globals_hook)(void);
extern void (*sp_gc_str_sweep_hook)(void);

/* ---- value-introspection hooks (set by the generated TU at startup) ----
 * lib/sp_json.c (and other cold readers) own no container types; they reach the
 * generated TU's typed arrays/hashes only through these generic readers, the
 * same idiom as the GC hooks above. sp_sym_name maps a symbol id to its name;
 * sp_json_kind classifies a boxed value (1=array, 2=hash, 0=other); len/aref
 * iterate any array; hpair yields a hash's (key,value) at insertion index i. */
extern const char *(*sp_sym_name_fn)(sp_sym);
extern int (*sp_json_kind_fn)(sp_RbVal);
extern sp_int (*sp_json_len_fn)(sp_RbVal);
extern sp_RbVal (*sp_json_aref_fn)(sp_RbVal, sp_int);
extern void (*sp_json_hpair_fn)(sp_RbVal, sp_int, sp_RbVal *, sp_RbVal *);
/* Container BUILDERS for JSON.parse (installed by the generated TU, which owns
   the hash type): make an empty string-keyed hash, and set a (key, value) pair
   -- CRuby's JSON.parse returns String keys. Arrays are built directly from the
   package ABI (sp_PolyArray). */
extern sp_RbVal (*sp_json_mk_hash_fn)(void);
extern sp_sym (*sp_json_sym_intern_fn)(const char *);  /* symbolize_names key interner (TU-installed) */
extern void (*sp_json_hash_set_fn)(sp_RbVal, const char *, sp_RbVal);
/* Recursive #inspect of a boxed value, for lib/sp_inspect.c's container walker
   (set to sp_poly_inspect; same idiom as the JSON hooks). */
extern const char *(*sp_poly_inspect_fn)(sp_RbVal);
/* #to_s of any boxed value, for a package that writes one (StringIO#puts):
   the generated TU installs sp_poly_to_s, which renders a user object
   through its own #to_s or the #<Name:0xADDR> default. */
extern const char *(*sp_poly_to_s_fn)(sp_RbVal);
/* Convert a plain object (a Struct) to a boxed StrPoly hash of its members,
   generic (no format knowledge). The generated program installs it (switch on
   cls_id) when it has Structs and a package consumes it; a consumer such as
   the json package reads it to serialize an object as a hash. NULL otherwise. */
extern sp_RbVal (*sp_obj_to_hash_fn)(sp_RbVal);
extern const char *(*sp_obj_to_json_fn)(sp_RbVal);
/* Symbol-keyed Struct/Data #to_h, for a poly receiver (#2906). */
extern sp_RbVal (*sp_obj_to_h_fn)(sp_RbVal);
/* user-object #to_a for container-read poly receivers (#3234): installed by
   the generated prologue when any instantiated class defines a no-arg to_a */
extern sp_RbVal (*sp_obj_to_a_fn)(sp_RbVal);
/* #to_ary, the CONVERSION protocol -- distinct from to_a, the enumeration
   one: Kernel#Array asks to_ary first, and only a class that defines it
   answers here (#4187). */
extern sp_RbVal (*sp_obj_to_ary_fn)(sp_RbVal);
/* The array a `case/in` array pattern matches a user object against. Separate
   from #to_a: a Data answers #deconstruct but has no #to_a at all. */
extern sp_RbVal (*sp_obj_deconstruct_fn)(sp_RbVal);
/* 1 when cls_id is a Data class. Data defines no #dig, so a dig that lands on
   one is the TypeError CRuby raises rather than a member read; Struct, which
   does define #dig, is unaffected. */
extern int (*sp_obj_is_data_fn)(int cls_id);
/* Data#with copy-update for a poly receiver: (value, symbol-keyed overrides). (#2890) */
extern sp_RbVal (*sp_obj_with_fn)(sp_RbVal, sp_RbVal);
/* default Object#inspect for user objects: the generated TU installs a
   per-class ivar walk (sp_obj_inspect_sw); sp_poly_inspect's OBJ default
   consults it so nested/boxed objects render like CRuby */
extern const char *(*sp_obj_inspect_fn)(int cls_id, void *p);
/* Same shape for user #to_s: sp_poly_to_s's OBJ default consults it so a
   boxed user object with a custom to_s renders through it. */
extern const char *(*sp_obj_to_s_fn)(int cls_id, void *p);
/* CRuby's implicit conversion protocol on BOXED user objects: the runtime's
   Integer/String conversion sites (pack, typed-slot coercions) reach a
   compiled #to_int / #to_str through these. A class without the method is
   the default arm (*ok = 0 / NULL) and the caller raises CRuby's TypeError. */
extern sp_int (*sp_obj_to_int_fn)(int cls_id, void *p, int *ok);
extern const char *(*sp_obj_to_str_fn)(int cls_id, void *p);
/* #to_path, which File/Dir/IO's path slots ask before #to_str (CRuby's
   rb_get_path); NULL when the class defines none. */
extern const char *(*sp_obj_to_path_fn)(int cls_id, void *p);
/* Kernel#Integer / Kernel#Float on a boxed user object: the class's #to_int,
   #to_i, #to_f or #to_str (`which`, in that order), WHATEVER the method's
   static type -- CRuby calls it and judges the answer -- boxed into *out.
   Answers 1 when the class has the method, 0 when it does not; with a NULL
   `out` it only answers that, calling nothing. */
extern int (*sp_obj_conv_fn)(int cls_id, void *p, int which, sp_RbVal *out);
/* Ruby class name for a user cls_id (the generated id->name table), so a
   runtime TU can word a TypeError the way CRuby does. */
extern const char *(*sp_obj_cls_name_fn)(int cls_id);

/* ---- Hot inline mark helpers (inlined into both sides) ----
 * String tag bytes: 0xfe heap-unmarked -> 0xfc marked; others skipped. */
#if defined(SP_PROCESS_ARENA)
static inline void sp_mark_string(const char *s) { (void)s; }
static inline void sp_mark_rbval(sp_RbVal v) { (void)v; }
static inline void sp_cell_scan_str(void *p) { (void)p; }
static inline void sp_cell_scan_ptr(void *p) { (void)p; }
static inline void sp_cell_scan_rbval(void *p) { (void)p; }
static inline void sp_cell_scan_procint(void *p) { (void)p; }
static inline void sp_gc_mark_root_entry(void **e) { (void)e; }
#else
static inline void sp_mark_string(const char *s) {
  if (!s) return;
  if ((unsigned char)s[-1] == 0xfe) {
    ((char *)s)[-1] = (char)0xfc;
    return;
  }
  /* 0xfd is a mutable String's payload, whose lifetime belongs to the handle
     in front of it: marking the bytes alone leaves the handle unreferenced,
     and its finalizer frees the bytes this container still points at. The
     payload's `next` field carries that handle (sp_fd_own). */
  if ((unsigned char)s[-1] == 0xfd) {
    void *owner = (void *)(((const sp_str_hdr *)(s - 1)) - 1)->next;
    if (owner) sp_gc_mark(owner);
  }
  /* No frozen (0xf1) branch here: this is inlined into optcarrot's GC mark and
     is layout-sensitive. A live frozen heap string is kept immortal by
     sp_str_sweep instead (#1449). */
}
static inline void sp_mark_rbval(sp_RbVal v) {
  if (v.tag == SP_TAG_STR) sp_mark_string(v.v.s);
  else if (v.tag == SP_TAG_OBJ && v.cls_id != SP_BUILTIN_FOREIGN_PTR &&
           v.cls_id != SP_BUILTIN_REGEX) sp_gc_mark(v.v.p);
  else if (v.tag == SP_TAG_BIGINT) sp_gc_mark(v.v.p);
}
/* Closure-cell content markers. A captured non-int local is laundered into the
   pointer-sized sp_int cell as (uintptr_t)<ptr>; the cell's GC scan marks the
   referent so it survives as long as the capturing proc does. */
static inline void sp_cell_scan_str(void *p) { sp_mark_string(*(const char **)p); }
static inline void sp_cell_scan_ptr(void *p) { sp_gc_mark(*(void **)p); }
static inline void sp_cell_scan_rbval(void *p) { sp_mark_rbval(*(sp_RbVal *)p); }
/* A captured Proc rides in an sp_int cell as (sp_int)(uintptr_t)ptr -- the cell
   is an integer slot, but what it holds is a collectable object, and without a
   scan the capture kept the CELL alive and nothing kept the proc. A nested
   `proc { |v| two.call(v, v) }` then called through freed memory (#4077). */
static inline void sp_cell_scan_procint(void *p) {
  sp_int v = *(sp_int *)p;
  if (v) sp_gc_mark((void *)(uintptr_t)v);
}
/* A low-bit-tagged root entry is an sp_RbVal* (see SP_GC_ROOT_RBVAL);
   an untagged entry is a plain void** to a direct GC pointer. */
static inline void sp_gc_mark_root_entry(void **e) {
  uintptr_t u = (uintptr_t)e;
  if (u & (uintptr_t)1) { sp_mark_rbval(*(sp_RbVal *)(u & ~(uintptr_t)1)); }
  else if (u & (uintptr_t)2) { sp_mark_string(*(const char **)(u & ~(uintptr_t)2)); }
  else { void *o = *e; if (o) sp_gc_mark(o); }
}
#endif  /* SP_PROCESS_ARENA */

#endif
