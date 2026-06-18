#include "types.h"
#include <stddef.h>
#include <string.h>

const char *ty_name(TyKind t) {
  switch (t) {
    case TY_UNKNOWN: return "unknown";
    case TY_VOID:    return "void";
    case TY_NIL:     return "nil";
    case TY_INT:     return "int";
    case TY_BIGINT:  return "bigint";
    case TY_FLOAT:   return "float";
    case TY_STRING:  return "string";
    case TY_STRBUF:  return "strbuf";
    case TY_SYMBOL:  return "symbol";
    case TY_BOOL:    return "bool";
    case TY_RANGE:   return "range";
    case TY_TIME:    return "time";
    case TY_COMPLEX: return "complex";
    case TY_RATIONAL: return "rational";
    case TY_STRINGIO: return "stringio";
    case TY_STRINGSCANNER: return "stringscanner";
    case TY_MATCHDATA: return "matchdata";
    case TY_REGEX:     return "regex";
    case TY_EXCEPTION: return "exception";
    case TY_INT_ARRAY:   return "int_array";
    case TY_FLOAT_ARRAY: return "float_array";
    case TY_STR_ARRAY:   return "str_array";
    case TY_POLY_ARRAY:  return "poly_array";
    case TY_STR_INT_HASH: return "str_int_hash";
    case TY_STR_STR_HASH: return "str_str_hash";
    case TY_INT_INT_HASH: return "int_int_hash";
    case TY_INT_STR_HASH: return "int_str_hash";
    case TY_SYM_POLY_HASH:  return "sym_poly_hash";
    case TY_STR_POLY_HASH:  return "str_poly_hash";
    case TY_POLY_POLY_HASH: return "poly_poly_hash";
    case TY_PROC:    return "proc";
    case TY_CURRY:   return "curry";
    case TY_FIBER:   return "fiber";
    case TY_RANDOM:  return "random";
    case TY_METHOD:  return "method";
    case TY_IO:      return "io";
    case TY_CLASS:   return "class";
    case TY_POLY:    return "poly";
  }
  return "?";
}

static const struct { TyKind kind, key, val; const char *cname; } hash_tbl[] = {
  {TY_STR_INT_HASH, TY_STRING, TY_INT,    "StrInt"},
  {TY_STR_STR_HASH, TY_STRING, TY_STRING, "StrStr"},
  {TY_INT_INT_HASH, TY_INT,    TY_INT,    "IntInt"},
  {TY_INT_STR_HASH, TY_INT,    TY_STRING, "IntStr"},
  {TY_SYM_POLY_HASH,  TY_SYMBOL, TY_POLY, "SymPoly"},
  {TY_STR_POLY_HASH,  TY_STRING, TY_POLY, "StrPoly"},
  {TY_POLY_POLY_HASH, TY_POLY,   TY_POLY, "PolyPoly"},
};

int ty_is_hash(TyKind t) {
  for (unsigned i = 0; i < sizeof hash_tbl / sizeof hash_tbl[0]; i++)
    if (hash_tbl[i].kind == t) return 1;
  return 0;
}
TyKind ty_hash_of(TyKind key, TyKind val) {
  for (unsigned i = 0; i < sizeof hash_tbl / sizeof hash_tbl[0]; i++)
    if (hash_tbl[i].key == key && hash_tbl[i].val == val) return hash_tbl[i].kind;
  return TY_UNKNOWN;
}
TyKind ty_hash_key(TyKind h) {
  for (unsigned i = 0; i < sizeof hash_tbl / sizeof hash_tbl[0]; i++)
    if (hash_tbl[i].kind == h) return hash_tbl[i].key;
  return TY_UNKNOWN;
}
TyKind ty_hash_val(TyKind h) {
  for (unsigned i = 0; i < sizeof hash_tbl / sizeof hash_tbl[0]; i++)
    if (hash_tbl[i].kind == h) return hash_tbl[i].val;
  return TY_UNKNOWN;
}
const char *ty_hash_cname(TyKind h) {
  for (unsigned i = 0; i < sizeof hash_tbl / sizeof hash_tbl[0]; i++)
    if (hash_tbl[i].kind == h) return hash_tbl[i].cname;
  return NULL;
}

int ty_is_numeric(TyKind t) { return t == TY_INT || t == TY_BIGINT || t == TY_FLOAT; }
int ty_is_array(TyKind t) {
  return t == TY_INT_ARRAY || t == TY_FLOAT_ARRAY ||
         t == TY_STR_ARRAY || t == TY_POLY_ARRAY;
}
TyKind ty_array_of(TyKind elem) {
  switch (elem) {
    case TY_INT:    return TY_INT_ARRAY;
    case TY_FLOAT:  return TY_FLOAT_ARRAY;
    case TY_STRING: return TY_STR_ARRAY;
    default:        return TY_POLY_ARRAY;
  }
}
TyKind ty_array_elem(TyKind arr) {
  switch (arr) {
    case TY_INT_ARRAY:   return TY_INT;
    case TY_FLOAT_ARRAY: return TY_FLOAT;
    case TY_STR_ARRAY:   return TY_STRING;
    default:             return TY_POLY;
  }
}

TyKind ty_unify(TyKind a, TyKind b) {
  if (a == b) return a;
  if (a == TY_UNKNOWN) return b;
  if (b == TY_UNKNOWN) return a;
  /* int values flow into bigint slots losslessly (the emitters insert
     sp_bigint_new_int at the boundary), so a bigint-promoted local that
     also sees int writes stays bigint instead of widening to poly. */
  if ((a == TY_BIGINT && b == TY_INT) || (a == TY_INT && b == TY_BIGINT)) return TY_BIGINT;
  /* A heap object reference that also sees nil stays the object type: the
     object pointer's NULL encodes nil (legacy's `ClassName?`), so a nullable
     single-class reference need not widen to poly. */
  if (a == TY_NIL && ty_is_object(b)) return b;
  if (b == TY_NIL && ty_is_object(a)) return a;
  return TY_POLY;
}

/* Numeric accumulator promotion: a fold accumulator seeded with one numeric
   type but reassigned to another (e.g. `[1.5].reduce(0){|a,x| a+x}` -- an int
   seed folded over floats) takes the wider numeric type (float > bigint > int)
   rather than widening to poly. Non-numeric mixes fall back to ty_unify. */
TyKind ty_promote_numeric(TyKind a, TyKind b) {
  if (a == b) return a;
  if (a == TY_UNKNOWN) return b;
  if (b == TY_UNKNOWN) return a;
  if (ty_is_numeric(a) && ty_is_numeric(b)) {
    if (a == TY_FLOAT || b == TY_FLOAT) return TY_FLOAT;
    return TY_BIGINT;  /* int + bigint */
  }
  return ty_unify(a, b);
}

/* The single-element-arg array iterators: a block bound to one of these
   receives exactly one param = the array element. Enumerated once here so the
   knowledge is not re-encoded as scattered method-name lists. */
static int ty_is_array_elem_iter(const char *n) {
  return !strcmp(n, "each") || !strcmp(n, "map") || !strcmp(n, "collect") ||
         !strcmp(n, "select") || !strcmp(n, "reject") || !strcmp(n, "filter") ||
         !strcmp(n, "find") || !strcmp(n, "detect") || !strcmp(n, "find_all") ||
         !strcmp(n, "sort_by") || !strcmp(n, "min_by") || !strcmp(n, "max_by") ||
         !strcmp(n, "count") || !strcmp(n, "sum") || !strcmp(n, "flat_map") ||
         !strcmp(n, "filter_map") || !strcmp(n, "partition") || !strcmp(n, "group_by") ||
         !strcmp(n, "any?") || !strcmp(n, "all?") || !strcmp(n, "none?") ||
         !strcmp(n, "one?") || !strcmp(n, "take_while") || !strcmp(n, "drop_while") ||
         !strcmp(n, "reverse_each") || !strcmp(n, "each_entry") || !strcmp(n, "find_index");
}

TyIterShape ty_iter_shape(const char *name) {
  if (!name) return TY_ITER_NONE;
  if (!strcmp(name, "map") || !strcmp(name, "collect")) return TY_ITER_MAP;
  if (!strcmp(name, "select") || !strcmp(name, "filter")) return TY_ITER_SELECT;
  if (!strcmp(name, "reject")) return TY_ITER_REJECT;
  return TY_ITER_NONE;
}

int ty_block_yield(TyKind recv, const char *name, TyKind *out, int max) {
  if (!name || max < 1) return 0;
#define BY_PUT(i, t) do { if ((i) < max) out[i] = (t); } while (0)
  if (ty_is_array(recv)) {
    TyKind e = ty_array_elem(recv);
    if (ty_is_array_elem_iter(name)) { BY_PUT(0, e); return 1; }
    if (!strcmp(name, "each_with_index")) { BY_PUT(0, e); BY_PUT(1, TY_INT); return 2; }
    return 0;
  }
  if (ty_is_hash(recv)) {
    if (!strcmp(name, "each") || !strcmp(name, "each_pair")) {
      BY_PUT(0, ty_hash_key(recv)); BY_PUT(1, ty_hash_val(recv)); return 2;
    }
    if (!strcmp(name, "each_key")) { BY_PUT(0, ty_hash_key(recv)); return 1; }
    if (!strcmp(name, "each_value")) { BY_PUT(0, ty_hash_val(recv)); return 1; }
    return 0;
  }
  if (recv == TY_RANGE) {
    /* a range yields ints to its element iterators */
    if (!strcmp(name, "each_with_index")) { BY_PUT(0, TY_INT); BY_PUT(1, TY_INT); return 2; }
    if (ty_is_array_elem_iter(name)) { BY_PUT(0, TY_INT); return 1; }
    return 0;
  }
  if (recv == TY_INT) {
    if (!strcmp(name, "times") || !strcmp(name, "upto") || !strcmp(name, "downto")) {
      BY_PUT(0, TY_INT); return 1;
    }
    return 0;
  }
  return 0;
#undef BY_PUT
}
