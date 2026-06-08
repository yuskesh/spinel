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

  /* Class.new(...) -> an instance of that class */
  if (recv >= 0 && !strcmp(name, "new")) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ConstantReadNode")) {
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      if (ci >= 0) return ty_object(ci);
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
    if (!strcmp(name, "length") || !strcmp(name, "size") ||
        !strcmp(name, "count") || !strcmp(name, "index")) return TY_INT;
    if (!strcmp(name, "sum"))                         return ty_array_elem(rt);
    if (!strcmp(name, "first") || !strcmp(name, "last") ||
        !strcmp(name, "min") || !strcmp(name, "max")) return ty_array_elem(rt);
    if (!strcmp(name, "join"))                        return TY_STRING;
    if (!strcmp(name, "inspect") || !strcmp(name, "to_s")) return TY_STRING;
    if (!strcmp(name, "empty?") || !strcmp(name, "include?")) return TY_BOOL;
    if (!strcmp(name, "push") || !strcmp(name, "<<") ||
        !strcmp(name, "reverse") || !strcmp(name, "sort") ||
        !strcmp(name, "uniq") || !strcmp(name, "to_a"))   return rt;
    if (!strcmp(name, "[]="))                         return ty_array_elem(rt);
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
    return ty_hash_of(kt, vt);
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
    } else if (cname) {
      child_class = comp_class_index(c, cname);  /* reopened class */
    }
  } else if (ty && !strcmp(ty, "DefNode")) {
    const char *name = nt_str(c->nt, id, "name");
    Scope *s = comp_scope_new(c, name, id);
    int new_idx = c->nscopes - 1;
    s->body = nt_ref(c->nt, id, "body");
    s->class_id = class_id;   /* instance method of the enclosing class */
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
    } else if (!strcmp(ty, "ConstantWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (nm && is_c_ident(nm)) comp_const_intern(c, nm);
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
    } else if (!strcmp(ty, "GlobalVariableOperatorWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (nm) lv = comp_gvar(c, nm + 1);
      TyKind cur = lv ? lv->type : TY_UNKNOWN;
      TyKind v = infer_type(c, nt_ref(nt, id, "value"));
      if (cur == TY_STRING) vt = TY_STRING;
      else if (ty_is_numeric(cur) && ty_is_numeric(v)) vt = (cur == TY_FLOAT || v == TY_FLOAT) ? TY_FLOAT : TY_INT;
      else vt = cur;
    } else if (!strcmp(ty, "ConstantWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (nm) lv = comp_const(c, nm);
      vt = infer_type(c, nt_ref(nt, id, "value"));
    } else {
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
    } else if (!strcmp(ty, "CallNode")) {
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
      if (newt == TY_NIL) newt = TY_UNKNOWN;
    } else if (!strcmp(ty, "LocalVariableOperatorWriteNode")) {
      nm = nt_str(nt, id, "name");
      Scope *s = comp_scope_of(c, id);
      LocalVar *cur = nm ? scope_local(s, nm) : NULL;
      TyKind vt = infer_type(c, nt_ref(nt, id, "value"));
      TyKind ct = cur ? (TyKind)cur->gc_root : TY_UNKNOWN; /* old type */
      if (ct == TY_STRING) newt = TY_STRING;
      else if (ty_is_numeric(ct) && ty_is_numeric(vt))
        newt = (ct == TY_FLOAT || vt == TY_FLOAT) ? TY_FLOAT : TY_INT;
      else newt = ct;
    } else {
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

  /* Fold container usage into the local type: `a << x` / `a.push(x)` /
     `a[i] = x` on a local `a` promotes it to an array of x's type (this is
     how an empty `[]` gets its element type). Part of the same recompute
     frame so it survives the reset and the change check stays consistent. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "CallNode")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || strcmp(nt_type(nt, recv) ? nt_type(nt, recv) : "", "LocalVariableReadNode")) continue;
    const char *name = nt_str(nt, id, "name");
    if (!name) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0;
    const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    TyKind elem = TY_UNKNOWN;
    if ((!strcmp(name, "push") || !strcmp(name, "<<")) && an == 1) elem = infer_type(c, argv[0]);
    else if (!strcmp(name, "[]=") && an == 2) elem = infer_type(c, argv[1]);
    else continue;
    if (elem == TY_UNKNOWN) continue;
    const char *rnm = nt_str(nt, recv, "name");
    LocalVar *lv = rnm ? scope_local(comp_scope_of(c, recv), rnm) : NULL;
    if (!lv || lv->is_param || lv->is_block_param) continue;
    /* only promote a container local: a known string uses `<<` for append,
       not array push -- don't pollute it */
    if (lv->type != TY_UNKNOWN && !ty_is_array(lv->type)) continue;
    lv->type = ty_unify(lv->type, ty_array_of(elem));
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
    /* Class.new -> initialize params */
    if (!strcmp(name, "new")) {
      const char *rty = nt_type(nt, recv);
      if (rty && !strcmp(rty, "ConstantReadNode")) {
        int ci = comp_class_index(c, nt_str(nt, recv, "name"));
        if (ci >= 0) changed |= bind_call_params(c, id, comp_method_in_class(c, ci, "initialize"));
      }
      continue;
    }
    /* obj.method -> instance method params */
    TyKind rt = infer_type(c, recv);
    if (ty_is_object(rt))
      changed |= bind_call_params(c, id, comp_method_in_class(c, ty_object_class(rt), name));
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
    if (!name || recv < 0) continue;
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
    TyKind r = sc->body >= 0 ? infer_type(c, sc->body) : TY_NIL;
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

void analyze_program(Compiler *c) {
  /* scope 0 = top level */
  Scope *top = comp_scope_new(c, NULL, -1);
  top->body = nt_ref(c->nt, c->nt->root_id, "statements");

  walk_scope(c, c->nt->root_id, 0, -1);
  register_locals(c);
  register_attrs(c);
  register_globals_consts(c);
  resolve_parents(c);
  inherit_members(c);

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
    ch |= infer_ivar_types(c);
    ch |= infer_inherited_ivars(c);
    ch |= infer_global_const_types(c);
    ch |= infer_return_types(c);
    if (!ch) break;
  }

  /* finalize: gc-root needs + full node type cache */
  for (int s = 0; s < c->nscopes; s++)
    for (int i = 0; i < c->scopes[s].nlocals; i++)
      c->scopes[s].locals[i].gc_root = (c->scopes[s].locals[i].type == TY_STRING);

  for (int id = 0; id < c->nt->count; id++)
    infer_type(c, id);
}
