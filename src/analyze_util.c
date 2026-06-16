#include "analyze_internal.h"

int is_builtin_class_name(const char *n) {
  if (!n) return 0;
  static const char *const CL[] = {
    "Integer","Float","String","Symbol","Array","Hash","Range","Time",
    "Module","Class","NilClass","TrueClass","FalseClass","Numeric",
    "Comparable","Enumerable","Object","BasicObject","Proc","Kernel",
    "IO","File","Exception","StandardError","RuntimeError","TypeError",
    "ArgumentError","NameError","NoMethodError","StopIteration","Math",
    "Complex","Rational","Encoding","Method","UnboundMethod","Fiber",
    "Thread","Mutex","GC","ObjectSpace","Signal","Process","Regexp",
    "MatchData","StringIO","StringScanner",NULL
  };
  for (int i = 0; CL[i]; i++) if (!strcmp(n, CL[i])) return 1;
  return 0;
}
int is_builtin_exception_name(const char *n) {
  if (!n) return 0;
  static const char *const EXC[] = {
    "Exception", "StandardError", "RuntimeError", "ArgumentError",
    "TypeError", "NameError", "NoMethodError", "IndexError",
    "KeyError", "RangeError", "IOError", "EOFError", "Errno::ENOENT",
    "ZeroDivisionError", "NotImplementedError", "StopIteration",
    "FloatDomainError", "FrozenError", "EncodingError", "LoadError",
    "SystemExit", "Interrupt", "ScriptError", "SyntaxError",
    "RegexpError", NULL
  };
  for (int i = 0; EXC[i]; i++) if (!strcmp(n, EXC[i])) return 1;
  return 0;
}
int class_inherits_builtin_exception(Compiler *c, int ci) {
  for (int k = ci; k >= 0; k = c->classes[k].parent) {
    int sc = nt_ref(c->nt, c->classes[k].def_node, "superclass");
    if (sc < 0) continue;
    const char *sty = nt_type(c->nt, sc);
    if (sty && !strcmp(sty, "ConstantReadNode") &&
        is_builtin_exception_name(nt_str(c->nt, sc, "name")))
      return 1;
    if (sty && !strcmp(sty, "ConstantPathNode") &&
        is_builtin_exception_name(nt_str(c->nt, sc, "name")))
      return 1;
  }
  return 0;
}
int an_re_has_captures(const char *src) {
  if (!src) return 0;
  for (const char *p = src; *p; p++) {
    if (*p == '\\') { if (p[1]) p++; continue; }
    if (*p == '(' && p[1] != '?') return 1;
  }
  return 0;
}
int str_in(const char *s, const char *const *set) {
  if (!s) return 0;
  for (int i = 0; set[i]; i++) if (strcmp(s, set[i]) == 0) return 1;
  return 0;
}
int is_arith_op(const char *op) {
  static const char *const set[] = {"+", "-", "*", "/", "%", "**", NULL};
  return str_in(op, set);
}
int is_cmp_op(const char *op) {
  static const char *const set[] = {"<", ">", "<=", ">=", NULL};
  return str_in(op, set);
}
int is_eq_op(const char *op) {
  static const char *const set[] = {"==", "!=", "===", NULL};
  return str_in(op, set);
}
int is_void_call(const char *name) {
  static const char *const set[] = {
    "puts", "print", "p", "pp", "require", "require_relative",
    "raise", "warn", "printf", NULL};
  return str_in(name, set);
}
int struct_member_idx(Compiler *c, ClassInfo *sc, int keynode) {
  const NodeTable *nt = c->nt;
  const char *kty = nt_type(nt, keynode);
  if (!kty) return -1;
  if (!strcmp(kty, "SymbolNode")) {
    const char *kn = nt_str(nt, keynode, "value");
    if (!kn) return -1;
    char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", kn);
    int iv = comp_ivar_index(sc, ivn);
    return iv;  /* ivar order == member order */
  }
  if (!strcmp(kty, "IntegerNode")) {
    int idx = (int)nt_int(nt, keynode, "value", -1);
    if (idx >= 0 && idx < sc->nivars) return idx;
  }
  return -1;
}
int scope_body_last(Compiler *c, int mi) {
  int body = c->scopes[mi].body;
  if (body < 0 || !nt_type(c->nt, body) || strcmp(nt_type(c->nt, body), "StatementsNode")) return -1;
  int n = 0; const int *bb = nt_arr(c->nt, body, "body", &n);
  return n > 0 ? bb[n - 1] : -1;
}
int is_blk_param_call(Compiler *c, int node, int mi) {
  const NodeTable *nt = c->nt;
  if (node < 0 || !nt_type(nt, node) || strcmp(nt_type(nt, node), "CallNode")) return 0;
  const char *nm = nt_str(nt, node, "name");
  if (!nm || (strcmp(nm, "call") && strcmp(nm, "()") && strcmp(nm, "[]"))) return 0;
  int recv = nt_ref(nt, node, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || strcmp(nt_type(nt, recv), "LocalVariableReadNode")) return 0;
  const char *rn = nt_str(nt, recv, "name");
  const char *bp = c->scopes[mi].blk_param;
  return rn && bp && bp[0] && !strcmp(rn, bp);
}
int g_yvt_mi[MAX_YVT_DEPTH];
int g_yvt_depth = 0;
int an_ie_class_id = -1;
int g_cbody_class_id = -1;
int g_cbody_direct = -1;
TyKind scan_break_type(Compiler *c, int id, int depth) {
  if (id < 0 || depth > 32) return TY_UNKNOWN;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) return TY_UNKNOWN;
  if (!strcmp(ty, "DefNode")) return TY_UNKNOWN;
  if (!strcmp(ty, "BlockNode") && depth > 0) return TY_UNKNOWN; /* inner block's breaks don't escape */
  if (!strcmp(ty, "BreakNode")) {
    int v = nt_ref(nt, id, "arguments");
    if (v < 0) return TY_NIL;
    int vargc = 0; const int *vargs = nt_arr(nt, v, "arguments", &vargc);
    if (vargc > 0) return infer_type(c, vargs[0]);
    return TY_NIL;
  }
  TyKind result = TY_UNKNOWN;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) {
    TyKind t = scan_break_type(c, nt_ref_at(nt, id, i), depth + 1);
    if (t != TY_UNKNOWN) result = ty_unify(result, t);
  }
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) {
      TyKind t = scan_break_type(c, ids[k], depth + 1);
      if (t != TY_UNKNOWN) result = ty_unify(result, t);
    }
  }
  return result;
}
TyKind scan_throw_type(Compiler *c, int id, int depth) {
  if (id < 0 || depth > 32) return TY_UNKNOWN;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) return TY_UNKNOWN;
  if (!strcmp(ty, "DefNode")) return TY_UNKNOWN;
  if (!strcmp(ty, "CallNode")) {
    const char *nm = nt_str(nt, id, "name");
    if (nm && !strcmp(nm, "throw") && nt_ref(nt, id, "receiver") < 0) {
      int v = nt_ref(nt, id, "arguments");
      int vargc = 0; const int *vargs = v >= 0 ? nt_arr(nt, v, "arguments", &vargc) : NULL;
      if (vargc >= 2) return infer_type(c, vargs[1]);
      return TY_NIL;
    }
  }
  TyKind result = TY_UNKNOWN;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) {
    TyKind t = scan_throw_type(c, nt_ref_at(nt, id, i), depth + 1);
    if (t != TY_UNKNOWN) result = ty_unify(result, t);
  }
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) {
      TyKind t = scan_throw_type(c, ids[k], depth + 1);
      if (t != TY_UNKNOWN) result = ty_unify(result, t);
    }
  }
  return result;
}
TyKind yield_value_type(Compiler *c, int mi) {
  for (int i = 0; i < g_yvt_depth; i++)
    if (g_yvt_mi[i] == mi) return TY_UNKNOWN;
  if (g_yvt_depth >= MAX_YVT_DEPTH) return TY_UNKNOWN;
  g_yvt_mi[g_yvt_depth++] = mi;

  const NodeTable *nt = c->nt;
  TyKind result = TY_UNKNOWN;
  for (int cid = 0; cid < nt->count; cid++) {
    const char *cty = nt_type(nt, cid);
    if (!cty || strcmp(cty, "CallNode")) continue;
    int blk = nt_ref(nt, cid, "block");
    /* A `callee(...)` forward carries its block implicitly inside the `...`
       (no explicit block node); treat it as a forwarded block too. */
    int fwd_args = 0;
    {
      int a = nt_ref(nt, cid, "arguments");
      int an = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
      fwd_args = (an == 1 && av && nt_type(nt, av[0]) &&
                  !strcmp(nt_type(nt, av[0]), "ForwardingArgumentsNode"));
    }
    if (blk < 0 && !fwd_args) continue;
    /* skip calls that live inside method mi itself (recursive self-calls);
       only external call sites provide a concrete block value type */
    if ((int)(comp_scope_of(c, cid) - c->scopes) == mi) continue;
    const char *cn = nt_str(nt, cid, "name");
    int crecv = nt_ref(nt, cid, "receiver");
    int rmi = -1;
    if (crecv < 0) {
      rmi = comp_method_index(c, cn);
      if (rmi < 0) { Scope *cs = comp_scope_of(c, cid); if (cs->class_id >= 0) rmi = comp_method_in_chain(c, cs->class_id, cn, NULL); }
    }
else {
      TyKind crt = infer_type(c, crecv);
      if (ty_is_object(crt)) rmi = comp_method_in_chain(c, ty_object_class(crt), cn, NULL);
      /* `Klass.new { block }`: the block feeds the class's initialize. */
      else if (cn && !strcmp(cn, "new") && nt_type(nt, crecv) &&
               !strcmp(nt_type(nt, crecv), "ConstantReadNode")) {
        int nci = comp_class_index(c, nt_str(nt, crecv, "name"));
        if (nci >= 0) rmi = comp_method_in_chain(c, nci, "initialize", NULL);
      }
    }
    if (rmi != mi) continue;
    /* `mi(&b)` / `mi(...)`: the call forwards the block of its enclosing
       method rather than passing a literal. The value `mi` yields is then
       whatever that forwarded block produces -- the enclosing method's
       own yield value. */
    const char *blkty = blk >= 0 ? nt_type(nt, blk) : NULL;
    if (fwd_args || (blkty && !strcmp(blkty, "BlockArgumentNode"))) {
      Scope *encl = comp_scope_of(c, cid);
      int emi = encl ? (int)(encl - c->scopes) : -1;
      TyKind ft = (emi >= 0 && emi != mi) ? yield_value_type(c, emi) : TY_UNKNOWN;
      if (ft == TY_VOID) ft = TY_NIL;
      if (c->scopes[mi].yields || c->scopes[mi].is_lowered_yield) {
        if (ft != TY_UNKNOWN) { result = ft; break; }
        continue;
      }
      result = ty_unify(result, ft);
      continue;
    }
    int bb = nt_ref(nt, blk, "body");
    int bn = 0; const int *bd = bb >= 0 ? nt_arr(nt, bb, "body", &bn) : NULL;
    TyKind bt;
    if (bn == 0) bt = TY_NIL;
    else if (nt_type(nt, bd[bn - 1]) && !strcmp(nt_type(nt, bd[bn - 1]), "ReturnNode"))
      /* `{ return e }`: a non-local return — the yield never produces a
         value, but typing it as e's type keeps the enclosing method's
         return shape consistent (the inline emits `return e` directly). */
      bt = return_node_type(c, bd[bn - 1]);
    else bt = infer_type(c, bd[bn - 1]);
    if (bt == TY_VOID) bt = TY_NIL;
    /* A yield-inlined (or self-recursive-lowered) method is specialized per
       call site, so its internal block type is the first concrete block. A
       non-inlined method (an escaping &block called via the proc ABI) has one
       body, so its block value type must unify ALL call sites (string + int
       block -> poly). */
    if (c->scopes[mi].yields || c->scopes[mi].is_lowered_yield) { result = bt; break; }
    result = ty_unify(result, bt);
  }
  g_yvt_depth--;
  return result;
}
TyKind method_call_ret(Compiler *c, int mi, int call_id) {
  int last = scope_body_last(c, mi);
  int is_yield = last >= 0 && nt_type(c->nt, last) && !strcmp(nt_type(c->nt, last), "YieldNode");
  /* Lowered yield methods (self-recursive + yield) carry the block's return value:
     return the per-call-site block body type so puts/assign use the right type. */
  if (c->scopes[mi].is_lowered_yield || is_yield || is_blk_param_call(c, last, mi)) {
    int blk = nt_ref(c->nt, call_id, "block");
    const char *bty = blk >= 0 ? nt_type(c->nt, blk) : NULL;
    /* `callee(&b)` / `callee(...)` forwards the block active in the enclosing
       method (a `...` forward carries it implicitly, with no block node), so
       the value `callee` yields is whatever that forwarded block produces --
       i.e. the enclosing method's own per-call-site yield value. */
    int fwd = (bty && !strcmp(bty, "BlockArgumentNode"));
    if (!fwd && blk < 0) {
      int a = nt_ref(c->nt, call_id, "arguments");
      int an = 0; const int *av = a >= 0 ? nt_arr(c->nt, a, "arguments", &an) : NULL;
      fwd = (an == 1 && av && nt_type(c->nt, av[0]) &&
             !strcmp(nt_type(c->nt, av[0]), "ForwardingArgumentsNode"));
    }
    if (fwd) {
      Scope *encl = comp_scope_of(c, call_id);
      int emi = encl ? (int)(encl - c->scopes) : -1;
      if (emi >= 0 && emi != mi) {
        TyKind ft = yield_value_type(c, emi);
        if (ft != TY_UNKNOWN && ft != TY_VOID) return ft;
      }
    }
    if (blk >= 0) {
      int bbody = nt_ref(c->nt, blk, "body");
      int bn = 0; const int *bb = bbody >= 0 ? nt_arr(c->nt, bbody, "body", &bn) : NULL;
      if (bn > 0) {
        const char *lty = nt_type(c->nt, bb[bn - 1]);
        if (lty && !strcmp(lty, "ReturnNode"))
          return return_node_type(c, bb[bn - 1]);  /* `{ return e }`: see yield_value_type */
        return infer_type(c, bb[bn - 1]);
      }
    }
  }
  return c->scopes[mi].ret;
}
int is_proc_constant(const NodeTable *nt, int n) {
  if (n < 0) return 0;
  const char *ty = nt_type(nt, n);
  if (!ty) return 0;
  if (!strcmp(ty, "ConstantReadNode") || !strcmp(ty, "ConstantPathNode")) {
    const char *nm = nt_str(nt, n, "name");
    return nm && !strcmp(nm, "Proc");
  }
  return 0;
}
int is_proc_literal(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty || strcmp(ty, "CallNode")) return 0;
  if (nt_ref(nt, id, "block") < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 && name && (!strcmp(name, "proc") || !strcmp(name, "lambda"))) return 1;
  if (recv >= 0 && name && !strcmp(name, "new") && is_proc_constant(nt, recv)) return 1;
  return 0;
}
int is_proc_create(Compiler *c, int id) {
  const char *ty = nt_type(c->nt, id);
  if (ty && !strcmp(ty, "LambdaNode")) return 1;
  return is_proc_literal(c, id);
}
TyKind proc_node_ret(Compiler *c, int create) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, create);
  int body;
  if (ty && !strcmp(ty, "LambdaNode")) body = nt_ref(nt, create, "body");
  else { int blk = nt_ref(nt, create, "block"); body = blk >= 0 ? nt_ref(nt, blk, "body") : -1; }
  if (body < 0) return TY_NIL;
  int bn = 0;
  const int *bb = nt_arr(nt, body, "body", &bn);
  return bn > 0 ? infer_type(c, bb[bn - 1]) : TY_NIL;
}
TyKind proc_ret_of(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  if (!ty) return TY_UNKNOWN;
  /* unwrap `(expr)` so `(f << g).call` sees the composition node */
  if (!strcmp(ty, "ParenthesesNode")) {
    int body = nt_ref(nt, node, "body");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    return bn == 1 ? proc_ret_of(c, bb[0]) : TY_UNKNOWN;
  }
  if (!strcmp(ty, "LambdaNode") || is_proc_literal(c, node)) return proc_node_ret(c, node);
  if (!strcmp(ty, "LocalVariableReadNode")) {
    Scope *s = comp_scope_of(c, node);
    LocalVar *lv = scope_local(s, nt_str(nt, node, "name"));
    return lv ? (TyKind)lv->proc_ret : TY_UNKNOWN;
  }
  if (!strcmp(ty, "CallNode")) {
    /* a method call that returns a proc -> the callee's recorded proc return */
    int recv = nt_ref(nt, node, "receiver");
    const char *name = nt_str(nt, node, "name");
    /* Hash#to_proc: the proc maps a key to the hash's value type. */
    if (recv >= 0 && name && !strcmp(name, "to_proc")) {
      TyKind rt = infer_type(c, recv);
      if (ty_is_hash(rt)) return ty_hash_val(rt);
    }
    /* proc << proc / proc >> proc: the composed call returns the OUTER proc's
       value (f<<g outer=f; f>>g outer=g). */
    if (recv >= 0 && name && infer_type(c, recv) == TY_PROC) {
      int args = nt_ref(nt, node, "arguments");
      int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an == 1 && infer_type(c, av[0]) == TY_PROC) {
        if (!strcmp(name, "<<")) return proc_ret_of(c, recv);
        if (!strcmp(name, ">>")) return proc_ret_of(c, av[0]);
      }
    }
    int mi = -1;
    if (recv < 0) {
      mi = comp_method_index(c, name);
      if (mi < 0) { Scope *self = comp_scope_of(c, node); if (self->class_id >= 0) mi = comp_method_in_chain(c, self->class_id, name, NULL); }
    }
else {
      TyKind rt = infer_type(c, recv);
      if (ty_is_object(rt)) mi = comp_method_in_chain(c, ty_object_class(rt), name, NULL);
    }
    if (mi >= 0) return (TyKind)c->scopes[mi].ret_proc_ret;
  }
  return TY_UNKNOWN;
}
TyKind proc_call_ret(Compiler *c, int recv) {
  TyKind r = proc_ret_of(c, recv);
  return r == TY_UNKNOWN ? TY_POLY : r;
}

/* The symbol-name argument of a `method(:sym)` call, or NULL. */
const char *method_sym_arg(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  int args = nt_ref(nt, node, "arguments");
  int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an < 1) return NULL;
  const char *aty = nt_type(nt, av[0]);
  if (aty && !strcmp(aty, "SymbolNode")) return nt_str(nt, av[0], "value");
  if (aty && !strcmp(aty, "StringNode")) {
    const char *s = nt_str(nt, av[0], "content");
    return s ? s : nt_str(nt, av[0], "unescaped");
  }
  return NULL;
}

/* True if `node` is a `method(:sym)` / `<recv>.method(:sym)` call. */
int is_method_obj_call(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0 || !nt_type(nt, node) || strcmp(nt_type(nt, node), "CallNode")) return 0;
  const char *nm = nt_str(nt, node, "name");
  return nm && !strcmp(nm, "method") && method_sym_arg(c, node) != NULL;
}

/* The target method scope index bound by a `method(:sym)` node, or -1
   (e.g. a top-level Kernel method like `puts`, or a builtin-array receiver). */
int method_obj_target_mi(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  const char *sym = method_sym_arg(c, node);
  if (!sym) return -1;
  int recv = nt_ref(nt, node, "receiver");
  if (recv < 0) {
    int mi = comp_method_index(c, sym);
    if (mi < 0) { Scope *s = comp_scope_of(c, node); if (s && s->class_id >= 0) mi = comp_method_in_chain(c, s->class_id, sym, NULL); }
    return mi;
  }
  TyKind rt = infer_type(c, recv);
  if (ty_is_object(rt)) return comp_method_in_chain(c, ty_object_class(rt), sym, NULL);
  return -1;
}

/* The `method(:sym)` node a Method-typed expression resolves to: either the
   call itself (inline) or, for a local variable, its assignment in scope. */
int method_recv_node(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  if (recv < 0) return -1;
  if (is_method_obj_call(c, recv)) return recv;
  const char *rty = nt_type(nt, recv);
  if (rty && !strcmp(rty, "LocalVariableReadNode")) {
    const char *vn = nt_str(nt, recv, "name");
    Scope *sc = comp_scope_of(c, recv);
    for (int w = 0; w < nt->count; w++) {
      const char *wty = nt_type(nt, w);
      if (!wty || strcmp(wty, "LocalVariableWriteNode")) continue;
      if (comp_scope_of(c, w) != sc) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || !vn || strcmp(wn, vn)) continue;
      int val = nt_ref(nt, w, "value");
      if (is_method_obj_call(c, val)) return val;
    }
  }
  return -1;
}
