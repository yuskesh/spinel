#include "analyze.h"

#include <stdio.h>
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

  /* identity methods: return the receiver unchanged */
  if (recv >= 0 && argc == 0 &&
      (!strcmp(name, "freeze") || !strcmp(name, "itself") ||
       !strcmp(name, "dup") || !strcmp(name, "clone")))
    return rt;

  /* x.class -> a class-name string (for known builtin/object receivers) */
  if (recv >= 0 && argc == 0 && !strcmp(name, "class") &&
      (ty_is_numeric(rt) || rt == TY_STRING || rt == TY_SYMBOL || rt == TY_BOOL ||
       rt == TY_RANGE || rt == TY_TIME || rt == TY_NIL ||
       ty_is_array(rt) || ty_is_hash(rt) || ty_is_object(rt)))
    return TY_STRING;

  /* Class.new(...) -> an instance of that class; built-in .new constructors */
  if (recv >= 0 && !strcmp(name, "new")) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ConstantReadNode")) {
      const char *cn = nt_str(nt, recv, "name");
      int ci = comp_class_index(c, cn);
      if (ci >= 0) return ty_object(ci);
      if (cn && !strcmp(cn, "Array") && argc == 2) return ty_array_of(infer_type(c, argv[1]));
      if (cn && !strcmp(cn, "String")) return TY_STRING;
    }
  }

  /* Time.now / at / local / mktime / utc / gm -> a Time value */
  if (recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Time") &&
        (!strcmp(name, "now") || !strcmp(name, "at") || !strcmp(name, "local") ||
         !strcmp(name, "mktime") || !strcmp(name, "utc") || !strcmp(name, "gm")))
      return TY_TIME;
  }

  /* Time instance methods */
  if (recv >= 0 && rt == TY_TIME) {
    if (!strcmp(name, "utc") || !strcmp(name, "gmtime") || !strcmp(name, "getutc") ||
        !strcmp(name, "localtime") || !strcmp(name, "getlocal") || !strcmp(name, "+") ||
        !strcmp(name, "-")) return TY_TIME;
    if (!strcmp(name, "to_s") || !strcmp(name, "inspect") || !strcmp(name, "strftime") ||
        !strcmp(name, "iso8601") || !strcmp(name, "zone") || !strcmp(name, "asctime") ||
        !strcmp(name, "ctime")) return TY_STRING;
    if (!strcmp(name, "to_f") || !strcmp(name, "subsec")) return TY_FLOAT;
    if (!strcmp(name, "utc?") || !strcmp(name, "gmt?") || !strcmp(name, "dst?") ||
        !strcmp(name, "sunday?") || !strcmp(name, "monday?")) return TY_BOOL;
    if (!strcmp(name, "class")) return TY_STRING;
    /* year/mon/day/hour/min/sec/wday/yday/to_i/tv_sec/tv_usec/usec/tv_nsec/nsec/... */
    return TY_INT;
  }

  /* Class.cmethod(...) -> the class method's return type */
  if (recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ConstantReadNode")) {
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      if (ci >= 0) {
        int mi = comp_cmethod_in_chain(c, ci, name, NULL);
        if (mi >= 0) return c->scopes[mi].ret;
      }
    }
  }

  /* obj.method(...) -> the method's return type (walks the superclass chain) */
  if (recv >= 0 && ty_is_object(rt)) {
    int cid = ty_object_class(rt);
    ClassInfo *cls = &c->classes[cid];
    /* attr reader */
    if (comp_reader_in_chain(c, cid, name, NULL)) {
      char ivn[256];
      snprintf(ivn, sizeof ivn, "@%s", name);
      int iv = comp_ivar_index(cls, ivn);
      if (iv >= 0) return cls->ivar_types[iv];
    }
    /* attr writer: obj.x= returns the assigned value */
    size_t ln = strlen(name);
    if (ln >= 2 && name[ln - 1] == '=') {
      char base[256];
      if (ln - 1 < sizeof base) {
        memcpy(base, name, ln - 1); base[ln - 1] = '\0';
        if (comp_writer_in_chain(c, cid, base, NULL) && argc >= 1) return infer_type(c, argv[0]);
      }
    }
    int mi = comp_method_in_chain(c, cid, name, NULL);
    if (mi >= 0) return c->scopes[mi].ret;
    if (!strcmp(name, "to_s") || !strcmp(name, "inspect")) return TY_STRING;
  }

  /* implicit-self call inside an instance method */
  if (recv < 0) {
    Scope *self = comp_scope_of(c, id);
    if (self->class_id >= 0) {
      if (comp_reader_in_chain(c, self->class_id, name, NULL)) {
        char ivn[256];
        snprintf(ivn, sizeof ivn, "@%s", name);
        int iv = comp_ivar_index(&c->classes[self->class_id], ivn);
        if (iv >= 0) return c->classes[self->class_id].ivar_types[iv];
      }
      int mi = comp_method_in_chain(c, self->class_id, name, NULL);
      if (mi >= 0) return c->scopes[mi].ret;
    }
  }

  /* user-defined free-function call (no receiver) */
  if (recv < 0) {
    int mi = comp_method_index(c, name);
    if (mi >= 0) return c->scopes[mi].ret;
    /* Kernel conversions */
    if (!strcmp(name, "Integer") && argc == 1) return TY_INT;
    if (!strcmp(name, "Float") && argc == 1) return TY_FLOAT;
  }

  /* array receiver methods */
  if (recv >= 0 && ty_is_array(rt)) {
    int block = nt_ref(nt, id, "block");
    if (block >= 0) {
      if (!strcmp(name, "map") || !strcmp(name, "collect")) {
        int body = nt_ref(nt, block, "body");
        int bn = 0;
        const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        return ty_array_of(bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN);
      }
      if (!strcmp(name, "select") || !strcmp(name, "reject") ||
          !strcmp(name, "filter") || !strcmp(name, "sort_by") ||
          !strcmp(name, "take_while") || !strcmp(name, "drop_while"))
        return rt;
    }
    if (!strcmp(name, "[]"))                          return ty_array_elem(rt);
    /* index returns nil on a miss -> poly (int-or-nil) */
    if (!strcmp(name, "index") && (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY)) return TY_POLY;
    if (!strcmp(name, "length") || !strcmp(name, "size") ||
        !strcmp(name, "count") || !strcmp(name, "index")) return TY_INT;
    if (!strcmp(name, "sum"))                         return ty_array_elem(rt);
    if (!strcmp(name, "first") || !strcmp(name, "last") ||
        !strcmp(name, "min") || !strcmp(name, "max")) return ty_array_elem(rt);
    if (!strcmp(name, "join"))                        return TY_STRING;
    if (!strcmp(name, "inspect") || !strcmp(name, "to_s")) return TY_STRING;
    if (!strcmp(name, "empty?") || !strcmp(name, "include?")) return TY_BOOL;
    if (!strcmp(name, "push") || !strcmp(name, "<<") || !strcmp(name, "append") ||
        !strcmp(name, "reverse") || !strcmp(name, "sort") || !strcmp(name, "uniq") ||
        !strcmp(name, "to_a") || !strcmp(name, "dup") || !strcmp(name, "clone") ||
        !strcmp(name, "compact"))   return rt;
    if (!strcmp(name, "[]="))                         return ty_array_elem(rt);
  }

  /* exception receiver methods */
  if (recv >= 0 && rt == TY_EXCEPTION) {
    if (!strcmp(name, "message") || !strcmp(name, "to_s") ||
        !strcmp(name, "to_str") || !strcmp(name, "inspect") ||
        !strcmp(name, "full_message") || !strcmp(name, "class")) return TY_STRING;
  }

  /* poly receiver / poly operand: result type of operations on sp_RbVal */
  if (recv >= 0 && (rt == TY_POLY || a0 == TY_POLY)) {
    if (!strcmp(name, "+") || !strcmp(name, "-") || !strcmp(name, "*") ||
        !strcmp(name, "/") || !strcmp(name, "%") || !strcmp(name, "**"))
      return TY_POLY;
    if (!strcmp(name, "<") || !strcmp(name, ">") || !strcmp(name, "<=") ||
        !strcmp(name, ">=") || !strcmp(name, "==") || !strcmp(name, "!=") ||
        !strcmp(name, "nil?") || !strcmp(name, "is_a?") || !strcmp(name, "kind_of?") ||
        !strcmp(name, "include?"))
      return TY_BOOL;
    if (rt == TY_POLY) {
      if (!strcmp(name, "to_s") || !strcmp(name, "inspect")) return TY_STRING;
      if (!strcmp(name, "to_i")) return TY_INT;
      if (!strcmp(name, "to_f")) return TY_FLOAT;
      /* poly method dispatch: unify the return type over every class that
         defines `name` (the runtime cls_id picks the impl). */
      TyKind r = TY_UNKNOWN; int found = 0;
      for (int k = 0; k < c->nclasses; k++) {
        int mi = comp_method_in_chain(c, k, name, NULL);
        if (mi >= 0) { r = found ? ty_unify(r, c->scopes[mi].ret) : c->scopes[mi].ret; found = 1; continue; }
        int rdcls = -1;
        if (comp_reader_in_chain(c, k, name, &rdcls)) {
          char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", name);
          int iv = comp_ivar_index(&c->classes[rdcls], ivn);
          TyKind rt2 = iv >= 0 ? c->classes[rdcls].ivar_types[iv] : TY_UNKNOWN;
          r = found ? ty_unify(r, rt2) : rt2; found = 1;
        }
      }
      if (found) return r;
    }
  }

  /* symbol receiver methods */
  if (recv >= 0 && rt == TY_SYMBOL) {
    if (!strcmp(name, "to_s") || !strcmp(name, "id2name") || !strcmp(name, "name")) return TY_STRING;
    if (!strcmp(name, "inspect")) return TY_STRING;
    if (!strcmp(name, "upcase") || !strcmp(name, "downcase") ||
        !strcmp(name, "capitalize") || !strcmp(name, "swapcase") ||
        !strcmp(name, "to_sym") || !strcmp(name, "itself")) return TY_SYMBOL;
    if (!strcmp(name, "length") || !strcmp(name, "size")) return TY_INT;
    if (!strcmp(name, "empty?") || !strcmp(name, "==") || !strcmp(name, "!=")) return TY_BOOL;
  }

  /* range receiver methods */
  if (recv >= 0 && rt == TY_RANGE) {
    if (!strcmp(name, "to_a"))      return TY_INT_ARRAY;
    if (!strcmp(name, "include?") || !strcmp(name, "member?") ||
        !strcmp(name, "cover?"))    return TY_BOOL;
    if (!strcmp(name, "sum") || !strcmp(name, "min") || !strcmp(name, "max") ||
        !strcmp(name, "first") || !strcmp(name, "last") ||
        !strcmp(name, "size") || !strcmp(name, "count") ||
        !strcmp(name, "begin") || !strcmp(name, "end"))  return TY_INT;
    int block = nt_ref(nt, id, "block");
    if (block >= 0 && (!strcmp(name, "map") || !strcmp(name, "collect"))) {
      int body = nt_ref(nt, block, "body");
      int bn = 0;
      const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      return ty_array_of(bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN);
    }
  }

  /* hash receiver methods */
  if (recv >= 0 && ty_is_hash(rt)) {
    if (!strcmp(name, "[]"))     return ty_hash_val(rt);
    if (!strcmp(name, "[]="))    return ty_hash_val(rt);
    if (!strcmp(name, "fetch"))  return ty_hash_val(rt);
    if (!strcmp(name, "length") || !strcmp(name, "size") ||
        !strcmp(name, "count")) return TY_INT;
    if (!strcmp(name, "keys"))   return ty_array_of(ty_hash_key(rt));
    if (!strcmp(name, "values")) return ty_array_of(ty_hash_val(rt));
    if (!strcmp(name, "has_key?") || !strcmp(name, "key?") ||
        !strcmp(name, "include?") || !strcmp(name, "member?") ||
        !strcmp(name, "has_value?") || !strcmp(name, "value?") ||
        !strcmp(name, "empty?")) return TY_BOOL;
  }

  /* string receiver methods */
  if (recv >= 0 && rt == TY_STRING) {
    if (!strcmp(name, "upcase") || !strcmp(name, "downcase") ||
        !strcmp(name, "capitalize") || !strcmp(name, "reverse") ||
        !strcmp(name, "strip") || !strcmp(name, "lstrip") ||
        !strcmp(name, "rstrip") || !strcmp(name, "chomp") ||
        !strcmp(name, "chop") || !strcmp(name, "chr")) return TY_STRING;
    if (!strcmp(name, "[]"))    return TY_STRING;
    if (!strcmp(name, "index") || !strcmp(name, "to_i")) return TY_INT;
    if (!strcmp(name, "to_f"))  return TY_FLOAT;
    if (!strcmp(name, "split") || !strcmp(name, "lines")) return TY_STR_ARRAY;
    if (!strcmp(name, "bytes")) return TY_INT_ARRAY;
    if (!strcmp(name, "gsub") || !strcmp(name, "sub") || !strcmp(name, "tr") ||
        !strcmp(name, "center") || !strcmp(name, "ljust") || !strcmp(name, "rjust"))
      return TY_STRING;
    if (!strcmp(name, "*")) return TY_STRING;
  }
  /* integer receiver methods */
  if (recv >= 0 && rt == TY_INT) {
    if (!strcmp(name, "chr")) return TY_STRING;
    if (!strcmp(name, "[]") && argc == 1) return TY_INT;  /* bit access */
    if (!strcmp(name, "gcd") || !strcmp(name, "lcm") || !strcmp(name, "clamp")) return TY_INT;
    if (!strcmp(name, "digits")) return TY_INT_ARRAY;
    if (!strcmp(name, "to_s") && argc == 1) return TY_STRING;
  }
  /* float receiver methods */
  if (recv >= 0 && rt == TY_FLOAT) {
    if (!strcmp(name, "floor") || !strcmp(name, "ceil") ||
        !strcmp(name, "round") || !strcmp(name, "truncate")) {
      /* value-based return type: ndigits > 0 (literal) -> Float, else Integer */
      if (argc == 1) {
        const char *aty = nt_type(nt, argv[0]);
        if (aty && !strcmp(aty, "IntegerNode") && nt_int(nt, argv[0], "value", 0) > 0)
          return TY_FLOAT;
      }
      return TY_INT;
    }
  }

  if ((!strcmp(name, "-@") || !strcmp(name, "+@")) && recv >= 0 && argc == 0)
    return ty_is_numeric(rt) ? rt : TY_UNKNOWN;
  if (!strcmp(name, "!")) return TY_BOOL;

  if (recv >= 0 && argc == 1 && is_arith_op(name)) {
    if (rt == TY_STRING) {
      if (!strcmp(name, "+") || !strcmp(name, "*")) return TY_STRING;
      return TY_UNKNOWN;
    }
    if (ty_is_numeric(rt) && ty_is_numeric(a0))
      return (rt == TY_FLOAT || a0 == TY_FLOAT) ? TY_FLOAT : TY_INT;
    return TY_UNKNOWN;
  }
  if (recv >= 0 && argc == 1 && is_cmp_op(name)) {
    if (!strcmp(name, "<=>")) return TY_INT;
    return TY_BOOL;
  }
  if (argc == 1 && is_eq_op(name)) return TY_BOOL;

  /* integer bitwise operators */
  if (recv >= 0 && argc == 1 && rt == TY_INT &&
      (!strcmp(name, "&") || !strcmp(name, "|") || !strcmp(name, "^") ||
       !strcmp(name, "<<") || !strcmp(name, ">>")))
    return TY_INT;
  /* boolean &/|/^ */
  if (recv >= 0 && argc == 1 && rt == TY_BOOL &&
      (!strcmp(name, "&") || !strcmp(name, "|") || !strcmp(name, "^")))
    return TY_BOOL;

  size_t nl = strlen(name);
  if (nl > 0 && name[nl - 1] == '?') return TY_BOOL;

  if (!strcmp(name, "to_s") || !strcmp(name, "inspect") ||
      !strcmp(name, "chr") || !strcmp(name, "to_str")) return TY_STRING;
  if (!strcmp(name, "to_i") || !strcmp(name, "to_int") ||
      !strcmp(name, "length") || !strcmp(name, "size") ||
      !strcmp(name, "ord") || !strcmp(name, "abs")) return TY_INT;
  if (!strcmp(name, "to_f")) return TY_FLOAT;
  if (!strcmp(name, "to_sym")) return TY_SYMBOL;

  if (is_void_call(name) && recv < 0) return TY_VOID;

  return TY_UNKNOWN;
}

/* ---- core inference ---- */

static TyKind infer_uncached(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) return TY_UNKNOWN;

  if (!strcmp(ty, "IntegerNode"))             return TY_INT;
  if (!strcmp(ty, "FloatNode"))               return TY_FLOAT;
  if (!strcmp(ty, "StringNode"))              return TY_STRING;
  if (!strcmp(ty, "InterpolatedStringNode"))  return TY_STRING;
  if (!strcmp(ty, "InterpolatedSymbolNode"))  return TY_SYMBOL;
  if (!strcmp(ty, "SymbolNode"))              return TY_SYMBOL;
  if (!strcmp(ty, "TrueNode"))                return TY_BOOL;
  if (!strcmp(ty, "FalseNode"))               return TY_BOOL;
  if (!strcmp(ty, "NilNode"))                 return TY_NIL;
  if (!strcmp(ty, "RangeNode"))               return TY_RANGE;

  if (!strcmp(ty, "LocalVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    LocalVar *lv = nm ? scope_local(s, nm) : NULL;
    return lv ? lv->type : TY_UNKNOWN;
  }
  if (!strcmp(ty, "GlobalVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? comp_gvar(c, nm + 1) : NULL;
    return lv ? lv->type : TY_UNKNOWN;
  }
  if (!strcmp(ty, "ConstantReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? comp_const(c, nm) : NULL;
    return lv ? lv->type : TY_UNKNOWN;
  }
  if (!strcmp(ty, "SelfNode")) {
    Scope *s = comp_scope_of(c, id);
    return s->class_id >= 0 ? ty_object(s->class_id) : TY_UNKNOWN;
  }
  if (!strcmp(ty, "InstanceVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    if (s->class_id < 0) return TY_UNKNOWN;
    ClassInfo *ci = &c->classes[s->class_id];
    int iv = nm ? comp_ivar_index(ci, nm) : -1;
    return iv >= 0 ? ci->ivar_types[iv] : TY_UNKNOWN;
  }
  if (!strcmp(ty, "ParenthesesNode")) {
    int body = nt_ref(nt, id, "body");
    if (body < 0) return TY_NIL;
    int n = 0;
    const int *b = nt_arr(nt, body, "body", &n);
    return n > 0 ? infer_type(c, b[n - 1]) : TY_NIL;
  }
  if (!strcmp(ty, "StatementsNode")) {
    int n = 0;
    const int *b = nt_arr(nt, id, "body", &n);
    return n > 0 ? infer_type(c, b[n - 1]) : TY_NIL;
  }
  if (!strcmp(ty, "IfNode") || !strcmp(ty, "UnlessNode")) {
    int then_b = nt_ref(nt, id, "statements");
    int else_b = nt_ref(nt, id, "subsequent");
    TyKind tt = then_b >= 0 ? infer_type(c, then_b) : TY_NIL;
    TyKind et = else_b >= 0 ? infer_type(c, else_b) : TY_NIL;
    return ty_unify(tt, et);
  }
  if (!strcmp(ty, "ElseNode")) {
    int s = nt_ref(nt, id, "statements");
    return s >= 0 ? infer_type(c, s) : TY_NIL;
  }
  if (!strcmp(ty, "ArrayNode")) {
    int n = 0;
    const int *els = nt_arr(nt, id, "elements", &n);
    if (n == 0) return TY_UNKNOWN;  /* empty: element type comes from usage */
    TyKind e = TY_UNKNOWN;
    for (int k = 0; k < n; k++) e = ty_unify(e, infer_type(c, els[k]));
    return ty_array_of(e);
  }
  if (!strcmp(ty, "HashNode")) {
    int n = 0;
    const int *els = nt_arr(nt, id, "elements", &n);
    if (n == 0) return TY_UNKNOWN;
    TyKind kt = TY_UNKNOWN, vt = TY_UNKNOWN;
    for (int k = 0; k < n; k++) {
      const char *aty = nt_type(nt, els[k]);
      if (!aty || strcmp(aty, "AssocNode")) return TY_UNKNOWN;
      kt = ty_unify(kt, infer_type(c, nt_ref(nt, els[k], "key")));
      vt = ty_unify(vt, infer_type(c, nt_ref(nt, els[k], "value")));
    }
    /* symbol keys -> SymPolyHash (boxed values), regardless of value type */
    if (kt == TY_SYMBOL) return TY_SYM_POLY_HASH;
    TyKind hv = ty_hash_of(kt, vt);
    /* string keys with a mixed/unsupported value type -> StrPolyHash */
    if (hv == TY_UNKNOWN && kt == TY_STRING && vt != TY_UNKNOWN) return TY_STR_POLY_HASH;
    return hv;
  }
  if (!strcmp(ty, "YieldNode")) {
    /* value of yield = the block body's value at a call site of this method */
    int mi = (int)(comp_scope_of(c, id) - c->scopes);
    for (int cid = 0; cid < nt->count; cid++) {
      const char *cty = nt_type(nt, cid);
      if (!cty || strcmp(cty, "CallNode")) continue;
      int blk = nt_ref(nt, cid, "block");
      if (blk < 0) continue;
      const char *cn = nt_str(nt, cid, "name");
      int crecv = nt_ref(nt, cid, "receiver");
      int rmi = -1;
      if (crecv < 0) {
        rmi = comp_method_index(c, cn);
        if (rmi < 0) {
          Scope *cs = comp_scope_of(c, cid);
          if (cs->class_id >= 0) rmi = comp_method_in_chain(c, cs->class_id, cn, NULL);
        }
      }
      else {
        TyKind crt = infer_type(c, crecv);
        if (ty_is_object(crt)) rmi = comp_method_in_chain(c, ty_object_class(crt), cn, NULL);
      }
      if (rmi != mi) continue;
      int bb = nt_ref(nt, blk, "body");
      int bn = 0;
      const int *bd = bb >= 0 ? nt_arr(nt, bb, "body", &bn) : NULL;
      return bn > 0 ? infer_type(c, bd[bn - 1]) : TY_NIL;
    }
    return TY_UNKNOWN;
  }
  if (!strcmp(ty, "SuperNode") || !strcmp(ty, "ForwardingSuperNode")) {
    Scope *s = comp_scope_of(c, id);
    if (s->class_id < 0 || !s->name) return TY_UNKNOWN;
    int p = c->classes[s->class_id].parent;
    if (p < 0) return TY_UNKNOWN;
    int mi = comp_method_in_chain(c, p, s->name, NULL);
    return mi >= 0 ? c->scopes[mi].ret : TY_UNKNOWN;
  }
  if (!strcmp(ty, "AndNode") || !strcmp(ty, "OrNode")) {
    TyKind lt = infer_type(c, nt_ref(nt, id, "left"));
    TyKind rt = infer_type(c, nt_ref(nt, id, "right"));
    if (lt == TY_BOOL && rt == TY_BOOL) return TY_BOOL;
    return ty_unify(lt, rt);  /* value form: a || b -> common type */
  }
  if (!strcmp(ty, "BeginNode")) {
    /* value = body value unified with each rescue handler's value */
    int body = nt_ref(nt, id, "statements");
    TyKind r = body >= 0 ? infer_type(c, body) : TY_NIL;
    for (int rs = nt_ref(nt, id, "rescue_clause"); rs >= 0; rs = nt_ref(nt, rs, "subsequent")) {
      int st = nt_ref(nt, rs, "statements");
      r = ty_unify(r, st >= 0 ? infer_type(c, st) : TY_NIL);
    }
    return r;
  }
  if (!strcmp(ty, "CallNode")) return infer_call(c, id);

  return TY_UNKNOWN;
}

TyKind infer_type(Compiler *c, int id) {
  if (id < 0 || id >= c->nt->count) return TY_UNKNOWN;
  TyKind t = infer_uncached(c, id);
  c->ntype[id] = t;
  return t;
}

/* ---- scope assignment ---- */

static void scope_add_param(Scope *s, const char *name, int defnode) {
  if (s->nparams % 8 == 0) {
    s->pnames = realloc(s->pnames, sizeof(char *) * (size_t)(s->nparams + 8));
    s->pdefault = realloc(s->pdefault, sizeof(int) * (size_t)(s->nparams + 8));
  }
  s->pdefault[s->nparams] = defnode;
  s->pnames[s->nparams++] = strdup(name);
  if (defnode < 0) s->nrequired = s->nparams;
  LocalVar *lv = scope_local_intern(s, name);
  lv->is_param = 1;
}

static void walk_scope(Compiler *c, int id, int scope_idx, int class_id) {
  if (id < 0 || id >= c->nt->count) return;
  c->nscope[id] = scope_idx;
  const char *ty = nt_type(c->nt, id);
  int child = scope_idx;
  int child_class = class_id;

  if (ty && !strcmp(ty, "ClassNode")) {
    int cp = nt_ref(c->nt, id, "constant_path");
    const char *cname = cp >= 0 ? nt_str(c->nt, cp, "name") : NULL;
    if (cname && comp_class_index(c, cname) < 0) {
      comp_class_new(c, cname, id);
      child_class = c->nclasses - 1;
    }
    else if (cname) {
      child_class = comp_class_index(c, cname);  /* reopened class */
    }
  }
  else if (ty && !strcmp(ty, "DefNode")) {
    const char *name = nt_str(c->nt, id, "name");
    Scope *s = comp_scope_new(c, name, id);
    int new_idx = c->nscopes - 1;
    s->body = nt_ref(c->nt, id, "body");
    s->class_id = class_id;   /* instance method of the enclosing class */
    /* `def self.foo` / `def Klass.foo`: a class (singleton) method. */
    if (nt_ref(c->nt, id, "receiver") >= 0) s->is_cmethod = 1;
    int pn = nt_ref(c->nt, id, "parameters");
    if (pn >= 0) {
      int rn = 0;
      const int *reqs = nt_arr(c->nt, pn, "requireds", &rn);
      for (int i = 0; i < rn; i++) {
        const char *pname = nt_str(c->nt, reqs[i], "name");
        if (pname) scope_add_param(s, pname, -1);
      }
      int on = 0;
      const int *opts = nt_arr(c->nt, pn, "optionals", &on);
      for (int i = 0; i < on; i++) {
        const char *pname = nt_str(c->nt, opts[i], "name");
        int dv = nt_ref(c->nt, opts[i], "value");
        if (pname) scope_add_param(s, pname, dv);
      }
      /* `&block` parameter: tracked for yield-style inlining, no typed slot.
         Anonymous `&` has no name; record "" so forwarding still works. */
      int bp = nt_ref(c->nt, pn, "block");
      if (bp >= 0 && nt_type(c->nt, bp) && !strcmp(nt_type(c->nt, bp), "BlockParameterNode")) {
        const char *bn = nt_str(c->nt, bp, "name");
        s->blk_param = strdup(bn ? bn : "");
      }
    }
    child = new_idx;
  }

  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) {
    int r = nt_ref_at(c->nt, id, i);
    if (r >= 0) walk_scope(c, r, child, child_class);
  }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(c->nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (ids[j] >= 0) walk_scope(c, ids[j], child, child_class);
  }
}

static void register_locals(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (!strcmp(ty, "LocalVariableWriteNode") ||
        !strcmp(ty, "LocalVariableTargetNode") ||
        !strcmp(ty, "LocalVariableReadNode") ||
        !strcmp(ty, "LocalVariableOperatorWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (nm) scope_local_intern(comp_scope_of(c, id), nm);
    }
    if (!strcmp(ty, "InstanceVariableWriteNode") ||
        !strcmp(ty, "InstanceVariableReadNode") ||
        !strcmp(ty, "InstanceVariableOperatorWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      Scope *s = comp_scope_of(c, id);
      if (nm && s->class_id >= 0) comp_ivar_intern(&c->classes[s->class_id], nm);
    }
  }
}

/* `Const = Struct.new(:a, :b)` / `Const = Data.define(:a, :b)` defines a
   class named Const whose positional members are attr_accessors. Register
   it as a class with one ivar + reader + writer per member. */
static int is_c_ident(const char *s);
static void register_structs(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "ConstantWriteNode")) continue;
    const char *cname = nt_str(nt, id, "name");
    int val = nt_ref(nt, id, "value");
    if (!cname || val < 0 || !is_c_ident(cname)) continue;
    if (!nt_type(nt, val) || strcmp(nt_type(nt, val), "CallNode")) continue;
    const char *mn = nt_str(nt, val, "name");
    int vr = nt_ref(nt, val, "receiver");
    const char *rn = vr >= 0 && nt_type(nt, vr) && !strcmp(nt_type(nt, vr), "ConstantReadNode")
                     ? nt_str(nt, vr, "name") : NULL;
    int is_struct = rn && ((!strcmp(rn, "Struct") && mn && !strcmp(mn, "new")) ||
                           (!strcmp(rn, "Data") && mn && !strcmp(mn, "define")));
    if (!is_struct) continue;
    if (comp_class_index(c, cname) >= 0) continue;
    ClassInfo *cls = comp_class_new(c, cname, id);
    cls->is_struct = 1;
    int args = nt_ref(nt, val, "arguments");
    int an = 0;
    const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    for (int a = 0; a < an; a++) {
      if (!nt_type(nt, argv[a]) || strcmp(nt_type(nt, argv[a]), "SymbolNode")) continue;
      const char *m = nt_str(nt, argv[a], "value");
      if (!m) continue;
      char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", m);
      comp_ivar_intern(cls, ivn);
      comp_add_reader(cls, m);
      comp_add_writer(cls, m);
    }
  }
}

/* Collect attr_reader/attr_writer/attr_accessor declarations in class
   bodies, registering backing ivars + reader/writer method names. */
static void register_attrs(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cls = &c->classes[ci];
    int body = nt_ref(nt, cls->def_node, "body");
    int n = 0;
    const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    for (int k = 0; k < n; k++) {
      int s = stmts[k];
      const char *sty = nt_type(nt, s);
      if (!sty || strcmp(sty, "CallNode")) continue;
      const char *nm = nt_str(nt, s, "name");
      if (!nm) continue;
      int reader = !strcmp(nm, "attr_reader") || !strcmp(nm, "attr_accessor");
      int writer = !strcmp(nm, "attr_writer") || !strcmp(nm, "attr_accessor");
      if (!reader && !writer) continue;
      int args = nt_ref(nt, s, "arguments");
      int an = 0;
      const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      for (int a = 0; a < an; a++) {
        const char *aty = nt_type(nt, argv[a]);
        if (!aty || strcmp(aty, "SymbolNode")) continue;
        const char *base = nt_str(nt, argv[a], "value");
        if (!base) continue;
        char ivname[256];
        snprintf(ivname, sizeof ivname, "@%s", base);
        comp_ivar_intern(cls, ivname);
        if (reader) comp_add_reader(cls, base);
        if (writer) comp_add_writer(cls, base);
      }
    }
  }
}

/* Collect `alias new old` (AliasMethodNode) and `alias_method :new, :old`
   (CallNode) statements in class bodies into the class alias table. */
static void register_aliases(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cls = &c->classes[ci];
    int body = nt_ref(nt, cls->def_node, "body");
    int n = 0;
    const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    for (int k = 0; k < n; k++) {
      int s = stmts[k];
      const char *sty = nt_type(nt, s);
      if (!sty) continue;
      if (!strcmp(sty, "AliasMethodNode")) {
        int nn = nt_ref(nt, s, "new_name");
        int on = nt_ref(nt, s, "old_name");
        const char *nw = nn >= 0 ? nt_str(nt, nn, "value") : NULL;
        const char *od = on >= 0 ? nt_str(nt, on, "value") : NULL;
        comp_add_alias(cls, nw, od);
      }
      else if (!strcmp(sty, "CallNode")) {
        const char *nm = nt_str(nt, s, "name");
        if (!nm || strcmp(nm, "alias_method")) continue;
        int args = nt_ref(nt, s, "arguments");
        int an = 0;
        const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
        if (an >= 2 && nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "SymbolNode") &&
            nt_type(nt, argv[1]) && !strcmp(nt_type(nt, argv[1]), "SymbolNode"))
          comp_add_alias(cls, nt_str(nt, argv[0], "value"), nt_str(nt, argv[1], "value"));
      }
    }
  }
}

static int is_c_ident(const char *s) {
  if (!s || !*s) return 0;
  for (const char *p = s; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '_')) return 0;
  return 1;
}

/* Register global variables ($g) and top-level constants (FOO). */
static void register_globals_consts(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (!strcmp(ty, "GlobalVariableWriteNode") || !strcmp(ty, "GlobalVariableReadNode") ||
        !strcmp(ty, "GlobalVariableOperatorWriteNode") || !strcmp(ty, "GlobalVariableTargetNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (nm && nm[0] == '$' && is_c_ident(nm + 1)) comp_gvar_intern(c, nm + 1);
    }
    else if (!strcmp(ty, "ConstantWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      /* a Struct/Data const names a class, not a value constant */
      if (nm && is_c_ident(nm) && comp_class_index(c, nm) < 0) comp_const_intern(c, nm);
    }
  }
}

static int infer_global_const_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    LocalVar *lv = NULL;
    TyKind vt = TY_UNKNOWN;
    if (!strcmp(ty, "GlobalVariableWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (nm) lv = comp_gvar(c, nm + 1);
      vt = infer_type(c, nt_ref(nt, id, "value"));
      if (vt == TY_NIL) continue;
    }
    else if (!strcmp(ty, "GlobalVariableOperatorWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (nm) lv = comp_gvar(c, nm + 1);
      TyKind cur = lv ? lv->type : TY_UNKNOWN;
      TyKind v = infer_type(c, nt_ref(nt, id, "value"));
      if (cur == TY_STRING) vt = TY_STRING;
      else if (ty_is_numeric(cur) && ty_is_numeric(v)) vt = (cur == TY_FLOAT || v == TY_FLOAT) ? TY_FLOAT : TY_INT;
      else vt = cur;
    }
    else if (!strcmp(ty, "ConstantWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (nm) lv = comp_const(c, nm);
      vt = infer_type(c, nt_ref(nt, id, "value"));
    }
    else {
      continue;
    }
    if (!lv) continue;
    TyKind merged = ty_unify(lv->type, vt);
    if (merged != lv->type) { lv->type = merged; changed = 1; }
  }
  return changed;
}

/* Resolve each class's superclass index from its ClassNode. */
static void resolve_parents(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int i = 0; i < c->nclasses; i++) {
    int sc = nt_ref(nt, c->classes[i].def_node, "superclass");
    if (sc < 0) continue;
    const char *sty = nt_type(nt, sc);
    if (sty && !strcmp(sty, "ConstantReadNode")) {
      int p = comp_class_index(c, nt_str(nt, sc, "name"));
      if (p >= 0 && p != i) c->classes[i].parent = p;
    }
  }
}

/* Merge inherited ivar/reader/writer NAMES into subclasses so the struct
   layout is [parent ivars..., own ivars...] (cast-compatible). Types are
   propagated later in the fixpoint. Parent-first order. */
static void inherit_members(Compiler *c) {
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    int p = ci->parent;
    if (p < 0 || p >= i) continue;  /* parent defined earlier; already merged */
    ClassInfo *pc = &c->classes[p];

    char **old = ci->ivars; TyKind *oldt = ci->ivar_types; int oldn = ci->nivars;
    ci->ivars = NULL; ci->ivar_types = NULL; ci->nivars = ci->civars = 0;
    for (int k = 0; k < pc->nivars; k++) {
      int idx = comp_ivar_intern(ci, pc->ivars[k]);
      ci->ivar_types[idx] = pc->ivar_types[k];
    }
    for (int k = 0; k < oldn; k++) {
      int idx = comp_ivar_intern(ci, old[k]);
      ci->ivar_types[idx] = ty_unify(ci->ivar_types[idx], oldt[k]);
      free(old[k]);
    }
    free(old); free(oldt);

    for (int k = 0; k < pc->nreaders; k++) comp_add_reader(ci, pc->readers[k]);
    for (int k = 0; k < pc->nwriters; k++) comp_add_writer(ci, pc->writers[k]);
  }
}

/* Propagate inherited @ivar types parent -> child. */
static int infer_inherited_ivars(Compiler *c) {
  int changed = 0;
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (ci->parent < 0) continue;
    ClassInfo *pc = &c->classes[ci->parent];
    for (int k = 0; k < pc->nivars; k++) {
      int idx = comp_ivar_index(ci, pc->ivars[k]);
      if (idx < 0) continue;
      TyKind merged = ty_unify(ci->ivar_types[idx], pc->ivar_types[k]);
      if (merged != ci->ivar_types[idx]) { ci->ivar_types[idx] = merged; changed = 1; }
    }
  }
  return changed;
}

/* @ivar types from their assignments across the class's methods. */
static int infer_ivar_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (!strcmp(ty, "InstanceVariableWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      TyKind vt = infer_type(c, nt_ref(nt, id, "value"));
      if (vt == TY_NIL) continue;  /* nil write doesn't pin the ivar type */
      Scope *s = comp_scope_of(c, id);
      if (!nm || s->class_id < 0) continue;
      ClassInfo *ci = &c->classes[s->class_id];
      int iv = comp_ivar_index(ci, nm);
      if (iv < 0) continue;
      TyKind merged = ty_unify(ci->ivar_types[iv], vt);
      if (merged != ci->ivar_types[iv]) { ci->ivar_types[iv] = merged; changed = 1; }
    }
    else if (!strcmp(ty, "CallNode")) {
      /* attr-writer assignment: obj.x = v  (CallNode "x=") */
      const char *nm = nt_str(nt, id, "name");
      int recv = nt_ref(nt, id, "receiver");
      size_t ln = nm ? strlen(nm) : 0;
      if (!nm || recv < 0 || ln < 2 || nm[ln - 1] != '=') continue;
      TyKind rt = infer_type(c, recv);
      if (!ty_is_object(rt)) continue;
      ClassInfo *ci = &c->classes[ty_object_class(rt)];
      char base[256];
      if (ln - 1 >= sizeof base) continue;
      memcpy(base, nm, ln - 1); base[ln - 1] = '\0';
      if (!comp_is_writer(ci, base)) continue;
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an < 1) continue;
      char ivname[256];
      snprintf(ivname, sizeof ivname, "@%s", base);
      int iv = comp_ivar_index(ci, ivname);
      if (iv < 0) continue;
      TyKind vt = infer_type(c, argv[0]);
      TyKind merged = ty_unify(ci->ivar_types[iv], vt);
      if (merged != ci->ivar_types[iv]) { ci->ivar_types[iv] = merged; changed = 1; }
    }
  }
  return changed;
}

/* ---- fixpoint passes ---- */

static int infer_write_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;

  /* Recompute non-param local types FRESH each iteration: reset to UNKNOWN
     (saving the old value), then unify all write-site RHS types. This lets
     a local NARROW as block-param/return inference improves, instead of
     monotonically widening to POLY from a stale early estimate. */
  for (int s = 0; s < c->nscopes; s++)
    for (int i = 0; i < c->scopes[s].nlocals; i++) {
      LocalVar *lv = &c->scopes[s].locals[i];
      /* stash old type in gc_root (unused by codegen) so we can detect
         change; block params are typed elsewhere, so leave them alone */
      if (!lv->is_param && !lv->is_block_param) { lv->gc_root = (int)lv->type; lv->type = TY_UNKNOWN; }
    }

  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    const char *nm = NULL;
    TyKind newt = TY_UNKNOWN;
    if (!strcmp(ty, "LocalVariableWriteNode")) {
      nm = nt_str(nt, id, "name");
      newt = infer_type(c, nt_ref(nt, id, "value"));
      /* a `x = nil` write doesn't pin the type: nil is the absent/default
         value, so the variable takes its non-nil assignments' type */
      if (newt == TY_NIL) newt = TY_POLY;
    }
    else if (!strcmp(ty, "LocalVariableOperatorWriteNode")) {
      nm = nt_str(nt, id, "name");
      Scope *s = comp_scope_of(c, id);
      LocalVar *cur = nm ? scope_local(s, nm) : NULL;
      TyKind vt = infer_type(c, nt_ref(nt, id, "value"));
      TyKind ct = cur ? (TyKind)cur->gc_root : TY_UNKNOWN; /* old type */
      if (ct == TY_STRING) newt = TY_STRING;
      else if (ty_is_numeric(ct) && ty_is_numeric(vt))
        newt = (ct == TY_FLOAT || vt == TY_FLOAT) ? TY_FLOAT : TY_INT;
      else newt = ct;
    }
    else if (!strcmp(ty, "LocalVariableOrWriteNode") ||
             !strcmp(ty, "LocalVariableAndWriteNode")) {
      /* a ||= v / a &&= v : the variable can hold its prior value or v */
      nm = nt_str(nt, id, "name");
      Scope *s = comp_scope_of(c, id);
      LocalVar *cur = nm ? scope_local(s, nm) : NULL;
      TyKind ct = cur ? (TyKind)cur->gc_root : TY_UNKNOWN;
      newt = ty_unify(ct, infer_type(c, nt_ref(nt, id, "value")));
    }
    else {
      continue;
    }
    if (!nm) continue;
    LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
    if (!lv || lv->is_param || lv->is_block_param) continue;
    lv->type = ty_unify(lv->type, newt);
  }

  /* Multiple assignment `a, b = e0, e1`: each target gets its element's
     type (the RHS ArrayNode is a tuple here, not an array value). */
  for (int id = 0; id < nt->count; id++) {
    if (strcmp(nt_type(nt, id) ? nt_type(nt, id) : "", "MultiWriteNode")) continue;
    int ln = 0;
    const int *lefts = nt_arr(nt, id, "lefts", &ln);
    int value = nt_ref(nt, id, "value");
    const char *vty = nt_type(nt, value);
    if (!vty || strcmp(vty, "ArrayNode")) continue;
    int en = 0;
    const int *els = nt_arr(nt, value, "elements", &en);
    for (int i = 0; i < ln && i < en; i++) {
      if (strcmp(nt_type(nt, lefts[i]) ? nt_type(nt, lefts[i]) : "", "LocalVariableTargetNode")) continue;
      const char *lnm = nt_str(nt, lefts[i], "name");
      TyKind et = infer_type(c, els[i]);
      if (et == TY_NIL) continue;
      LocalVar *lv = lnm ? scope_local(comp_scope_of(c, id), lnm) : NULL;
      if (!lv || lv->is_param || lv->is_block_param) continue;
      lv->type = ty_unify(lv->type, et);
    }
  }

  /* Fold container usage into the local type so an empty `[]` / `{}` gets
     its element / key+value type from how it is filled. `a << x` /
     `a.push(x)` / `a[i] = x` (int key) -> array; `h[k] = v` / `h[k] op= v`
     (string key) -> hash. Part of the recompute frame so it survives reset. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    int recv, kt = TY_UNKNOWN, vt = TY_UNKNOWN, is_push = 0;
    if (!strcmp(ty, "CallNode")) {
      recv = nt_ref(nt, id, "receiver");
      const char *name = nt_str(nt, id, "name");
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (name && (!strcmp(name, "push") || !strcmp(name, "<<")) && an == 1) {
        is_push = 1; vt = infer_type(c, argv[0]);
      }
      else if (name && !strcmp(name, "[]=") && an == 2) {
        kt = infer_type(c, argv[0]); vt = infer_type(c, argv[1]);
      }
      else continue;
    }
    else if (!strcmp(ty, "IndexOperatorWriteNode")) {
      recv = nt_ref(nt, id, "receiver");
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an != 1) continue;
      kt = infer_type(c, argv[0]); vt = infer_type(c, nt_ref(nt, id, "value"));
    }
    else {
      continue;
    }
    if (recv < 0 || strcmp(nt_type(nt, recv) ? nt_type(nt, recv) : "", "LocalVariableReadNode")) continue;
    const char *rnm = nt_str(nt, recv, "name");
    LocalVar *lv = rnm ? scope_local(comp_scope_of(c, recv), rnm) : NULL;
    if (!lv || lv->is_param || lv->is_block_param) continue;

    if (is_push || kt == TY_INT) {
      /* array (a known string's `<<` is append, not push -- don't pollute) */
      if (vt == TY_UNKNOWN) continue;
      if (lv->type != TY_UNKNOWN && !ty_is_array(lv->type)) continue;
      lv->type = ty_unify(lv->type, ty_array_of(vt));
    }
    else if (kt == TY_STRING) {
      if (vt == TY_UNKNOWN) continue;
      TyKind hv = ty_hash_of(TY_STRING, vt);
      if (hv == TY_UNKNOWN) hv = TY_STR_POLY_HASH;  /* mixed values */
      if (lv->type != TY_UNKNOWN && !ty_is_hash(lv->type)) continue;
      /* a str-keyed hash that has seen >1 value type widens to StrPoly */
      if (lv->type != TY_UNKNOWN && lv->type != hv &&
          (lv->type == TY_STR_INT_HASH || lv->type == TY_STR_STR_HASH || lv->type == TY_STR_POLY_HASH))
        hv = TY_STR_POLY_HASH;
      lv->type = hv;
    }
    else if (kt == TY_SYMBOL) {
      /* symbol key -> SymPolyHash (boxed values) */
      if (vt == TY_UNKNOWN) continue;
      if (lv->type != TY_UNKNOWN && lv->type != TY_SYM_POLY_HASH) continue;
      lv->type = TY_SYM_POLY_HASH;
    }
  }

  /* detect change vs the stashed old types */
  for (int s = 0; s < c->nscopes; s++)
    for (int i = 0; i < c->scopes[s].nlocals; i++) {
      LocalVar *lv = &c->scopes[s].locals[i];
      if (!lv->is_param && !lv->is_block_param && (TyKind)lv->gc_root != lv->type) changed = 1;
    }
  return changed;
}

/* Unify a call's argument types into method scope `mi`'s parameters. */
static int bind_call_params(Compiler *c, int call_id, int mi) {
  if (mi < 0) return 0;
  const NodeTable *nt = c->nt;
  Scope *m = &c->scopes[mi];
  int args = nt_ref(nt, call_id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  int n = argc < m->nparams ? argc : m->nparams;
  int changed = 0;
  for (int k = 0; k < n; k++) {
    TyKind at = infer_type(c, argv[k]);
    LocalVar *p = scope_local(m, m->pnames[k]);
    if (!p) continue;
    TyKind merged = ty_unify(p->type, at);
    if (merged != p->type) { p->type = merged; changed = 1; }
  }
  return changed;
}

/* Optional parameters get a type from their default value too. */
static int infer_default_param_types(Compiler *c) {
  int changed = 0;
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    for (int i = 0; i < sc->nparams; i++) {
      if (sc->pdefault[i] < 0) continue;
      TyKind dt = infer_type(c, sc->pdefault[i]);
      if (dt == TY_NIL) continue;  /* nil default doesn't pin the type */
      LocalVar *p = scope_local(sc, sc->pnames[i]);
      if (!p) continue;
      TyKind merged = ty_unify(p->type, dt);
      if (merged != p->type) { p->type = merged; changed = 1; }
    }
  }
  return changed;
}

static int infer_param_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (!strcmp(ty, "SuperNode")) {
      Scope *s = comp_scope_of(c, id);
      if (s->class_id < 0 || !s->name) continue;
      int p = c->classes[s->class_id].parent;
      if (p < 0) continue;
      changed |= bind_call_params(c, id, comp_method_in_chain(c, p, s->name, NULL));
      continue;
    }
    if (strcmp(ty, "CallNode")) continue;
    const char *name = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");

    if (recv < 0) {
      int mi = comp_method_index(c, name);
      if (mi < 0) {
        Scope *self = comp_scope_of(c, id);
        if (self->class_id >= 0) mi = comp_method_in_chain(c, self->class_id, name, NULL);
      }
      changed |= bind_call_params(c, id, mi);
      continue;
    }
    /* Class.new -> initialize params; Class.cmethod -> cmethod params */
    {
      const char *rty = nt_type(nt, recv);
      if (rty && !strcmp(rty, "ConstantReadNode")) {
        int ci = comp_class_index(c, nt_str(nt, recv, "name"));
        if (ci >= 0) {
          if (!strcmp(name, "new") && c->classes[ci].is_struct) {
            /* Struct construction: positional args set member ivars in order. */
            ClassInfo *cls = &c->classes[ci];
            int args = nt_ref(nt, id, "arguments");
            int an = 0;
            const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
            for (int a = 0; a < an && a < cls->nivars; a++) {
              TyKind at = infer_type(c, argv[a]);
              TyKind m = ty_unify(cls->ivar_types[a], at);
              if (m != cls->ivar_types[a]) { cls->ivar_types[a] = m; changed = 1; }
            }
            continue;
          }
          if (!strcmp(name, "new"))
            changed |= bind_call_params(c, id, comp_method_in_chain(c, ci, "initialize", NULL));
          else
            changed |= bind_call_params(c, id, comp_cmethod_in_chain(c, ci, name, NULL));
          continue;
        }
      }
      if (!strcmp(name, "new")) continue;
    }
    /* obj.method -> instance method params */
    TyKind rt = infer_type(c, recv);
    if (ty_is_object(rt))
      changed |= bind_call_params(c, id, comp_method_in_chain(c, ty_object_class(rt), name, NULL));
  }
  return changed;
}

/* `for x in coll` binds x to the collection's element type (int for a
   range, the array element type for an array). */
static int infer_for_index(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "ForNode")) continue;
    int idx = nt_ref(nt, id, "index");
    int coll = nt_ref(nt, id, "collection");
    if (idx < 0 || coll < 0) continue;
    const char *vn = nt_str(nt, idx, "name");
    if (!vn) continue;
    TyKind ct = infer_type(c, coll);
    TyKind et = ct == TY_RANGE ? TY_INT : ty_is_array(ct) ? ty_array_elem(ct) : TY_UNKNOWN;
    if (et == TY_UNKNOWN) continue;
    LocalVar *lv = scope_local_intern(comp_scope_of(c, idx), vn);
    lv->is_block_param = 1;  /* iteration-bound: survives the write-types reset */
    if (lv->type != et) { lv->type = et; changed = 1; }
  }
  return changed;
}

/* Name of a block's idx-th required parameter, or NULL. */
const char *block_param_name(Compiler *c, int block, int idx) {
  int bp = nt_ref(c->nt, block, "parameters");      /* BlockParametersNode */
  if (bp < 0) return NULL;
  int pn = nt_ref(c->nt, bp, "parameters");          /* ParametersNode */
  if (pn < 0) return NULL;
  int n = 0;
  const int *reqs = nt_arr(c->nt, pn, "requireds", &n);
  if (idx < n) return nt_str(c->nt, reqs[idx], "name");
  return NULL;
}

/* First YieldNode belonging to scope `si`, or -1. */
static int first_yield(Compiler *c, int si) {
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (ty && !strcmp(ty, "YieldNode") && c->nscope[id] == si) return id;
  }
  return -1;
}

/* Arguments node of the first `<&block-param>.call(...)` in scope `si`, or
   -1. Lets block-param inference treat block.call like a yield. */
static int first_block_call_args(Compiler *c, int si) {
  Scope *m = &c->scopes[si];
  if (!m->blk_param || !m->blk_param[0]) return -1;
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (!ty || strcmp(ty, "CallNode") || c->nscope[id] != si) continue;
    const char *nm = nt_str(c->nt, id, "name");
    if (!nm || strcmp(nm, "call")) continue;
    int recv = nt_ref(c->nt, id, "receiver");
    if (recv < 0 || !nt_type(c->nt, recv) || strcmp(nt_type(c->nt, recv), "LocalVariableReadNode")) continue;
    const char *rn = nt_str(c->nt, recv, "name");
    if (rn && !strcmp(rn, m->blk_param)) return nt_ref(c->nt, id, "arguments");
  }
  return -1;
}

/* Bind block parameter types for supported iteration methods. */
static int infer_block_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "CallNode")) continue;
    int block = nt_ref(nt, id, "block");
    if (block < 0) continue;
    const char *name = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");
    if (!name) continue;

    /* call to a user yielding method: block params take the yield arg types */
    {
      int mi = -1;
      if (recv < 0) {
        mi = comp_method_index(c, name);
        if (mi < 0) {
          Scope *self = comp_scope_of(c, id);
          if (self->class_id >= 0) mi = comp_method_in_chain(c, self->class_id, name, NULL);
        }
      }
      else {
        TyKind rt0 = infer_type(c, recv);
        if (ty_is_object(rt0)) mi = comp_method_in_chain(c, ty_object_class(rt0), name, NULL);
      }
      if (mi >= 0 && c->scopes[mi].yields) {
        int yn = first_yield(c, mi);
        int ya = yn >= 0 ? nt_ref(nt, yn, "arguments") : first_block_call_args(c, mi);
        int yc = 0;
        const int *yargs = ya >= 0 ? nt_arr(nt, ya, "arguments", &yc) : NULL;
        Scope *bs = comp_scope_of(c, block);
        for (int k = 0; k < yc; k++) {
          const char *bp = block_param_name(c, block, k);
          if (!bp) continue;
          LocalVar *lv = scope_local_intern(bs, bp); lv->is_block_param = 1;
          TyKind m = ty_unify(lv->type, infer_type(c, yargs[k]));
          if (m != lv->type) { lv->type = m; changed = 1; }
        }
        continue;
      }
    }

    if (recv < 0) continue;
    TyKind rt = infer_type(c, recv);
    const char *p0 = block_param_name(c, block, 0);
    if (!p0) continue;

    TyKind pt = TY_UNKNOWN;
    if ((!strcmp(name, "times") || !strcmp(name, "upto") ||
         !strcmp(name, "downto") || !strcmp(name, "step")) && rt == TY_INT)
      pt = TY_INT;
    else if (!strcmp(name, "each") && rt == TY_RANGE)
      pt = TY_INT;
    else if ((!strcmp(name, "each") || !strcmp(name, "map") ||
              !strcmp(name, "select") || !strcmp(name, "reject") ||
              !strcmp(name, "find") || !strcmp(name, "each_with_index")) &&
             ty_is_array(rt))
      pt = ty_array_elem(rt);

    /* array.each_with_index { |x, i| } binds element + int index */
    if (!strcmp(name, "each_with_index") && ty_is_array(rt)) {
      Scope *es = comp_scope_of(c, block);
      LocalVar *ep = scope_local_intern(es, p0); ep->is_block_param = 1;
      TyKind em = ty_unify(ep->type, ty_array_elem(rt));
      if (em != ep->type) { ep->type = em; changed = 1; }
      const char *p1 = block_param_name(c, block, 1);
      if (p1) {
        LocalVar *ip = scope_local_intern(es, p1); ip->is_block_param = 1;
        TyKind im = ty_unify(ip->type, TY_INT);
        if (im != ip->type) { ip->type = im; changed = 1; }
      }
      continue;
    }

    /* hash.each / each_pair { |k, v| } binds two params */
    if ((!strcmp(name, "each") || !strcmp(name, "each_pair")) && ty_is_hash(rt)) {
      Scope *hs = comp_scope_of(c, block);
      LocalVar *kp = scope_local_intern(hs, p0); kp->is_block_param = 1;
      TyKind km = ty_unify(kp->type, ty_hash_key(rt));
      if (km != kp->type) { kp->type = km; changed = 1; }
      const char *p1 = block_param_name(c, block, 1);
      if (p1) {
        LocalVar *vp = scope_local_intern(hs, p1); vp->is_block_param = 1;
        TyKind vm = ty_unify(vp->type, ty_hash_val(rt));
        if (vm != vp->type) { vp->type = vm; changed = 1; }
      }
      continue;
    }

    if (pt == TY_UNKNOWN) continue;
    Scope *s = comp_scope_of(c, block);
    LocalVar *lv = scope_local_intern(s, p0); lv->is_block_param = 1;
    TyKind merged = ty_unify(lv->type, pt);
    if (merged != lv->type) { lv->type = merged; changed = 1; }
  }
  return changed;
}

/* Value type of an explicit `return expr` (or nil for bare return). */
static TyKind return_node_type(Compiler *c, int id) {
  int args = nt_ref(c->nt, id, "arguments");
  if (args < 0) return TY_NIL;
  int n = 0;
  const int *a = nt_arr(c->nt, args, "arguments", &n);
  return n > 0 ? infer_type(c, a[0]) : TY_NIL;
}

static int infer_return_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  /* implicit return: the body's value */
  for (int s = 1; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    /* An empty method body returns nil; if its value is used at all it must
       be poly (a void C function yields nothing to read). */
    int empty_body = sc->body < 0;
    if (sc->body >= 0 && nt_type(nt, sc->body) && !strcmp(nt_type(nt, sc->body), "StatementsNode")) {
      int bn = 0; nt_arr(nt, sc->body, "body", &bn); if (bn == 0) empty_body = 1;
    }
    TyKind r = empty_body ? TY_POLY : infer_type(c, sc->body);
    /* explicit returns within this scope */
    for (int id = 0; id < nt->count; id++) {
      const char *ty = nt_type(nt, id);
      if (ty && !strcmp(ty, "ReturnNode") && comp_scope_of(c, id) == sc)
        r = ty_unify(r, return_node_type(c, id));
    }
    if (r != sc->ret) { sc->ret = r; changed = 1; }
  }
  return changed;
}

/* Mark each method scope reachable iff its name is referenced anywhere as
   a call name, alias target, or symbol literal. Scope 0 (top level) and
   every `initialize` are always reachable. */
static void compute_reachable(Compiler *c) {
  const NodeTable *nt = c->nt;
  /* Build the set of referenced names. */
  char **names = NULL; int n = 0, cap = 0;
  #define ADD_NAME(NM) do { const char *_nm = (NM); \
    if (_nm) { int _f = 0; for (int _i = 0; _i < n; _i++) if (!strcmp(names[_i], _nm)) { _f = 1; break; } \
    if (!_f) { if (n >= cap) { cap = cap ? cap*2 : 32; names = realloc(names, sizeof(char*)*(size_t)cap); } names[n++] = strdup(_nm); } } } while (0)

  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (!strcmp(ty, "CallNode")) ADD_NAME(nt_str(nt, id, "name"));
    else if (!strcmp(ty, "SymbolNode")) ADD_NAME(nt_str(nt, id, "value"));
  }
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cls = &c->classes[ci];
    for (int i = 0; i < cls->naliases; i++) { ADD_NAME(cls->alias_new[i]); ADD_NAME(cls->alias_old[i]); }
  }

  /* Names that may be invoked implicitly (no explicit CallNode): keep live. */
  static const char *const implicit[] = {
    "to_s", "inspect", "==", "<=>", "eql?", "hash", "each", "coerce",
    "to_str", "to_ary", "to_a", "to_i", "to_int", "to_h", "to_proc", "call", NULL };

  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (s == 0 || !sc->name || !strcmp(sc->name, "initialize")) { sc->reachable = 1; continue; }
    sc->reachable = 0;
    for (int i = 0; implicit[i]; i++) if (!strcmp(implicit[i], sc->name)) { sc->reachable = 1; break; }
    if (sc->reachable) continue;
    for (int i = 0; i < n; i++) if (!strcmp(names[i], sc->name)) { sc->reachable = 1; break; }
  }
  for (int i = 0; i < n; i++) free(names[i]);
  free(names);
  #undef ADD_NAME
}

void analyze_program(Compiler *c) {
  /* scope 0 = top level */
  Scope *top = comp_scope_new(c, NULL, -1);
  top->body = nt_ref(c->nt, c->nt->root_id, "statements");

  walk_scope(c, c->nt->root_id, 0, -1);
  register_structs(c);
  register_locals(c);
  register_attrs(c);
  register_aliases(c);
  register_globals_consts(c);

  /* rescue variables (`rescue => e`) are typed as exception objects */
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (!ty || strcmp(ty, "RescueNode")) continue;
    int ref = nt_ref(c->nt, id, "reference");
    if (ref < 0 || strcmp(nt_type(c->nt, ref) ? nt_type(c->nt, ref) : "", "LocalVariableTargetNode")) continue;
    const char *nm = nt_str(c->nt, ref, "name");
    if (!nm) continue;
    LocalVar *lv = scope_local_intern(comp_scope_of(c, ref), nm);
    lv->type = TY_EXCEPTION;
    lv->is_block_param = 1;  /* set externally; don't reset in the fixpoint */
  }

  resolve_parents(c);
  inherit_members(c);

  /* mark block-aware methods (contain yield or block_given?) -- these are
     inlined at every call site so block_given? reflects the actual site */
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (!ty) continue;
    if (!strcmp(ty, "YieldNode")) comp_scope_of(c, id)->yields = 1;
    else if (!strcmp(ty, "CallNode") && nt_ref(c->nt, id, "receiver") < 0) {
      const char *nm = nt_str(c->nt, id, "name");
      if (nm && !strcmp(nm, "block_given?")) comp_scope_of(c, id)->yields = 1;
    }
  }

  /* `&block` + block.call: a method whose block parameter never escapes
     (every read is a `.call` receiver or a `&block` forward) is inlined at
     its call sites exactly like a yielding method. The block-param slot is
     then virtual -- the literal block flows in like an implicit yield. */
  for (int mi = 0; mi < c->nscopes; mi++) {
    Scope *m = &c->scopes[mi];
    if (!m->blk_param) continue;
    /* Anonymous `&`: nameless, so it can only be forwarded -- always safe
       to inline (there is no escaping read to worry about). */
    if (!m->blk_param[0]) { m->yields = 1; continue; }
    int escapes = 0, uses = 0;
    for (int id = 0; id < c->nt->count && !escapes; id++) {
      const char *ty = nt_type(c->nt, id);
      if (!ty || strcmp(ty, "LocalVariableReadNode")) continue;
      if (comp_scope_of(c, id) != m) continue;
      const char *nm = nt_str(c->nt, id, "name");
      if (!nm || strcmp(nm, m->blk_param)) continue;
      uses++;
      /* approved: receiver of a `.call`, or expression of a `&block` arg */
      int ok = 0;
      for (int p = 0; p < c->nt->count; p++) {
        const char *pty = nt_type(c->nt, p);
        if (!pty) continue;
        if (!strcmp(pty, "CallNode") && nt_ref(c->nt, p, "receiver") == id) {
          const char *cn = nt_str(c->nt, p, "name");
          if (cn && !strcmp(cn, "call")) { ok = 1; break; }
        }
        if (!strcmp(pty, "BlockArgumentNode") && nt_ref(c->nt, p, "expression") == id) { ok = 1; break; }
      }
      if (!ok) escapes = 1;
    }
    if (!escapes && uses > 0) m->yields = 1;
  }

  /* intern every symbol literal so codegen can emit the id table */
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (ty && !strcmp(ty, "SymbolNode")) {
      const char *v = nt_str(c->nt, id, "value");
      if (v) comp_sym_intern(c, v);
    }
  }

  for (int iter = 0; iter < 128; iter++) {
    int ch = 0;
    ch |= infer_write_types(c);
    ch |= infer_param_types(c);
    ch |= infer_default_param_types(c);
    ch |= infer_block_params(c);
    ch |= infer_for_index(c);
    ch |= infer_ivar_types(c);
    ch |= infer_inherited_ivars(c);
    ch |= infer_global_const_types(c);
    ch |= infer_return_types(c);
    if (!ch) break;
  }

  /* Backstop: a parameter still unknown but with a `= nil` default is a
     nullable param -- represent it as poly so it can hold nil or a value. */
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    for (int i = 0; i < sc->nparams; i++) {
      if (sc->pdefault[i] < 0) continue;
      const char *dty = nt_type(c->nt, sc->pdefault[i]);
      if (!dty || strcmp(dty, "NilNode")) continue;
      LocalVar *p = scope_local(sc, sc->pnames[i]);
      if (p && p->type == TY_UNKNOWN) p->type = TY_POLY;
    }
  }
  /* recompute returns: a method returning such a param is now poly */
  for (int iter = 0; iter < 8; iter++) if (!infer_return_types(c)) break;

  /* Reachability: an instance/free method is live only if its name is
     referenced somewhere -- as a call name, an alias target, or a symbol
     literal (covering send/method/define_method). Names never mentioned
     are dead code; skipping them avoids type-checking uninvoked methods
     (e.g. a never-called method with an uninferrable param). */
  compute_reachable(c);

  /* finalize: gc-root needs + full node type cache */
  for (int s = 0; s < c->nscopes; s++)
    for (int i = 0; i < c->scopes[s].nlocals; i++)
      c->scopes[s].locals[i].gc_root = (c->scopes[s].locals[i].type == TY_STRING);

  for (int id = 0; id < c->nt->count; id++)
    infer_type(c, id);
}
