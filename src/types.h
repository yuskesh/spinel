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
  TY_FLOAT,
  TY_STRING,
  TY_SYMBOL,
  TY_BOOL,
  TY_RANGE,
  TY_INT_ARRAY,
  TY_FLOAT_ARRAY,
  TY_STR_ARRAY,
  TY_POLY_ARRAY,
  TY_STR_INT_HASH,
  TY_STR_STR_HASH,
  TY_INT_INT_HASH,
  TY_INT_STR_HASH,
  TY_POLY          /* union / top: a value whose static type widened */
} TyKind;

const char *ty_name(TyKind t);         /* legacy string tag, for diagnostics */
int ty_is_numeric(TyKind t);           /* INT or FLOAT */
int ty_is_array(TyKind t);
TyKind ty_array_of(TyKind elem);       /* element type -> array kind */
TyKind ty_array_elem(TyKind arr);      /* array kind -> element type */
int ty_is_hash(TyKind t);
TyKind ty_hash_of(TyKind key, TyKind val); /* (key,val) -> hash kind (UNKNOWN if unsupported) */
TyKind ty_hash_key(TyKind h);
TyKind ty_hash_val(TyKind h);
const char *ty_hash_cname(TyKind h);   /* "StrInt" etc, for sp_<X>Hash_* */
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
