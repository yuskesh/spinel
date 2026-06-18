/* The Spinel type lattice, as a C enum.
 *
 * The legacy compiler represents types as string tags ("int", "string",
 * "poly", "int_array", ...). We port the closed core as an enum and will
 * grow parameterized containers (arrays/hashes) into a richer struct as
 * later milestones need them. For now scalars + the poly top suffice.
 */
#ifndef SPINEL_TYPES_H
#define SPINEL_TYPES_H

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
  TY_TIME,
  TY_COMPLEX,      /* Cartesian Complex value (sp_Complex: re, im) */
  TY_RATIONAL,     /* Rational value (sp_Rational: num, den) */
  TY_STRINGIO,
  TY_STRINGSCANNER,
  TY_MATCHDATA,
  TY_REGEX,
  TY_EXCEPTION,
  TY_INT_ARRAY,
  TY_FLOAT_ARRAY,
  TY_STR_ARRAY,
  TY_POLY_ARRAY,
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
  TY_RANDOM,       /* a per-instance PRNG (sp_Random *) */
  TY_METHOD,       /* a bound Method object (sp_BoundMethod *) */
  TY_IO,           /* a File/IO handle (sp_File *) */
  TY_ARGF,         /* the ARGF pseudo-IO singleton (sp_Argf *) */
  TY_CLASS,        /* a Class/Module value (sp_Class, carries cls_id) */
  TY_POLY          /* union / top: a value whose static type widened */
} TyKind;

const char *ty_name(TyKind t);         /* legacy string tag, for diagnostics */
int ty_is_numeric(TyKind t);           /* INT or FLOAT */
TyKind ty_promote_numeric(TyKind a, TyKind b); /* fold-accumulator numeric promotion */
int ty_is_array(TyKind t);
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

/* User-defined object types are encoded above the built-in range so the
   flat TyKind node-type array still works: a class with index i has type
   TY_OBJECT_BASE + i. The class table (names, ivars) lives in Compiler. */
#define TY_OBJECT_BASE 1000
static inline int    ty_is_object(TyKind t)   { return (int)t >= TY_OBJECT_BASE; }
static inline TyKind ty_object(int class_id)  { return (TyKind)(TY_OBJECT_BASE + class_id); }
static inline int    ty_object_class(TyKind t){ return (int)t - TY_OBJECT_BASE; }

#endif
