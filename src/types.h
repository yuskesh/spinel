/* The Spinel type lattice, as a C enum.
 *
 * The legacy compiler represents types as string tags ("int", "string",
 * "poly", "int_array", ...). We port the closed core as an enum and will
 * grow parameterized containers (arrays/hashes) into a richer struct as
 * later milestones need them. For now scalars + the poly top suffice.
 */
#ifndef SPINEL_TYPES_H
#define SPINEL_TYPES_H

/* Fast equality for short strings. The iterated analysis/codegen passes compare
   huge numbers of short keys / type names / method names; glibc's strcmp
   resolves to __strcmp_avx2, whose per-call AVX setup dominates for strings
   this short (and is paid in full even when the first byte already differs). An
   inline byte loop is several times cheaper here -- strcmp self-time was ~47% of
   a profiled optcarrot compile. Returns 1 if equal. */
static inline int sp_streq(const char *a, const char *b) {
  for (;; a++, b++) {
    if (*a != *b) return 0;
    if (*a == 0) return 1;
  }
}

/* Read-only Hash/Enumerable names a boxed receiver answers by pretending to be
   the general boxed-key/value hash: inference types the call that way and
   codegen normalizes the receiver to match, so the two agree by construction.
   Every name here must have a PolyPolyHash emitter, and none may mutate -- the
   normalization copies any other hash variant, so a write would be lost. */
static inline int ty_poly_hash_face_name(const char *nm) {
  static const char *const NAMES[] = {
    "dig", "value?", "has_value?", "invert", "assoc", "rassoc",
    "filter_map", "each_with_object", "group_by", "partition", "zip",
    "reduce", "inject", "find", "detect", "find_all", "take", "drop",
    "select", "filter", "reject", "compact", "slice",
    "except", "values_at", "fetch_values", "entries", "flatten", "first",
    "each_pair", "each_key", "each_value", "transform_values",
    "transform_keys", "tally", "chunk_while", "each_entry", "flat_map",
    "collect_concat", "none?", "one?",
    0 };
  if (!nm) return 0;
  for (int i = 0; NAMES[i]; i++) if (sp_streq(nm, NAMES[i])) return 1;
  return 0;
}

typedef enum {
  TY_UNKNOWN = 0,  /* not yet inferred, or an unsupported construct */
  TY_VOID,         /* a statement with no usable value */
  TY_NIL,
  TY_INT,
  TY_BIGINT,       /* arbitrary-precision integer (sp_Bigint *) */
  TY_FLOAT,
  TY_STRING,
  TY_STRBUF,       /* a mutable string (sp_String *); a storage refinement of
                      TY_STRING used for locals repeatedly appended via `<<`.
                      Demoted to TY_STRING on read, so it never escapes. */
  TY_SYMBOL,
  TY_BOOL,
  TY_RANGE,
  TY_FLOAT_RANGE,  /* (1.0..3.0): endpoints are mrb_float (sp_FloatRange) */
  TY_STR_RANGE,    /* ("a".."e"): endpoints are strings (sp_StrRange) */
  TY_TIME,
  TY_COMPLEX,      /* Cartesian Complex value (sp_Complex: re, im) */
  TY_RATIONAL,     /* Rational value (sp_Rational: num, den) */
  TY_MATCHDATA,
  TY_REGEX,
  TY_EXCEPTION,
  TY_INT_ARRAY,
  TY_FLOAT_ARRAY,
  TY_STR_ARRAY,
  TY_POLY_ARRAY,
  TY_INT_ARRAY_ARRAY, /* array of int-arrays (sp_PtrArray of sp_IntArray*):
                         the typed nested counterpart of TY_POLY_ARRAY for a
                         monomorphic array-of-int-array. Produced ONLY by the
                         post-fixpoint narrow_object_arrays pass, like
                         TY_OBJ_ARRAY, so it never cascades through the
                         fixpoint. Indexing yields a typed sp_IntArray* with no
                         per-element boxing (#3145 / BabyStark ExtField). */
  TY_STR_INT_HASH,
  TY_STR_STR_HASH,
  TY_INT_INT_HASH,
  TY_INT_STR_HASH,
  TY_SYM_POLY_HASH,  /* symbol keys, boxed (poly) values */
  TY_STR_POLY_HASH,  /* string keys, boxed (poly) values */
  TY_POLY_POLY_HASH, /* heterogeneous keys and values (both sp_RbVal) */
  TY_PROC,         /* a first-class Proc/lambda value (sp_Proc *) */
  TY_CURRY,        /* a curried Proc argument accumulator (sp_Curry *) */
  TY_FIBER,        /* a cooperative Fiber (sp_Fiber *) */
  TY_THREAD,       /* a green Thread (sp_thread *) */
  TY_QUEUE,        /* a thread-safe Queue (sp_queue *) */
  TY_MUTEX,        /* a Mutex (sp_mutex *) */
  TY_CONDVAR,      /* a ConditionVariable (sp_condvar *) */
  TY_RANDOM,       /* a per-instance PRNG (sp_Random *) */
  TY_DIR,          /* an open directory handle (sp_Dir *) */
  TY_ADDRINFO,     /* one resolved endpoint (sp_Addrinfo *) */
  TY_SOCKOPT,      /* Socket::Option (sp_SockOpt *) */
  TY_TMS,          /* Process.times -> Process::Tms (sp_Tms, four Floats) */
  TY_OPENSTRUCT,   /* OpenStruct: dynamic symbol->value members (#3135) */
  TY_METHOD,       /* a bound Method object (sp_BoundMethod *) */
  TY_IO,           /* a File/IO handle (sp_File *) */
  TY_ARGF,         /* the ARGF pseudo-IO singleton (sp_Argf *) */
  TY_ENUMERATOR,   /* an external Enumerator (sp_Enumerator *) */
  TY_CLASS,        /* a Class/Module value (sp_Class, carries cls_id) */
  TY_POLY          /* union / top: a value whose static type widened */
} TyKind;

const char *ty_name(TyKind t);         /* legacy string tag, for diagnostics */
int ty_is_numeric(TyKind t);           /* INT or FLOAT */
TyKind ty_promote_numeric(TyKind a, TyKind b); /* fold-accumulator numeric promotion */
int ty_is_array(TyKind t);
/* Set while the type fixpoint iterates; defined in analyze.c. Declared here
   because ty_array_of consults it -- see the TY_UNKNOWN case. */
extern int g_infer_optimistic;

TyKind ty_array_of(TyKind elem);       /* element type -> array kind */
TyKind ty_array_elem(TyKind arr);      /* array kind -> element type */
int ty_is_hash(TyKind t);
TyKind ty_hash_of(TyKind key, TyKind val); /* (key,val) -> hash kind (UNKNOWN if unsupported) */
TyKind ty_hash_key(TyKind h);
TyKind ty_hash_val(TyKind h);
const char *ty_hash_cname(TyKind h);   /* "StrInt" etc, for sp_<X>Hash_* */

/* Block-yield protocol for builtin iterators. The params a block bound to
   `recv.<name>` receives, expressed purely against the receiver's
   element/key/value -- i.e. the context-free iterators whose yield depends only
   on the receiver shape, not on call arguments or a receiver chain. Writes up
   to `max` param types into out[] and returns the count, or 0 if `name` is not
   such an iterator on `recv`. The single source of truth for builtin block
   protocols (forwarded-`&callable` desugar arity, builtin yield-stubs); keeps
   that knowledge from being re-encoded as scattered method-name lists. */
int ty_block_yield(TyKind recv, const char *name, TyKind *out, int max);

/* Iterator collection shape -- what a value-producing iterator BUILDS, as
   opposed to ty_block_yield's block-param types. One source of truth for the
   map/collect, select/filter, reject classifications codegen dispatches on. */
typedef enum { TY_ITER_NONE, TY_ITER_MAP, TY_ITER_SELECT, TY_ITER_REJECT } TyIterShape;
TyIterShape ty_iter_shape(const char *name);
/* Merge two observed types into the narrowest type covering both.
   Equal -> same; UNKNOWN acts as identity; otherwise widen to POLY
   (numeric int+float stays POLY for now -- mixed-numeric vars are rare
   and handled when they appear). */
TyKind ty_unify(TyKind a, TyKind b);
/* --- spinel-llvm patch L5 / E124 (SPINEL_LLVM_WIDEN_LOG) --------------------
   poly を作った合流の「由来」= どの規則が合流させたかを記録するため、ty_unify を
   マクロで ty_unify_at に差し替え、呼び出し地点を __FILE__:__LINE__ で載せる。
   217 箇所の呼び出しには 1 行も触らない。フラグ未設定なら何も集めない。 */
TyKind ty_unify_at(TyKind a, TyKind b, const char *site);
#define SP_LLVM_U2(x) #x
#define SP_LLVM_U1(x) SP_LLVM_U2(x)
#define ty_unify(a, b) ty_unify_at((a), (b), __FILE__ ":" SP_LLVM_U1(__LINE__))
/* --- end spinel-llvm patch L5 / E124 ---------------------------------------- */

/* User-defined object types are encoded above the built-in range so the
   flat TyKind node-type array still works: a class with index i has type
   TY_OBJECT_BASE + i. The class table (names, ivars) lives in Compiler. */
#define TY_OBJECT_BASE 1000
/* A homogeneous array of object class i (every element an instance of one
   user class) is encoded as TY_OBJ_ARRAY_BASE + i, above the object range.
   It maps to the runtime sp_PtrArray (unboxed void* elements), so indexing
   yields a typed `sp_X *` directly with no per-element boxing/dispatch --
   the typed counterpart of TY_POLY_ARRAY for monomorphic object arrays.
   Produced ONLY by the conservative post-fixpoint narrow_object_arrays pass,
   never by forward inference, so it cannot cascade through the fixpoint. */
#define TY_OBJ_ARRAY_BASE 1000000
static inline int    ty_is_object(TyKind t)   { return (int)t >= TY_OBJECT_BASE && (int)t < TY_OBJ_ARRAY_BASE; }
static inline TyKind ty_object(int class_id)  { return (TyKind)(TY_OBJECT_BASE + class_id); }
static inline int    ty_object_class(TyKind t){ return (int)t - TY_OBJECT_BASE; }
static inline int    ty_is_obj_array(TyKind t)   { return (int)t >= TY_OBJ_ARRAY_BASE; }
static inline TyKind ty_obj_array(int class_id)  { return (TyKind)(TY_OBJ_ARRAY_BASE + class_id); }
static inline int    ty_obj_array_class(TyKind t){ return (int)t - TY_OBJ_ARRAY_BASE; }
/* Both TY_OBJ_ARRAY and TY_INT_ARRAY_ARRAY are backed by sp_PtrArray (unboxed
   void* elements): code that only cares about the container representation (new,
   push, length, box, GC-root) uses this; the element cast on index differs. */
static inline int    ty_is_ptr_array(TyKind t)   { return ty_is_obj_array(t) || t == TY_INT_ARRAY_ARRAY; }

#endif
