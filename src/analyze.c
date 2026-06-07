#include "analyze.h"

#include <stdlib.h>
#include <string.h>

/* ---- operator classification ---- */

static int str_in(const char *s, const char *const *set) {
  if (!s) return 0;
  for (int i = 0; set[i]; i++) if (strcmp(s, set[i]) == 0) return 1;
  return 0;
}

static int is_arith_op(const char *op) {
  static const char *const set[] = {"+", "-", "*", "/", "%", "**", NULL};
  return str_in(op, set);
}
static int is_cmp_op(const char *op) {
  static const char *const set[] = {"<", ">", "<=", ">=", NULL};
  return str_in(op, set);
}
static int is_eq_op(const char *op) {
  static const char *const set[] = {"==", "!=", NULL};
  return str_in(op, set);
}
static int is_void_call(const char *name) {
  static const char *const set[] = {
    "puts", "print", "p", "pp", "require", "require_relative",
    "raise", "warn", "printf", NULL};
  return str_in(name, set);
}

/* ---- call inference ---- */

static TyKind infer_call(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);

  if (!name) return TY_UNKNOWN;

  TyKind rt = recv >= 0 ? infer_type(c, recv) : TY_UNKNOWN;
  TyKind a0 = argc >= 1 ? infer_type(c, argv[0]) : TY_UNKNOWN;

  /* unary minus / plus: `-x`, `+x` */
  if ((strcmp(name, "-@") == 0 || strcmp(name, "+@") == 0) && recv >= 0 && argc == 0)
    return ty_is_numeric(rt) ? rt : TY_UNKNOWN;
  if (strcmp(name, "!") == 0) return TY_BOOL;

  if (recv >= 0 && argc == 1 && is_arith_op(name)) {
    if (rt == TY_STRING) {
      if (strcmp(name, "+") == 0 || strcmp(name, "*") == 0) return TY_STRING;
      return TY_UNKNOWN;
    }
    if (ty_is_numeric(rt) && ty_is_numeric(a0))
      return (rt == TY_FLOAT || a0 == TY_FLOAT) ? TY_FLOAT : TY_INT;
    return TY_UNKNOWN;
  }
  if (recv >= 0 && argc == 1 && is_cmp_op(name)) {
    if (strcmp(name, "<=>") == 0) return TY_INT;
    return TY_BOOL;
  }
  if (argc == 1 && is_eq_op(name)) return TY_BOOL;

  /* predicate methods conventionally end in ? and return bool */
  size_t nl = strlen(name);
  if (nl > 0 && name[nl - 1] == '?') return TY_BOOL;

  /* common conversions / queries */
  if (strcmp(name, "to_s") == 0 || strcmp(name, "inspect") == 0 ||
      strcmp(name, "chr") == 0 || strcmp(name, "to_str") == 0) return TY_STRING;
  if (strcmp(name, "to_i") == 0 || strcmp(name, "to_int") == 0 ||
      strcmp(name, "length") == 0 || strcmp(name, "size") == 0 ||
      strcmp(name, "ord") == 0 || strcmp(name, "abs") == 0) return TY_INT;
  if (strcmp(name, "to_f") == 0) return TY_FLOAT;
  if (strcmp(name, "to_sym") == 0) return TY_SYMBOL;

  if (is_void_call(name) && recv < 0) return TY_VOID;

  return TY_UNKNOWN;
}

/* ---- core inference ---- */

static TyKind infer_uncached(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) return TY_UNKNOWN;

  if (strcmp(ty, "IntegerNode") == 0)        return TY_INT;
  if (strcmp(ty, "FloatNode") == 0)          return TY_FLOAT;
  if (strcmp(ty, "StringNode") == 0)         return TY_STRING;
  if (strcmp(ty, "InterpolatedStringNode") == 0) return TY_STRING;
  if (strcmp(ty, "SymbolNode") == 0)         return TY_SYMBOL;
  if (strcmp(ty, "TrueNode") == 0)           return TY_BOOL;
  if (strcmp(ty, "FalseNode") == 0)          return TY_BOOL;
  if (strcmp(ty, "NilNode") == 0)            return TY_NIL;
  if (strcmp(ty, "RangeNode") == 0)          return TY_RANGE;

  if (strcmp(ty, "LocalVariableReadNode") == 0) {
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? comp_local(c, nm) : NULL;
    return lv ? lv->type : TY_UNKNOWN;
  }
  if (strcmp(ty, "ParenthesesNode") == 0) {
    int body = nt_ref(nt, id, "body");
    if (body < 0) return TY_UNKNOWN;
    /* body is a StatementsNode; type is its last statement */
    int n = 0;
    const int *b = nt_arr(nt, body, "body", &n);
    return n > 0 ? infer_type(c, b[n - 1]) : TY_NIL;
  }
  if (strcmp(ty, "StatementsNode") == 0) {
    int n = 0;
    const int *b = nt_arr(nt, id, "body", &n);
    return n > 0 ? infer_type(c, b[n - 1]) : TY_NIL;
  }
  if (strcmp(ty, "IfNode") == 0 || strcmp(ty, "UnlessNode") == 0) {
    int then_b = nt_ref(nt, id, "statements");
    int else_b = nt_ref(nt, id, "subsequent");
    TyKind tt = then_b >= 0 ? infer_type(c, then_b) : TY_NIL;
    TyKind et = else_b >= 0 ? infer_type(c, else_b) : TY_NIL;
    return ty_unify(tt, et);
  }
  if (strcmp(ty, "ElseNode") == 0) {
    int s = nt_ref(nt, id, "statements");
    return s >= 0 ? infer_type(c, s) : TY_NIL;
  }
  if (strcmp(ty, "CallNode") == 0) return infer_call(c, id);

  return TY_UNKNOWN;
}

TyKind infer_type(Compiler *c, int id) {
  if (id < 0 || id >= c->nt->count) return TY_UNKNOWN;
  TyKind t = infer_uncached(c, id);
  c->ntype[id] = t;
  return t;
}

/* ---- local variable registration + fixpoint ---- */

static void register_locals(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (strcmp(ty, "LocalVariableWriteNode") == 0 ||
        strcmp(ty, "LocalVariableTargetNode") == 0 ||
        strcmp(ty, "LocalVariableReadNode") == 0 ||
        strcmp(ty, "LocalVariableOperatorWriteNode") == 0) {
      const char *nm = nt_str(nt, id, "name");
      if (nm) comp_local_intern(c, nm);
    }
  }
}

/* One pass over all writes; returns 1 if any local type changed. */
static int infer_write_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    const char *nm = NULL;
    TyKind newt = TY_UNKNOWN;
    if (strcmp(ty, "LocalVariableWriteNode") == 0) {
      nm = nt_str(nt, id, "name");
      int v = nt_ref(nt, id, "value");
      newt = infer_type(c, v);
    } else if (strcmp(ty, "LocalVariableOperatorWriteNode") == 0) {
      nm = nt_str(nt, id, "name");
      const char *op = nt_str(nt, id, "binary_operator");
      int v = nt_ref(nt, id, "value");
      LocalVar *cur = nm ? comp_local(c, nm) : NULL;
      TyKind vt = infer_type(c, v);
      TyKind ct = cur ? cur->type : TY_UNKNOWN;
      if (ct == TY_STRING) newt = TY_STRING;
      else if (ty_is_numeric(ct) && ty_is_numeric(vt))
        newt = (ct == TY_FLOAT || vt == TY_FLOAT) ? TY_FLOAT : TY_INT;
      else newt = ct;
      (void)op;
    } else {
      continue;
    }
    if (!nm) continue;
    LocalVar *lv = comp_local(c, nm);
    if (!lv) continue;
    TyKind merged = ty_unify(lv->type, newt);
    if (merged != lv->type) { lv->type = merged; changed = 1; }
  }
  return changed;
}

void analyze_program(Compiler *c) {
  register_locals(c);

  /* Fixpoint on local variable types (bounded to avoid runaway). */
  for (int iter = 0; iter < 64; iter++) {
    if (!infer_write_types(c)) break;
  }

  /* Mark gc-root needs and finalize the node type cache. */
  for (int i = 0; i < c->nlocals; i++)
    c->locals[i].gc_root = (c->locals[i].type == TY_STRING);

  for (int id = 0; id < c->nt->count; id++)
    infer_type(c, id);
}
