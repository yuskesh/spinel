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
  TY_POLY          /* union / top: a value whose static type widened */
} TyKind;

const char *ty_name(TyKind t);         /* legacy string tag, for diagnostics */
int ty_is_numeric(TyKind t);           /* INT or FLOAT */
/* Merge two observed types into the narrowest type covering both.
   Equal -> same; UNKNOWN acts as identity; otherwise widen to POLY
   (numeric int+float stays POLY for now -- mixed-numeric vars are rare
   and handled when they appear). */
TyKind ty_unify(TyKind a, TyKind b);

#endif
