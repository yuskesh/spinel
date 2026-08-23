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
    case TY_MATCHDATA: return "matchdata";
    case TY_REGEX:     return "regex";
    case TY_EXCEPTION: return "exception";
    case TY_INT_ARRAY:   return "int_array";
    case TY_FLOAT_ARRAY: return "float_array";
    case TY_STR_ARRAY:   return "str_array";
    case TY_POLY_ARRAY:  return "poly_array";
    case TY_INT_ARRAY_ARRAY: return "int_array_array";
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
    case TY_THREAD:  return "thread";
    case TY_QUEUE:   return "queue";
    case TY_MUTEX:   return "mutex";
    case TY_CONDVAR: return "condvar";
    case TY_RANDOM:  return "random";
    case TY_DIR:     return "dir";
    case TY_ADDRINFO: return "addrinfo";
    case TY_SOCKOPT: return "sockopt";
    case TY_STR_RANGE: return "str_range";
    case TY_TMS:     return "tms";
    case TY_OPENSTRUCT: return "openstruct";
    case TY_METHOD:  return "method";
    case TY_IO:      return "io";
    case TY_ENUMERATOR: return "enumerator";
    case TY_CLASS:   return "class";
    case TY_POLY:    return "poly";
  }
  if (ty_is_obj_array(t)) return "obj_array";
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
         t == TY_STR_ARRAY || t == TY_POLY_ARRAY || t == TY_INT_ARRAY_ARRAY;
}
TyKind ty_array_of(TyKind elem) {
  switch (elem) {
    case TY_INT:    return TY_INT_ARRAY;
    case TY_FLOAT:  return TY_FLOAT_ARRAY;
    case TY_STRING: return TY_STR_ARRAY;
    /* An element type that is not yet known is not the same as an element
       type that is anything: while the fixpoint runs, keep the array at
       bottom so the slots it flows into wait for the real element type
       instead of locking onto the poly array. Producers reach this from
       every direction -- a literal whose elements are calls not yet typed,
       a `map` whose block return is not yet typed -- so the rule belongs
       here rather than at each of them. See g_infer_optimistic. */
    case TY_UNKNOWN: return g_infer_optimistic ? TY_UNKNOWN : TY_POLY_ARRAY;
    default:        return TY_POLY_ARRAY;
  }
}
TyKind ty_array_elem(TyKind arr) {
  switch (arr) {
    case TY_INT_ARRAY:       return TY_INT;
    case TY_FLOAT_ARRAY:     return TY_FLOAT;
    case TY_STR_ARRAY:       return TY_STRING;
    case TY_INT_ARRAY_ARRAY: return TY_INT_ARRAY;
    default:                 return TY_POLY;
  }
}
/* ty_array_of deliberately does NOT map TY_INT_ARRAY -> TY_INT_ARRAY_ARRAY:
   like TY_OBJ_ARRAY, the nested-int-array type is produced only by the
   post-fixpoint narrow pass, never by forward inference (a forward mapping
   would cascade the nested type through the fixpoint and destabilize it). */

void sp_llvm_unify_note(TyKind a, TyKind b, const char *site);   /* spinel-llvm L5 */
extern const char *sp_llvm_last_poly_site;   /* spinel-llvm L5 / E125 */
TyKind ty_unify_at(TyKind a, TyKind b, const char *site) {
  sp_llvm_last_poly_site = NULL;   /* E125: 毎回消してから */
  if (a == b) return a;
  if (a == TY_UNKNOWN) return b;
  if (b == TY_UNKNOWN) return a;
  /* int values flow into bigint slots losslessly (the emitters insert
     sp_bigint_new_int at the boundary), so a bigint-promoted local that
     also sees int writes stays bigint instead of widening to poly. */
  if ((a == TY_BIGINT && b == TY_INT) || (a == TY_INT && b == TY_BIGINT)) return TY_BIGINT;
  /* a plain string value flows into a mutable-string (shared handle) slot
     losslessly: the write re-wraps it in the sp_String (#3227 phase 3) */
  if ((a == TY_STRBUF && b == TY_STRING) || (a == TY_STRING && b == TY_STRBUF)) return TY_STRBUF;
  /* A heap object reference that also sees nil stays the object type: the
     object pointer's NULL encodes nil (legacy's `ClassName?`), so a nullable
     single-class reference need not widen to poly. */
  if (a == TY_NIL && ty_is_object(b)) return b;
  if (b == TY_NIL && ty_is_object(a)) return a;
  /* A poly array that also sees nil stays a (nullable) poly array: the
     sp_PolyArray* NULL encodes nil, and the poly-array method paths already
     NULL-guard, so a method returning `array | nil` need not widen to poly
     (which would strip every array method from the result). */
  if (a == TY_NIL && b == TY_POLY_ARRAY) return b;
  if (b == TY_NIL && a == TY_POLY_ARRAY) return a;
  /* An exception reference that also sees nil stays TY_EXCEPTION: the
     sp_Exception* NULL encodes nil ($! outside a rescue is already nil), and
     the exception method arms NULL-guard (#2739). */
  if (a == TY_NIL && b == TY_EXCEPTION) return b;
  if (b == TY_NIL && a == TY_EXCEPTION) return a;
  /* An IO handle that also sees nil stays TY_IO: sp_File* NULL is nil (the
     readiness family answers nil on timeout, and #nil? reads it), and widening
     to poly stripped every IO method from an `h = nil; h = <accept>` slot. */
  if (a == TY_NIL && b == TY_IO) return b;
  if (b == TY_NIL && a == TY_IO) return a;
  /* spinel-llvm L5 (E123): どの型どうしの合流が poly を作ったかを数える。
     未設定時は即 return するだけ。定義は analyze.c。 */
  sp_llvm_unify_note(a, b, site);
  sp_llvm_last_poly_site = site;   /* E125: 直後の代入が拾う */
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
  return sp_streq(n, "each") || sp_streq(n, "map") || sp_streq(n, "collect") ||
         sp_streq(n, "select") || sp_streq(n, "reject") || sp_streq(n, "filter") ||
         sp_streq(n, "find") || sp_streq(n, "detect") || sp_streq(n, "find_all") ||
         sp_streq(n, "sort_by") || sp_streq(n, "min_by") || sp_streq(n, "max_by") ||
         sp_streq(n, "count") || sp_streq(n, "sum") || sp_streq(n, "flat_map") ||
         sp_streq(n, "collect_concat") ||
         sp_streq(n, "filter_map") || sp_streq(n, "partition") || sp_streq(n, "group_by") ||
         sp_streq(n, "any?") || sp_streq(n, "all?") || sp_streq(n, "none?") ||
         sp_streq(n, "one?") || sp_streq(n, "take_while") || sp_streq(n, "drop_while") ||
         sp_streq(n, "reverse_each") || sp_streq(n, "each_entry") || sp_streq(n, "find_index");
}

TyIterShape ty_iter_shape(const char *name) {
  if (!name) return TY_ITER_NONE;
  if (sp_streq(name, "map") || sp_streq(name, "collect")) return TY_ITER_MAP;
  if (sp_streq(name, "select") || sp_streq(name, "filter")) return TY_ITER_SELECT;
  if (sp_streq(name, "reject")) return TY_ITER_REJECT;
  return TY_ITER_NONE;
}

int ty_block_yield(TyKind recv, const char *name, TyKind *out, int max) {
  if (!name || max < 1) return 0;
#define BY_PUT(i, t) do { if ((i) < max) out[i] = (t); } while (0)
  if (ty_is_array(recv)) {
    TyKind e = ty_array_elem(recv);
    if (ty_is_array_elem_iter(name)) { BY_PUT(0, e); return 1; }
    if (sp_streq(name, "each_with_index")) { BY_PUT(0, e); BY_PUT(1, TY_INT); return 2; }
    return 0;
  }
  if (ty_is_hash(recv)) {
    if (sp_streq(name, "each") || sp_streq(name, "each_pair")) {
      BY_PUT(0, ty_hash_key(recv)); BY_PUT(1, ty_hash_val(recv)); return 2;
    }
    if (sp_streq(name, "each_key")) { BY_PUT(0, ty_hash_key(recv)); return 1; }
    if (sp_streq(name, "each_value")) { BY_PUT(0, ty_hash_val(recv)); return 1; }
    return 0;
  }
  if (recv == TY_RANGE) {
    /* a range yields ints to its element iterators */
    if (sp_streq(name, "each_with_index")) { BY_PUT(0, TY_INT); BY_PUT(1, TY_INT); return 2; }
    if (ty_is_array_elem_iter(name)) { BY_PUT(0, TY_INT); return 1; }
    return 0;
  }
  if (recv == TY_INT) {
    if (sp_streq(name, "times") || sp_streq(name, "upto") || sp_streq(name, "downto")) {
      BY_PUT(0, TY_INT); return 1;
    }
    return 0;
  }
  return 0;
#undef BY_PUT
}
