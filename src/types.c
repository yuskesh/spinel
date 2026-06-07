#include "types.h"

const char *ty_name(TyKind t) {
  switch (t) {
    case TY_UNKNOWN: return "unknown";
    case TY_VOID:    return "void";
    case TY_NIL:     return "nil";
    case TY_INT:     return "int";
    case TY_FLOAT:   return "float";
    case TY_STRING:  return "string";
    case TY_SYMBOL:  return "symbol";
    case TY_BOOL:    return "bool";
    case TY_RANGE:   return "range";
    case TY_POLY:    return "poly";
  }
  return "?";
}

int ty_is_numeric(TyKind t) { return t == TY_INT || t == TY_FLOAT; }

TyKind ty_unify(TyKind a, TyKind b) {
  if (a == b) return a;
  if (a == TY_UNKNOWN) return b;
  if (b == TY_UNKNOWN) return a;
  return TY_POLY;
}
