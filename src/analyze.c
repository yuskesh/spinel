#include "analyze.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The receiver class for a node inside an instance_eval/exec block, or -1. */
static int ie_class_of(Compiler *c, int node);

/* Forward declarations for FFI helpers defined later in this file. */
static const char *ffi_arg_str(const NodeTable *nt, int nid);
static int ffi_arg_int(const NodeTable *nt, int nid);
static TyKind ffi_spec_to_ty(const char *spec);
static int ffi_find_func(Compiler *c, const char *mod, const char *name);
static int ffi_find_buf(Compiler *c, const char *mod, const char *name);
static int ffi_find_reader(Compiler *c, const char *mod, const char *name);

static int is_builtin_class_name(const char *n) {
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

static int is_builtin_exception_name(const char *n) {
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

/* Returns 1 if user class ci (or any ancestor in user chain) has a builtin
   exception as direct superclass (per its ClassNode superclass field). */
static int class_inherits_builtin_exception(Compiler *c, int ci) {
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

static int re_has_captures(const char *src) {
  if (!src) return 0;
  for (const char *p = src; *p; p++) {
    if (*p == '\\') { if (p[1]) p++; continue; }
    if (*p == '(' && p[1] != '?') return 1;
  }
  return 0;
}

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
  static const char *const set[] = {"==", "!=", "===", NULL};
  return str_in(op, set);
}
static int is_void_call(const char *name) {
  static const char *const set[] = {
    "puts", "print", "p", "pp", "require", "require_relative",
    "raise", "warn", "printf", NULL};
  return str_in(name, set);
}

/* ---- call inference ---- */

/* Resolve a struct member from a literal key node: a SymbolNode names a
   member; an IntegerNode is a positional index. Returns the member index
   (0-based, matching ivar order) or -1. */
static int struct_member_idx(Compiler *c, ClassInfo *sc, int keynode) {
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

/* Last statement of a scope's body, or -1. */
static int scope_body_last(Compiler *c, int mi) {
  int body = c->scopes[mi].body;
  if (body < 0 || !nt_type(c->nt, body) || strcmp(nt_type(c->nt, body), "StatementsNode")) return -1;
  int n = 0; const int *bb = nt_arr(c->nt, body, "body", &n);
  return n > 0 ? bb[n - 1] : -1;
}

/* The return type of a call to method `mi`. A method whose body is just a
   bare `yield` returns the block's value -- and since it inlines per call
   site, use THIS site's block value type rather than the unified return. */
/* 1 if `node` is `<&block-param>.call(...)` / .() / [] for method mi -- the
   explicit-call equivalent of `yield`, inlined the same way. */
static int is_blk_param_call(Compiler *c, int node, int mi) {
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

/* Re-entrancy guard for yield_value_type: prevents infinite recursion when a
   recursive method forwards its block to itself (e.g. countdown { blk.call }). */
#define MAX_YVT_DEPTH 32
static int g_yvt_mi[MAX_YVT_DEPTH];
static int g_yvt_depth = 0;

/* Temporary class override for instance_eval block body inference.
   -1 = no override; set to the receiver's class_id while inferring
   the block body so that InstanceVariableReadNode uses the right class. */
static int g_ie_class_id = -1;
/* Class index of the class/module body currently being analyzed (set around
   ClassNode/ModuleNode body traversal). -1 outside any class body. */
static int g_cbody_class_id = -1;

/* Scan a subtree for BreakNode values and return their unified type.
   Stops at DefNode/BlockNode boundaries (inner blocks have their own break scope). */
static TyKind scan_break_type(Compiler *c, int id, int depth) {
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

/* Unify the value types of every `throw <tag>, <val>` inside a catch block.
   A nested inner BlockNode is still scanned because `throw` is dynamic. */
static TyKind scan_throw_type(Compiler *c, int id, int depth) {
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

/* The value type of `yield` / a `<&block-param>.call` inside method mi: the
   block-body value type at a (any) call site of mi. Polymorphic, resolved from
   the first matching caller -- matches how the rewrite inlines per call site. */
static TyKind yield_value_type(Compiler *c, int mi) {
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
    if (blk < 0) continue;
    /* skip calls that live inside method mi itself (recursive self-calls);
       only external call sites provide a concrete block value type */
    if ((int)(comp_scope_of(c, cid) - c->scopes) == mi) continue;
    const char *cn = nt_str(nt, cid, "name");
    int crecv = nt_ref(nt, cid, "receiver");
    int rmi = -1;
    if (crecv < 0) {
      rmi = comp_method_index(c, cn);
      if (rmi < 0) { Scope *cs = comp_scope_of(c, cid); if (cs->class_id >= 0) rmi = comp_method_in_chain(c, cs->class_id, cn, NULL); }
    } else {
      TyKind crt = infer_type(c, crecv);
      if (ty_is_object(crt)) rmi = comp_method_in_chain(c, ty_object_class(crt), cn, NULL);
    }
    if (rmi != mi) continue;
    int bb = nt_ref(nt, blk, "body");
    int bn = 0; const int *bd = bb >= 0 ? nt_arr(nt, bb, "body", &bn) : NULL;
    if (bn == 0) { result = TY_NIL; break; }
    TyKind bt = infer_type(c, bd[bn - 1]);
    result = bt == TY_VOID ? TY_NIL : bt;  /* a void last-expr's block value is nil */
    break;
  }
  g_yvt_depth--;
  return result;
}

static TyKind method_call_ret(Compiler *c, int mi, int call_id) {
  int last = scope_body_last(c, mi);
  int is_yield = last >= 0 && nt_type(c->nt, last) && !strcmp(nt_type(c->nt, last), "YieldNode");
  /* Lowered yield methods (self-recursive + yield) carry the block's return value:
     return the per-call-site block body type so puts/assign use the right type. */
  if (c->scopes[mi].is_lowered_yield || is_yield || is_blk_param_call(c, last, mi)) {
    int blk = nt_ref(c->nt, call_id, "block");
    if (blk >= 0) {
      int bbody = nt_ref(c->nt, blk, "body");
      int bn = 0; const int *bb = bbody >= 0 ? nt_arr(c->nt, bbody, "body", &bn) : NULL;
      if (bn > 0) return infer_type(c, bb[bn - 1]);
    }
  }
  return c->scopes[mi].ret;
}

/* 1 if `id` is a proc/lambda literal: `proc {}` / `lambda {}` (CallNode with
   no receiver and a block) or `Proc.new {}`. */
static int is_proc_constant(const NodeTable *nt, int n) {
  if (n < 0) return 0;
  const char *ty = nt_type(nt, n);
  if (!ty) return 0;
  if (!strcmp(ty, "ConstantReadNode") || !strcmp(ty, "ConstantPathNode")) {
    const char *nm = nt_str(nt, n, "name");
    return nm && !strcmp(nm, "Proc");
  }
  return 0;
}
static int is_proc_literal(Compiler *c, int id) {
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

/* 1 if `id` is any proc-creating literal: a proc/lambda/Proc.new call (above)
   or a `->(){}` LambdaNode. */
static int is_proc_create(Compiler *c, int id) {
  const char *ty = nt_type(c->nt, id);
  if (ty && !strcmp(ty, "LambdaNode")) return 1;
  return is_proc_literal(c, id);
}

/* The body return type of a proc-creating node (proc/lambda CallNode literal,
   or a LambdaNode). The last statement of the block/lambda body is the value. */
static TyKind proc_node_ret(Compiler *c, int create) {
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

/* The body return type (`#call`'s result) of a proc-valued expression, or
   TY_UNKNOWN if not statically known. Resolves a literal directly, a local's
   recorded proc_ret, and a method call's recorded ret_proc_ret. */
static TyKind proc_ret_of(Compiler *c, int node) {
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
    } else {
      TyKind rt = infer_type(c, recv);
      if (ty_is_object(rt)) mi = comp_method_in_chain(c, ty_object_class(rt), name, NULL);
    }
    if (mi >= 0) return (TyKind)c->scopes[mi].ret_proc_ret;
  }
  return TY_UNKNOWN;
}

/* The return type of a proc-typed expression's `.call`; poly when unknown. */
static TyKind proc_call_ret(Compiler *c, int recv) {
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

  /* Safe navigation &. : nil receiver always short-circuits to nil */
  {
    const char *call_op = nt_str(nt, id, "call_operator");
    if (recv >= 0 && call_op && !strcmp(call_op, "&.") && rt == TY_NIL) return TY_NIL;
  }

  /* nil receiver: type inference for NilClass methods */
  if (recv >= 0 && rt == TY_NIL) {
    if (!strcmp(name, "to_s") || !strcmp(name, "inspect")) return TY_STRING;
    if (!strcmp(name, "nil?") || !strcmp(name, "is_a?") || !strcmp(name, "kind_of?") ||
        !strcmp(name, "instance_of?")) return TY_BOOL;
    if (!strcmp(name, "to_i") || !strcmp(name, "to_int")) return TY_INT;
    if (!strcmp(name, "to_f") || !strcmp(name, "to_r")) return TY_FLOAT;
    if (!strcmp(name, "to_a")) return TY_POLY_ARRAY;
    if (!strcmp(name, "to_h")) return TY_SYM_POLY_HASH;
    if (!strcmp(name, "respond_to?")) return TY_BOOL;
  }

  /* an empty array literal used directly as a receiver (`[].flatten`) has no
     usage to fold an element type from; treat it as an empty poly array so
     array methods dispatch instead of falling through to unresolved. */
  if (rt == TY_UNKNOWN && recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ArrayNode")) {
      int en = 0; nt_arr(nt, recv, "elements", &en);
      if (en == 0) {
        /* first/last/min/max/pop/shift/sample of an empty array returns 0
           (the typed slot's zero value); carry it as an int */
        if ((!strcmp(name, "first") || !strcmp(name, "last") ||
             !strcmp(name, "min") || !strcmp(name, "max") ||
             !strcmp(name, "sample") ||
             !strcmp(name, "pop") || !strcmp(name, "shift")) && argc == 0) return TY_INT;
        rt = TY_POLY_ARRAY;
      }
    }
  }

  /* `<&block-param>.call(...)` inside a yielding method: the explicit-call form
     of yield. Its value is the call-site block's value (resolved like yield). */
  {
    int emi = (int)(comp_scope_of(c, id) - c->scopes);
    if (emi > 0 && is_blk_param_call(c, id, emi)) return yield_value_type(c, emi);
  }

  /* __dir__ -> the source directory (a string) */
  if (recv < 0 && !strcmp(name, "__dir__") && argc == 0) return TY_STRING;

  /* bare `name` inside a class method body -> the class name string */
  if (recv < 0 && !strcmp(name, "name") && argc == 0) {
    Scope *self = comp_scope_of(c, id);
    if (self && self->is_cmethod && self->class_id >= 0) return TY_STRING;
  }
  /* `self.name` / `self.to_s` inside a class method -> the class name string */
  if (recv >= 0 && argc == 0 &&
      (!strcmp(name, "name") || !strcmp(name, "to_s") || !strcmp(name, "inspect")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "SelfNode")) {
    Scope *self = comp_scope_of(c, id);
    if (self && self->is_cmethod && self->class_id >= 0) return TY_STRING;
  }

  /* loop { break val } -> the type of the break value */
  if (recv < 0 && !strcmp(name, "loop")) {
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0) {
      int body = nt_ref(nt, blk, "body");
      if (body >= 0) {
        TyKind bt = scan_break_type(c, body, 0);
        if (bt != TY_UNKNOWN) return bt;
      }
    }
    return TY_NIL;
  }

  /* catch(:tag) { ... [throw :tag, val] ... } -> unify the block's last
     value with every throw value targeting the tag */
  if (recv < 0 && !strcmp(name, "catch")) {
    int blk = nt_ref(nt, id, "block");
    TyKind result = TY_UNKNOWN;
    if (blk >= 0) {
      int body = nt_ref(nt, blk, "body");
      if (body >= 0) {
        int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
        if (bn > 0) result = infer_type(c, bb[bn - 1]);
        TyKind tt = scan_throw_type(c, body, 0);
        if (tt != TY_UNKNOWN) result = ty_unify(result, tt);
      }
    }
    return result == TY_UNKNOWN ? TY_NIL : result;
  }

  /* recv.instance_eval/exec { ... } -> the block's last-expression type
     (bare calls inside resolve via the ie node->class map). A trampoline
     method `recv.M { ... }` resolves the same way. */
  int ie_kind = (recv >= 0 && (!strcmp(name, "instance_eval") || !strcmp(name, "instance_exec")) &&
                 ty_is_object(rt) && comp_method_in_chain(c, ty_object_class(rt), name, NULL) < 0);
  if (!ie_kind && recv >= 0 && ty_is_object(rt) && nt_ref(nt, id, "block") >= 0)
    ie_kind = comp_trampoline_kind(c, ty_object_class(rt), name, NULL) != 0;
  if (ie_kind) {
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0) {
      int body = nt_ref(nt, blk, "body");
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      if (bn > 0) { TyKind bt = infer_type(c, bb[bn - 1]); return bt == TY_VOID ? TY_NIL : bt; }
      return TY_NIL;
    }
  }

  /* method(:sym) / <recv>.method(:sym) -> a bound Method object */
  if (name && !strcmp(name, "method") && method_sym_arg(c, id) != NULL) return TY_METHOD;

  /* <method>.call(args) / [] -> the target method's return type. */
  if (recv >= 0 && rt == TY_METHOD &&
      (!strcmp(name, "call") || !strcmp(name, "()") || !strcmp(name, "[]"))) {
    int mn = method_recv_node(c, recv);
    int mi = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
    if (mi >= 0) return c->scopes[mi].ret == TY_UNKNOWN ? TY_INT : c->scopes[mi].ret;
    return TY_INT;  /* bound-method ABI returns mrb_int */
  }
  /* <method>.name -> the method name string; .arity -> int */
  if (recv >= 0 && rt == TY_METHOD && argc == 0) {
    if (!strcmp(name, "name")) return TY_STRING;
    if (!strcmp(name, "arity")) return TY_INT;
  }
  /* <poly>.call(args): a boxed Proc/Method called through the runtime ABI,
     which returns mrb_int. (Skip when a user class defines `call`: that goes
     through normal dispatch and returns the method's own type.) */
  if (recv >= 0 && rt == TY_POLY &&
      (!strcmp(name, "call") || !strcmp(name, "()"))) {
    int has_user_call = 0;
    for (int k = 0; k < c->nclasses && !has_user_call; k++)
      if (comp_method_in_class(c, k, "call") >= 0) has_user_call = 1;
    if (!has_user_call) return TY_INT;
  }

  /* proc {} / lambda {} / Proc.new {} -> a first-class Proc value */
  if (is_proc_literal(c, id)) return TY_PROC;

  /* <proc>.call(args) / .() / [] -> the proc's recorded body return type */
  if (recv >= 0 && rt == TY_PROC &&
      (!strcmp(name, "call") || !strcmp(name, "()") || !strcmp(name, "[]")))
    return proc_call_ret(c, recv);

  /* Proc composition: proc << proc / proc >> proc -> a new Proc. */
  if (recv >= 0 && rt == TY_PROC && argc == 1 &&
      (!strcmp(name, "<<") || !strcmp(name, ">>")) &&
      infer_type(c, argv[0]) == TY_PROC)
    return TY_PROC;

  /* Proc introspection */
  if (recv >= 0 && rt == TY_PROC && argc == 0) {
    if (!strcmp(name, "arity")) return TY_INT;
    if (!strcmp(name, "lambda?")) return TY_BOOL;
    if (!strcmp(name, "parameters")) return TY_POLY_ARRAY;
  }

  /* TY_CLASS method dispatch -- .new on a dynamic class variable returns TY_POLY.
     Exception: self.class.new(...) resolves to the enclosing class statically. */
  if (recv >= 0 && rt == TY_CLASS && !strcmp(name, "new") &&
      nt_type(nt, recv) && strcmp(nt_type(nt, recv), "ConstantReadNode") != 0 &&
      strcmp(nt_type(nt, recv), "ConstantPathNode") != 0) {
    int _is_self_class = (nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "class") &&
      nt_ref(nt, recv, "receiver") >= 0 &&
      nt_type(nt, nt_ref(nt, recv, "receiver")) &&
      !strcmp(nt_type(nt, nt_ref(nt, recv, "receiver")), "SelfNode"));
    if (!_is_self_class) return TY_POLY;
  }

  if (recv >= 0 && rt == TY_CLASS && strcmp(name, "new") != 0) {
    if (argc == 0 && (!strcmp(name, "to_s") || !strcmp(name, "name") || !strcmp(name, "inspect")))
      return TY_STRING;
    if (argc == 0 && !strcmp(name, "nil?")) return TY_BOOL;
    if (argc == 0 && !strcmp(name, "class")) return TY_STRING;
    if (argc == 0 && !strcmp(name, "superclass")) return TY_CLASS;
    if (argc == 1 && (!strcmp(name, "==") || !strcmp(name, "eql?") || !strcmp(name, "!="))) return TY_BOOL;
    if (argc == 1 && (!strcmp(name, "<") || !strcmp(name, ">") || !strcmp(name, "<=") || !strcmp(name, ">="))) return TY_BOOL;
    if (argc == 0 && !strcmp(name, "ancestors")) return TY_POLY_ARRAY;
    if (argc == 1 && (!strcmp(name, "is_a?") || !strcmp(name, "kind_of?") || !strcmp(name, "instance_of?"))) return TY_BOOL;
    if (argc <= 1 && !strcmp(name, "instance_methods")) return TY_POLY;
  }

  /* __method__ / __callee__ -> the enclosing method's name (a symbol) */
  if (recv < 0 && argc == 0 &&
      (!strcmp(name, "__method__") || !strcmp(name, "__callee__")))
    return TY_SYMBOL;

  /* identity methods: return the receiver unchanged */
  if (recv >= 0 && argc == 0 &&
      (!strcmp(name, "freeze") || !strcmp(name, "itself") ||
       !strcmp(name, "dup") || !strcmp(name, "clone")))
    return rt;

  /* x.class -> a class-name string (for known builtin receivers) or TY_CLASS (user objects) */
  if (recv >= 0 && argc == 0 && !strcmp(name, "class")) {
    if (ty_is_object(rt)) return TY_CLASS;  /* user object: return sp_Class value */
    if (ty_is_numeric(rt) || rt == TY_STRING || rt == TY_SYMBOL || rt == TY_BOOL ||
        rt == TY_RANGE || rt == TY_TIME || rt == TY_NIL || rt == TY_POLY ||
        rt == TY_METHOD || rt == TY_PROC ||
        ty_is_array(rt) || ty_is_hash(rt))
      return TY_STRING;
  }

  /* X.class.name / .to_s -> the class-name string (X.class is already that) */
  if (recv >= 0 && argc == 0 && (!strcmp(name, "name") || !strcmp(name, "to_s")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "class"))
    return TY_STRING;

  /* __ENCODING__.name / .to_s / .inspect -> the encoding name string */
  if (recv >= 0 && argc == 0 &&
      (!strcmp(name, "name") || !strcmp(name, "to_s") || !strcmp(name, "inspect")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "SourceEncodingNode"))
    return TY_STRING;
  /* <enc>.encoding.name -> the encoding name string */
  if (recv >= 0 && argc == 0 && !strcmp(name, "name") && rt == TY_POLY &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "encoding"))
    return TY_STRING;

  /* Module.singleton_writer= / Module.singleton_reader */
  if (recv >= 0 && nt_type(nt, recv) &&
      (!strcmp(nt_type(nt, recv), "ConstantReadNode") ||
       !strcmp(nt_type(nt, recv), "ConstantPathNode"))) {
    const char *cn = nt_str(nt, recv, "name");
    int ci = cn ? comp_class_index(c, cn) : -1;
    if (ci >= 0) {
      ClassInfo *cls = &c->classes[ci];
      int nlen = (int)strlen(name);
      /* setter: name ends with '=' */
      if (nlen > 1 && name[nlen - 1] == '=') {
        char base[256]; int blen = nlen - 1;
        if (blen > 0 && blen < (int)sizeof(base)) {
          memcpy(base, name, (size_t)blen); base[blen] = '\0';
          if (comp_is_sg_writer(cls, base)) return TY_VOID;
        }
      } else {
        if (comp_is_sg_reader(cls, name)) return TY_POLY;
      }
    }
  }
  /* self.singleton_writer= / self.singleton_reader: inside a class method
     or directly in a class/module body (g_cbody_class_id). */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "SelfNode")) {
    Scope *_self = comp_scope_of(c, id);
    int _sg_cid = (_self && _self->is_cmethod && _self->class_id >= 0)
                  ? _self->class_id : g_cbody_class_id;
    if (_sg_cid >= 0) {
      ClassInfo *_cls = &c->classes[_sg_cid];
      int _nlen = (int)strlen(name);
      if (_nlen > 1 && name[_nlen - 1] == '=') {
        char _base[256]; int _blen = _nlen - 1;
        if (_blen > 0 && _blen < (int)sizeof(_base)) {
          memcpy(_base, name, (size_t)_blen); _base[_blen] = '\0';
          if (comp_is_sg_writer(_cls, _base)) return TY_VOID;
        }
      }
      else if (comp_is_sg_reader(_cls, name)) return TY_POLY;
    }
  }

  /* FFI: call on a module that registered ffi_func/ffi_buffer/ffi_read_* */
  if (recv >= 0 && nt_type(nt, recv)) {
    const char *rty_ffi = nt_type(nt, recv);
    const char *rcmod = NULL;
    if (!strcmp(rty_ffi, "ConstantReadNode"))
      rcmod = nt_str(nt, recv, "name");
    else if (!strcmp(rty_ffi, "ConstantPathNode"))
      rcmod = nt_str(nt, recv, "name");
    if (rcmod) {
      int fi = ffi_find_func(c, rcmod, name);
      if (fi >= 0) return ffi_spec_to_ty(c->ffi_func_ret[fi]);
      /* ffi_buffer: Module.buf_name returns the static char* (ptr type -> TY_POLY) */
      if (ffi_find_buf(c, rcmod, name) >= 0) return TY_POLY;
      /* ffi_read_*: Module.reader_name(buf) returns int or ptr */
      int ri = ffi_find_reader(c, rcmod, name);
      if (ri >= 0) {
        const char *kind = c->ffi_reader_kinds[ri];
        if (kind && !strcmp(kind, "ptr")) return TY_POLY;
        return TY_INT;
      }
    }
  }

  /* SomeClass.name / .to_s / .inspect -> class name string */
  if (recv >= 0 && argc == 0 &&
      (!strcmp(name, "name") || !strcmp(name, "to_s") || !strcmp(name, "inspect")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && comp_class_index(c, nt_str(nt, recv, "name")) >= 0)
    return TY_STRING;
  /* SomeClass.superclass -> sp_Class value for the parent class */
  if (recv >= 0 && argc == 0 && !strcmp(name, "superclass") &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && comp_class_index(c, nt_str(nt, recv, "name")) >= 0)
    return TY_CLASS;

  /* SomeClass.ancestors -> PolyArray of class objects */
  if (recv >= 0 && argc == 0 && !strcmp(name, "ancestors") &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && comp_class_index(c, nt_str(nt, recv, "name")) >= 0)
    return TY_POLY_ARRAY;

  /* SomeClass.instance_methods / .public_instance_methods -> PolyArray of symbols */
  if (recv >= 0 && argc <= 1 &&
      (!strcmp(name, "instance_methods") || !strcmp(name, "public_instance_methods") ||
       !strcmp(name, "private_instance_methods") || !strcmp(name, "protected_instance_methods")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode"))
    return TY_POLY_ARRAY;

  /* self.class.new(...) -> an instance of the enclosing class */
  if (recv >= 0 && !strcmp(name, "new") && nt_type(nt, recv) &&
      !strcmp(nt_type(nt, recv), "CallNode") && nt_str(nt, recv, "name") &&
      !strcmp(nt_str(nt, recv, "name"), "class")) {
    Scope *self = comp_scope_of(c, id);
    if (self && self->class_id >= 0) return ty_object(self->class_id);
  }

  /* Class.new(...) -> an instance of that class; built-in .new constructors */
  if (recv >= 0 && !strcmp(name, "new")) {
    const char *rty = nt_type(nt, recv);
    /* a namespaced class (M::Sub) or root-qualified builtin (::Array etc) */
    if (rty && !strcmp(rty, "ConstantPathNode")) {
      const char *cn = nt_str(nt, recv, "name");
      int ci = cn ? comp_class_index(c, cn) : -1;
      if (ci >= 0) {
        if (class_inherits_builtin_exception(c, ci)) return TY_EXCEPTION;
        int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
        if (ucnew >= 0) return (TyKind)c->scopes[ucnew].ret;
        return ty_object(ci);
      }
      if (cn && is_builtin_exception_name(cn)) return TY_EXCEPTION;
      /* ::Array.new / ::String.new / ::StringIO.new etc. */
      if (cn && !strcmp(cn, "Array") && argc == 2) return ty_array_of(infer_type(c, argv[1]));
      if (cn && !strcmp(cn, "Array")) return TY_POLY_ARRAY;
      if (cn && !strcmp(cn, "Object")) return TY_POLY;
      if (cn && !strcmp(cn, "String")) return TY_STRING;
      if (cn && !strcmp(cn, "StringIO")) return TY_STRINGIO;
      if (cn && !strcmp(cn, "StringScanner")) return TY_STRINGSCANNER;
      if (cn && !strcmp(cn, "Hash")) return TY_UNKNOWN;
      if (cn && !strcmp(cn, "Regexp")) return TY_REGEX;
      if (cn && !strcmp(cn, "Fiber")) return TY_FIBER;
      if (cn && (!strcmp(cn, "Thread") || !strcmp(cn, "Mutex") || !strcmp(cn, "Monitor") ||
                 !strcmp(cn, "Random") || !strcmp(cn, "IO") || !strcmp(cn, "File") ||
                 !strcmp(cn, "GzipReader") || !strcmp(cn, "GzipWriter"))) return TY_POLY;
    }
    if (rty && !strcmp(rty, "ConstantReadNode")) {
      const char *cn = nt_str(nt, recv, "name");
      int ci = comp_class_index(c, cn);
      if (ci >= 0) {
        if (class_inherits_builtin_exception(c, ci)) return TY_EXCEPTION;
        int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
        if (ucnew >= 0) return (TyKind)c->scopes[ucnew].ret;
        return ty_object(ci);
      }
      if (cn && is_builtin_exception_name(cn)) return TY_EXCEPTION;
      if (cn && !strcmp(cn, "Array") && argc == 2) return ty_array_of(infer_type(c, argv[1]));
      if (cn && !strcmp(cn, "Array")) {
        int blk = nt_ref(nt, id, "block");
        if (blk >= 0) {
          /* Array.new(n) { body }: element type from last expression of block body */
          int bbody = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
          if (bn > 0 && bb) { TyKind et = infer_type(c, bb[bn - 1]); if (et != TY_UNKNOWN) return ty_array_of(et); }
        }
        return TY_POLY_ARRAY;
      }
      if (cn && !strcmp(cn, "Array")) return TY_POLY_ARRAY; /* Array.new / Array.new(n) */
      if (cn && !strcmp(cn, "Object")) return TY_POLY;  /* identity sentinel */
      if (cn && !strcmp(cn, "String")) return TY_STRING;
      if (cn && !strcmp(cn, "StringIO")) return TY_STRINGIO;
      if (cn && !strcmp(cn, "StringScanner")) return TY_STRINGSCANNER;
      /* Hash.new { |hash, key| default } : a string-keyed poly hash with a
         default-proc (the block computes the missing-key value). */
      if (cn && !strcmp(cn, "Hash") && nt_ref(nt, id, "block") >= 0) return TY_STR_POLY_HASH;
      if (cn && !strcmp(cn, "Hash")) return TY_UNKNOWN; /* hash type determined by key usage */
      if (cn && !strcmp(cn, "Regexp")) return TY_REGEX;
      /* Builtin object types */
      if (cn && !strcmp(cn, "Fiber")) return TY_FIBER;
      /* Thread.new { block }: modeled as a Fiber run to completion on #value. */
      if (cn && !strcmp(cn, "Thread") && nt_ref(nt, id, "block") >= 0) return TY_FIBER;
      if (cn && !strcmp(cn, "Random")) return TY_RANDOM;
      if (cn && (!strcmp(cn, "Thread") || !strcmp(cn, "Mutex") || !strcmp(cn, "Monitor") ||
                 !strcmp(cn, "IO") || !strcmp(cn, "File") ||
                 !strcmp(cn, "GzipReader") || !strcmp(cn, "GzipWriter"))) return TY_POLY;
    }
  }

  /* Regexp.compile is an alias for Regexp.new */
  if (recv >= 0 && !strcmp(name, "compile")) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ConstantReadNode")) {
      const char *cn = nt_str(nt, recv, "name");
      if (cn && !strcmp(cn, "Regexp")) return TY_REGEX;
    }
  }

  /* StringScanner instance methods */
  if (recv >= 0 && rt == TY_STRINGSCANNER) {
    if (!strcmp(name, "scan") || !strcmp(name, "check") || !strcmp(name, "scan_until") ||
        !strcmp(name, "matched") || !strcmp(name, "pre_match") || !strcmp(name, "post_match") ||
        !strcmp(name, "rest") || !strcmp(name, "string") || !strcmp(name, "getch") ||
        !strcmp(name, "peek") || !strcmp(name, "[]")) return TY_STRING;  /* nullable via NULL */
    if (!strcmp(name, "matched?") || !strcmp(name, "eos?") || !strcmp(name, "rest?")) return TY_BOOL;
    if (!strcmp(name, "pos") || !strcmp(name, "charpos") || !strcmp(name, "rest_size")) return TY_INT;
    if (!strcmp(name, "reset") || !strcmp(name, "terminate") || !strcmp(name, "unscan")) return TY_STRINGSCANNER;
  }

  /* Regexp class methods */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Regexp")) {
    if ((!strcmp(name, "escape") || !strcmp(name, "quote")) && argc >= 1) return TY_STRING;
    if (!strcmp(name, "union") && argc >= 1) return TY_REGEX;
    if (!strcmp(name, "last_match") && argc == 0) return TY_POLY;
    if (!strcmp(name, "last_match") && argc == 1) return TY_STRING;
  }

  /* Regexp instance methods */
  if (recv >= 0 && rt == TY_REGEX) {
    if (!strcmp(name, "match?") || !strcmp(name, "===")) return TY_BOOL;
    if (!strcmp(name, "match")) return TY_MATCHDATA;
    if (!strcmp(name, "=~")) return TY_POLY;
    if (!strcmp(name, "source") || !strcmp(name, "inspect") || !strcmp(name, "to_s")) return TY_STRING;
    if (!strcmp(name, "freeze") || !strcmp(name, "dup") || !strcmp(name, "clone")) return TY_REGEX;
  }

  /* MatchData instance methods */
  if (recv >= 0 && rt == TY_MATCHDATA) {
    if (!strcmp(name, "[]") && argc == 1) return TY_STRING;
    if (!strcmp(name, "pre_match") || !strcmp(name, "post_match") || !strcmp(name, "to_s")) return TY_STRING;
    if (!strcmp(name, "begin") || !strcmp(name, "end") || !strcmp(name, "length") || !strcmp(name, "size")) return TY_INT;
    if (!strcmp(name, "offset")) return TY_INT_ARRAY;
    if (!strcmp(name, "captures") || !strcmp(name, "to_a") || !strcmp(name, "named_captures")) return TY_POLY_ARRAY;
    if (!strcmp(name, "nil?")) return TY_BOOL;
  }

  /* StringIO.open(args) { |io| body } -> the block body's value */
  if (recv >= 0 && !strcmp(name, "open") && nt_type(nt, recv) &&
      !strcmp(nt_type(nt, recv), "ConstantReadNode") && nt_str(nt, recv, "name") &&
      !strcmp(nt_str(nt, recv, "name"), "StringIO")) {
    int block = nt_ref(nt, id, "block");
    int bbody = block >= 0 ? nt_ref(nt, block, "body") : -1;
    if (bbody >= 0) {
      int bn = 0; const int *bb = nt_arr(nt, bbody, "body", &bn);
      return bn > 0 ? infer_type(c, bb[bn - 1]) : TY_NIL;
    }
    return TY_STRINGIO;
  }

  /* StringIO instance methods */
  if (recv >= 0 && rt == TY_STRINGIO) {
    if (!strcmp(name, "string") || !strcmp(name, "read")) return TY_STRING;
    if (!strcmp(name, "gets") || !strcmp(name, "getc")) return TY_POLY;  /* nil at eof */
    if (!strcmp(name, "eof?") || !strcmp(name, "eof") || !strcmp(name, "closed?") ||
        !strcmp(name, "sync") || !strcmp(name, "isatty") || !strcmp(name, "tty?")) return TY_BOOL;
    if (!strcmp(name, "flush")) return TY_STRINGIO;
    /* pos/tell/size/length/lineno/write/puts/print/putc/getbyte/seek/... */
    return TY_INT;
  }

  /* Time.now / at / local / mktime / utc / gm -> a Time value */
  if (recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Time") &&
        (!strcmp(name, "now") || !strcmp(name, "at") || !strcmp(name, "local") ||
         !strcmp(name, "mktime") || !strcmp(name, "utc") || !strcmp(name, "gm") ||
         !strcmp(name, "new")))
      return TY_TIME;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "GC") &&
        (!strcmp(name, "start") || !strcmp(name, "compact")))
      return TY_NIL;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "GC") &&
        !strcmp(name, "stat"))
      return TY_STR_INT_HASH;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Process")) {
      if (!strcmp(name, "pid") || !strcmp(name, "ppid")) return TY_INT;
      if (!strcmp(name, "clock_gettime")) return TY_FLOAT;
    }
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Integer") &&
        !strcmp(name, "sqrt"))
      return TY_INT;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Math") &&
        (!strcmp(name, "sin") || !strcmp(name, "cos") || !strcmp(name, "tan") ||
         !strcmp(name, "asin") || !strcmp(name, "acos") || !strcmp(name, "atan") ||
         !strcmp(name, "atan2") || !strcmp(name, "sinh") || !strcmp(name, "cosh") ||
         !strcmp(name, "tanh") || !strcmp(name, "asinh") || !strcmp(name, "acosh") ||
         !strcmp(name, "atanh") || !strcmp(name, "exp") || !strcmp(name, "log") ||
         !strcmp(name, "log2") || !strcmp(name, "log10") || !strcmp(name, "sqrt") ||
         !strcmp(name, "cbrt") || !strcmp(name, "hypot") || !strcmp(name, "frexp") ||
         !strcmp(name, "ldexp") || !strcmp(name, "erf") || !strcmp(name, "erfc")))
      return TY_FLOAT;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "JSON") &&
        (!strcmp(name, "generate") || !strcmp(name, "dump")))
      return TY_STRING;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Dir") &&
        (!strcmp(name, "exist?") || !strcmp(name, "exists?")))
      return TY_BOOL;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Dir")) {
      if (!strcmp(name, "pwd") || !strcmp(name, "home")) return TY_STRING;
      if (!strcmp(name, "glob")) return TY_STR_ARRAY;
      if (!strcmp(name, "mkdir") || !strcmp(name, "rmdir") || !strcmp(name, "chdir"))
        return TY_INT;
    }
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "File")) {
      if (!strcmp(name, "basename") || !strcmp(name, "dirname") || !strcmp(name, "extname") ||
          !strcmp(name, "read") || !strcmp(name, "binread") || !strcmp(name, "expand_path") ||
          !strcmp(name, "join"))
        return TY_STRING;
      if (!strcmp(name, "exist?") || !strcmp(name, "exists?"))
        return TY_BOOL;
      if (!strcmp(name, "write") || !strcmp(name, "binwrite") || !strcmp(name, "delete"))
        return TY_INT;
      if (!strcmp(name, "readable?") || !strcmp(name, "directory?") || !strcmp(name, "file?") ||
          !strcmp(name, "zero?") || !strcmp(name, "empty?"))
        return TY_BOOL;
      if (!strcmp(name, "mtime"))
        return TY_TIME;
      if (!strcmp(name, "readlines")) return TY_STR_ARRAY;
      /* File.open / File.new without a block -> a typed IO handle */
      if (!strcmp(name, "open") || !strcmp(name, "new")) {
        int blk = nt_ref(nt, id, "block");
        if (blk < 0) return TY_IO;
        /* Pin block param to TY_IO so body dispatch works (f.write, f.puts, etc.) */
        const char *bp0 = block_param_name(c, blk, 0);
        Scope *bs = bp0 ? comp_scope_of(c, blk) : NULL;
        LocalVar *blv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
        if (blv) blv->type = TY_IO;
        return TY_POLY;
      }
    }
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "IO")) {
      /* IO.pipe -> [r, w] pair; each is TY_POLY; the pair is a str_array */
      if (!strcmp(name, "pipe")) return TY_STR_ARRAY;
    }
    /* Fiber.new {} / Thread.new {} / Fiber.current etc.
       Handles both bare Const and ::Const path forms. */
    if (rty && (!strcmp(rty, "ConstantReadNode") || !strcmp(rty, "ConstantPathNode"))) {
      const char *cn2 = nt_str(nt, recv, "name");
      if (cn2 && !strcmp(name, "new") && !strcmp(cn2, "Fiber")) return TY_FIBER;
      /* Thread.new { block } modeled as a Fiber (single-threaded); #value
         resumes it to completion. */
      if (cn2 && !strcmp(name, "new") && !strcmp(cn2, "Thread") &&
          nt_ref(nt, id, "block") >= 0)
        return TY_FIBER;
      if (cn2 && !strcmp(name, "new") && !strcmp(cn2, "Random")) return TY_RANDOM;
      if (cn2 && !strcmp(name, "new") &&
          (!strcmp(cn2, "Thread") || !strcmp(cn2, "Mutex")))
        return TY_POLY;
      if (cn2 && !strcmp(cn2, "Fiber") && !strcmp(name, "current")) return TY_FIBER;
      if (cn2 && !strcmp(cn2, "Fiber") && !strcmp(name, "yield")) return TY_POLY;
      /* Random class methods: Random.rand(n)->int / Random.rand->float */
      if (cn2 && !strcmp(cn2, "Random") && !strcmp(name, "rand"))
        return argc >= 1 ? TY_INT : TY_FLOAT;
      if (cn2 && !strcmp(cn2, "Random") && !strcmp(name, "bytes")) return TY_STRING;
    }
  }

  /* TY_FIBER instance methods */
  if (recv >= 0 && rt == TY_FIBER) {
    if (!strcmp(name, "resume") || !strcmp(name, "transfer")) return TY_POLY;
    if (!strcmp(name, "alive?")) return TY_BOOL;
    if (!strcmp(name, "value")) return TY_POLY;
  }

  /* TY_RANDOM instance methods */
  if (recv >= 0 && rt == TY_RANDOM) {
    if (!strcmp(name, "rand")) return argc >= 1 ? TY_INT : TY_FLOAT;
    if (!strcmp(name, "bytes")) return TY_STRING;
    if (!strcmp(name, "seed")) return TY_INT;
  }

  /* TY_IO (File/IO handle) instance methods */
  if (recv >= 0 && rt == TY_IO) {
    if (!strcmp(name, "read") || !strcmp(name, "gets") || !strcmp(name, "readline") ||
        !strcmp(name, "path") || !strcmp(name, "to_path")) return TY_STRING;
    if (!strcmp(name, "read") && nt_ref(nt, id, "arguments") >= 0) return TY_STRING;
    if (!strcmp(name, "readlines")) return TY_STR_ARRAY;
    if (!strcmp(name, "write") || !strcmp(name, "syswrite") || !strcmp(name, "pos") ||
        !strcmp(name, "tell") || !strcmp(name, "seek") || !strcmp(name, "rewind") ||
        !strcmp(name, "close")) return TY_INT;
    if (!strcmp(name, "print") || !strcmp(name, "puts") || !strcmp(name, "flush")) return TY_NIL;
    if (!strcmp(name, "closed?") || !strcmp(name, "eof?") || !strcmp(name, "eof")) return TY_BOOL;
    if (!strcmp(name, "each_line") || !strcmp(name, "each")) {
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) {
        const char *bp0 = block_param_name(c, blk, 0);
        Scope *bs = bp0 ? comp_scope_of(c, blk) : NULL;
        LocalVar *blv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
        if (blv) blv->type = TY_STRING;
      }
      return TY_IO;
    }
    return TY_POLY;
  }

  /* Time instance methods */
  if (recv >= 0 && rt == TY_TIME) {
    if (!strcmp(name, "-") && argc > 0) {
      TyKind at = infer_type(c, argv[0]);
      if (at == TY_TIME) return TY_FLOAT;
    }
    if (!strcmp(name, "utc") || !strcmp(name, "gmtime") || !strcmp(name, "getutc") ||
        !strcmp(name, "localtime") || !strcmp(name, "getlocal") || !strcmp(name, "+") ||
        !strcmp(name, "-")) return TY_TIME;
    if (!strcmp(name, "to_s") || !strcmp(name, "inspect") || !strcmp(name, "strftime") ||
        !strcmp(name, "iso8601") || !strcmp(name, "zone") || !strcmp(name, "asctime") ||
        !strcmp(name, "ctime")) return TY_STRING;
    if (!strcmp(name, "to_f") || !strcmp(name, "subsec")) return TY_FLOAT;
    if (!strcmp(name, "utc?") || !strcmp(name, "gmt?") || !strcmp(name, "dst?") ||
        !strcmp(name, "isdst") ||
        !strcmp(name, "sunday?") || !strcmp(name, "monday?") ||
        !strcmp(name, "<") || !strcmp(name, ">") || !strcmp(name, "<=") ||
        !strcmp(name, ">=") || !strcmp(name, "==") || !strcmp(name, "!=")) return TY_BOOL;
    if (!strcmp(name, "<=>")) return TY_INT;
    if (!strcmp(name, "class")) return TY_STRING;
    /* year/mon/day/hour/min/sec/wday/yday/to_i/tv_sec/tv_usec/usec/tv_nsec/nsec/... */
    return TY_INT;
  }

  /* Class.cmethod(...) / M::Sub.cmethod(...) -> the class method's return type */
  if (recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && (!strcmp(rty, "ConstantReadNode") || !strcmp(rty, "ConstantPathNode"))) {
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      if (ci >= 0) {
        int mi = comp_cmethod_in_chain(c, ci, name, NULL);
        if (mi >= 0) return c->scopes[mi].ret;
      }
    }
    /* obj.class.cmeth(...) -> unify class method return types across hierarchy */
    if (rty && !strcmp(rty, "CallNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "class")) {
      int robj = nt_ref(nt, recv, "receiver");
      TyKind rrt = robj >= 0 ? infer_type(c, robj) : TY_UNKNOWN;
      if (ty_is_object(rrt)) {
        int cid = ty_object_class(rrt);
        int mi = comp_cmethod_in_chain(c, cid, name, NULL);
        if (mi >= 0) {
          TyKind r = (TyKind)c->scopes[mi].ret;
          for (int k = 0; k < c->nclasses; k++) {
            int _desc = 0;
            for (int _p = c->classes[k].parent; _p >= 0; _p = c->classes[_p].parent)
              if (_p == cid) { _desc = 1; break; }
            if (!_desc) continue;
            int kmi = comp_cmethod_in_class(c, k, name);
            if (kmi >= 0) r = ty_unify(r, (TyKind)c->scopes[kmi].ret);
          }
          return r;
        }
      }
    }
  }

  /* Struct instance methods */
  if (recv >= 0 && ty_is_object(rt) && c->classes[ty_object_class(rt)].is_struct) {
    ClassInfo *sc = &c->classes[ty_object_class(rt)];
    if (!strcmp(name, "to_a") || !strcmp(name, "values") ||
        !strcmp(name, "deconstruct") || !strcmp(name, "members")) return TY_POLY_ARRAY;
    if (!strcmp(name, "to_h")) {
      int block = nt_ref(nt, id, "block");
      if (block >= 0) {
        /* to_h { |k,v| [nk, nv] }: hash type from the block's pair */
        int bbody = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
        int last = bn > 0 ? bb[bn - 1] : -1;
        if (last >= 0 && nt_type(nt, last) && !strcmp(nt_type(nt, last), "ArrayNode")) {
          int en = 0; const int *els = nt_arr(nt, last, "elements", &en);
          if (en == 2) {
            TyKind kt = infer_type(c, els[0]), vt = infer_type(c, els[1]);
            if (kt == TY_SYMBOL) return TY_SYM_POLY_HASH;
            if (kt == TY_STRING && vt == TY_STRING) return TY_STR_STR_HASH;
            if (kt == TY_STRING) return TY_STR_POLY_HASH;
            TyKind h = ty_hash_of(kt, vt);
            return h != TY_UNKNOWN ? h : TY_STR_POLY_HASH;
          }
        }
      }
      return TY_SYM_POLY_HASH;
    }
    if (!strcmp(name, "dig") && argc >= 1) {
      int mi = struct_member_idx(c, sc, argv[0]);
      if (mi >= 0) {
        TyKind mt = sc->ivar_types[mi];
        if (argc == 1) return mt;
        /* dig(member, key, ...): index into the member's container */
        if (ty_is_hash(mt) && argc == 2) return ty_hash_val(mt);
        if (ty_is_array(mt) && argc == 2) return ty_array_elem(mt);
        return TY_POLY;
      }
    }
    if (!strcmp(name, "[]") && argc == 1) {
      /* struct[:sym] or struct[int]: return specific member type if known */
      int mi = struct_member_idx(c, sc, argv[0]);
      if (mi >= 0) return sc->ivar_types[mi];
      /* integer index: try to resolve literal */
      const char *kty = nt_type(nt, argv[0]);
      if (kty && !strcmp(kty, "IntegerNode")) {
        long long idx = (long long)nt_int(nt, argv[0], "value", 0);
        if (idx < 0) idx += (long long)sc->nivars;
        if (idx >= 0 && idx < sc->nivars) return sc->ivar_types[(int)idx];
      }
      return TY_POLY;
    }
    if (!strcmp(name, "[]=") && argc == 2) return sc->nivars > 0 ? sc->ivar_types[0] : TY_POLY;
  }

  /* built-in class reopening: look up user-defined methods on scalar built-in types */
  if (recv >= 0) {
    const char *oc_cn = NULL;
    if (rt == TY_STRING)       oc_cn = "String";
    else if (rt == TY_INT)     oc_cn = "Integer";
    else if (rt == TY_FLOAT)   oc_cn = "Float";
    else if (rt == TY_SYMBOL)  oc_cn = "Symbol";
    else if (rt == TY_BOOL)    oc_cn = "TrueClass";
    if (oc_cn) {
      int oc_ci = comp_class_index(c, oc_cn);
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return method_call_ret(c, oc_mi, id);
      }
    }
  }

  /* obj.method(...) -> the method's return type (walks the superclass chain) */
  if (recv >= 0 && ty_is_object(rt)) {
    int cid = ty_object_class(rt);
    ClassInfo *cls = &c->classes[cid];
    if (!strcmp(name, "is_a?") || !strcmp(name, "kind_of?") || !strcmp(name, "instance_of?") ||
        !strcmp(name, "respond_to?") || !strcmp(name, "==") || !strcmp(name, "!=") ||
        !strcmp(name, "nil?") || !strcmp(name, "equal?") || !strcmp(name, "frozen?")) return TY_BOOL;
    /* attr reader (resolve alias so `alias v access_token` returns @access_token type) */
    { int rdcls = -1;
      if (comp_reader_in_chain(c, cid, name, &rdcls)) {
        const char *rname = comp_resolve_alias(c, cid, name);
        char ivn[256];
        snprintf(ivn, sizeof ivn, "@%s", rname);
        ClassInfo *rci = (rdcls >= 0 && rdcls < c->nclasses) ? &c->classes[rdcls] : cls;
        int iv = comp_ivar_index(rci, ivn);
        if (iv >= 0) return rci->ivar_types[iv];
      }
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
    if (mi >= 0) {
      TyKind r = method_call_ret(c, mi, id);
      /* Unify with descendant direct overrides: codegen dispatch emits a
         cls_id switch over all overrides, so the result type must cover all. */
      for (int k = 0; k < c->nclasses; k++) {
        int is_desc = 0;
        for (int p = c->classes[k].parent; p >= 0; p = c->classes[p].parent)
          if (p == cid) { is_desc = 1; break; }
        if (!is_desc) continue;
        int dmi = comp_method_in_class(c, k, name);
        if (dmi >= 0) r = ty_unify(r, (TyKind)c->scopes[dmi].ret);
      }
      return r;
    }
    if (!strcmp(name, "to_s") || !strcmp(name, "inspect")) return TY_STRING;
  }

  /* implicit-self call inside an instance method */
  if (recv < 0) {
    Scope *self = comp_scope_of(c, id);
    if (self->class_id >= 0) {
      { int rdcls2 = -1;
        if (comp_reader_in_chain(c, self->class_id, name, &rdcls2)) {
          const char *rname2 = comp_resolve_alias(c, self->class_id, name);
          char ivn[256];
          snprintf(ivn, sizeof ivn, "@%s", rname2);
          ClassInfo *rci2 = (rdcls2 >= 0 && rdcls2 < c->nclasses) ? &c->classes[rdcls2] : &c->classes[self->class_id];
          int iv = comp_ivar_index(rci2, ivn);
          if (iv >= 0) return rci2->ivar_types[iv];
        }
      }
      /* bare `new` inside a class method returns an instance of self's class */
      if (self->is_cmethod && !strcmp(name, "new"))
        return ty_object(self->class_id);
      int mi = comp_method_in_chain(c, self->class_id, name, NULL);
      if (mi < 0 && self->is_cmethod)
        mi = comp_cmethod_in_chain(c, self->class_id, name, NULL);
      if (mi >= 0) {
        TyKind r = method_call_ret(c, mi, id);
        /* Unify with descendant direct overrides: codegen dispatch will
           emit a cls_id switch over all overrides, so the return type
           must accommodate every override's return type. */
        for (int k = 0; k < c->nclasses; k++) {
          int is_desc = 0;
          for (int p = c->classes[k].parent; p >= 0; p = c->classes[p].parent)
            if (p == self->class_id) { is_desc = 1; break; }
          if (!is_desc) continue;
          int dmi = self->is_cmethod ? comp_cmethod_in_class(c, k, name) :
                                       comp_method_in_class(c, k, name);
          if (dmi >= 0) r = ty_unify(r, (TyKind)c->scopes[dmi].ret);
        }
        return r;
      }
      /* Built-in class reopening: implicit self → delegate to built-in type lookup */
      if (mi < 0 && !self->is_cmethod) {
        const char *bcn = c->classes[self->class_id].name;
        TyKind brt = TY_UNKNOWN;
        if (!strcmp(bcn, "String"))        brt = TY_STRING;
        else if (!strcmp(bcn, "Integer"))  brt = TY_INT;
        else if (!strcmp(bcn, "Float"))    brt = TY_FLOAT;
        else if (!strcmp(bcn, "Symbol"))   brt = TY_SYMBOL;
        if (brt != TY_UNKNOWN) {
          /* Temporarily set rt to the built-in type and recursively call infer_call
             is not safe. Instead inline key return types for common method names. */
          if (brt == TY_STRING) {
            if (!strcmp(name, "upcase") || !strcmp(name, "downcase") ||
                !strcmp(name, "capitalize") || !strcmp(name, "reverse") || !strcmp(name, "strip") ||
                !strcmp(name, "lstrip") || !strcmp(name, "rstrip") || !strcmp(name, "chomp") ||
                !strcmp(name, "chop") || !strcmp(name, "dup") || !strcmp(name, "clone") ||
                !strcmp(name, "to_s") || !strcmp(name, "inspect") || !strcmp(name, "succ") ||
                !strcmp(name, "next") || !strcmp(name, "chr") || !strcmp(name, "encode") ||
                !strcmp(name, "b") || !strcmp(name, "force_encoding") || !strcmp(name, "scrub") ||
                !strcmp(name, "squeeze") || !strcmp(name, "tr") || !strcmp(name, "delete"))
              return TY_STRING;
            if ((!strcmp(name, "+") || !strcmp(name, "*")) && argc >= 1) return TY_STRING;
            if (!strcmp(name, "gsub") || !strcmp(name, "sub")) return TY_STRING;
            if (!strcmp(name, "[]") || !strcmp(name, "slice")) return TY_STRING;
            if (!strcmp(name, "length") || !strcmp(name, "size") || !strcmp(name, "bytesize") ||
                !strcmp(name, "to_i") || !strcmp(name, "count") || !strcmp(name, "ord") ||
                !strcmp(name, "hex") || !strcmp(name, "oct") || !strcmp(name, "rindex") ||
                !strcmp(name, "index"))
              return TY_INT;
            if (!strcmp(name, "to_f")) return TY_FLOAT;
            if (!strcmp(name, "to_sym")) return TY_SYMBOL;
            if (!strcmp(name, "empty?") || !strcmp(name, "include?") ||
                !strcmp(name, "start_with?") || !strcmp(name, "end_with?") ||
                !strcmp(name, "==") || !strcmp(name, "!="))
              return TY_BOOL;
            if (!strcmp(name, "split") || !strcmp(name, "chars") || !strcmp(name, "lines") ||
                !strcmp(name, "bytes"))
              return TY_STR_ARRAY;
          }
          else if (brt == TY_INT) {
            if (!strcmp(name, "+") || !strcmp(name, "-") || !strcmp(name, "*") ||
                !strcmp(name, "/") || !strcmp(name, "%") || !strcmp(name, "**") ||
                !strcmp(name, "abs") || !strcmp(name, "succ") || !strcmp(name, "next") ||
                !strcmp(name, "pred") || !strcmp(name, "gcd") || !strcmp(name, "lcm") ||
                !strcmp(name, "&") || !strcmp(name, "|") || !strcmp(name, "^") ||
                !strcmp(name, "<<") || !strcmp(name, ">>"))
              return TY_INT;
            if (!strcmp(name, "to_f")) return TY_FLOAT;
            if (!strcmp(name, "to_s")) return TY_STRING;
            if (!strcmp(name, "to_r")) return TY_POLY;
            if (!strcmp(name, "odd?") || !strcmp(name, "even?") || !strcmp(name, "zero?") ||
                !strcmp(name, "==") || !strcmp(name, "!=") || !strcmp(name, "<") ||
                !strcmp(name, "<=") || !strcmp(name, ">") || !strcmp(name, ">="))
              return TY_BOOL;
          }
          else if (brt == TY_FLOAT) {
            if (!strcmp(name, "+") || !strcmp(name, "-") || !strcmp(name, "*") ||
                !strcmp(name, "/") || !strcmp(name, "**") || !strcmp(name, "abs") ||
                !strcmp(name, "floor") || !strcmp(name, "ceil") || !strcmp(name, "round") ||
                !strcmp(name, "truncate"))
              return TY_FLOAT;
            if (!strcmp(name, "to_i")) return TY_INT;
            if (!strcmp(name, "to_s")) return TY_STRING;
            if (!strcmp(name, "zero?") || !strcmp(name, "nan?") || !strcmp(name, "infinite?") ||
                !strcmp(name, "finite?") || !strcmp(name, "==") || !strcmp(name, "!=") ||
                !strcmp(name, "<") || !strcmp(name, "<=") || !strcmp(name, ">") ||
                !strcmp(name, ">="))
              return TY_BOOL;
          }
          else if (brt == TY_SYMBOL) {
            if (!strcmp(name, "to_s") || !strcmp(name, "id2name") || !strcmp(name, "inspect"))
              return TY_STRING;
            if (!strcmp(name, "to_sym") || !strcmp(name, "itself")) return TY_SYMBOL;
            if (!strcmp(name, "length") || !strcmp(name, "size")) return TY_INT;
            if (!strcmp(name, "empty?") || !strcmp(name, "==") || !strcmp(name, "!="))
              return TY_BOOL;
          }
        }
      }
      /* Method defined only in descendants (not in base chain):
         unify return types of all descendant implementations. */
      if (self->is_cmethod) {
        TyKind r = TY_UNKNOWN; int found = 0;
        for (int k = 0; k < c->nclasses; k++) {
          int is_desc = 0;
          for (int p = c->classes[k].parent; p >= 0; p = c->classes[p].parent)
            if (p == self->class_id) { is_desc = 1; break; }
          if (!is_desc) continue;
          int dmi = comp_cmethod_in_class(c, k, name);
          if (dmi < 0) continue;
          r = found ? ty_unify(r, (TyKind)c->scopes[dmi].ret) : (TyKind)c->scopes[dmi].ret;
          found = 1;
        }
        if (found) return r;
      }
    }
  }

  /* bare call inside a module/class body -> class method of that module/class */
  if (recv < 0 && g_cbody_class_id >= 0) {
    int smi = comp_cmethod_in_chain(c, g_cbody_class_id, name, NULL);
    if (smi >= 0) return method_call_ret(c, smi, id);
  }
  /* bare call inside an instance_eval/exec block: dispatch on receiver class */
  if (recv < 0) {
    int iec = ie_class_of(c, id);
    if (iec >= 0) {
      int imi = comp_method_in_chain(c, iec, name, NULL);
      if (imi >= 0) return method_call_ret(c, imi, id);
    }
  }
  /* user-defined free-function call (no receiver) */
  if (recv < 0) {
    int mi = comp_method_index(c, name);
    if (mi < 0) mi = comp_included_method_index(c, name);
    if (mi >= 0) return method_call_ret(c, mi, id);
    /* Kernel conversions */
    if (!strcmp(name, "Integer") && (argc == 1 || argc == 2)) return TY_INT;
    if (!strcmp(name, "Float") && argc == 1) return TY_FLOAT;
    if (!strcmp(name, "String") && argc == 1) return TY_STRING;
    if ((!strcmp(name, "format") || !strcmp(name, "sprintf")) && argc >= 1) return TY_STRING;
    if (!strcmp(name, "system") && argc >= 1) return TY_BOOL;
    if (!strcmp(name, "trap") && argc >= 1) return TY_STRING;
    if (!strcmp(name, "rand")) {
      if (argc == 0) return TY_FLOAT;
      /* rand(float_range) → Float */
      const char *atype = nt_type(nt, argv[0]);
      if (atype && !strcmp(atype, "RangeNode")) {
        int lo = nt_ref(nt, argv[0], "left");
        if (lo >= 0 && infer_type(c, lo) == TY_FLOAT) return TY_FLOAT;
      }
      return TY_INT;
    }
    if (!strcmp(name, "srand")) return TY_INT;
  }
  /* Signal.trap / ::Signal.trap */
  if (recv >= 0 && !strcmp(name, "trap") && argc >= 1) {
    const char *rty = nt_type(nt, recv);
    if (rty && (!strcmp(rty, "ConstantReadNode") || !strcmp(rty, "ConstantPathNode"))) {
      const char *rname = nt_str(nt, recv, "name");
      if (rname && !strcmp(rname, "Signal")) return TY_STRING;
    }
  }

  /* Fiber storage: Fiber[:k] and Fiber.current[:k] -> poly */
  if (recv >= 0 && !strcmp(name, "[]") && argc == 1) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && !strcmp(rn, "Fiber")) return TY_POLY;
    }
    if (rty && !strcmp(rty, "CallNode")) {
      const char *rn = nt_str(nt, recv, "name");
      int rr = nt_ref(nt, recv, "receiver");
      if (rn && !strcmp(rn, "current") && rr >= 0) {
        const char *rrty = nt_type(nt, rr);
        const char *rrn = nt_str(nt, rr, "name");
        if (rrty && !strcmp(rrty, "ConstantReadNode") && rrn && !strcmp(rrn, "Fiber"))
          return TY_POLY;
      }
    }
  }
  /* Fiber[:k] = v -> returns v's type */
  if (recv >= 0 && !strcmp(name, "[]=") && argc == 2) {
    const char *rty = nt_type(nt, recv);
    int is_fiber = 0;
    if (rty && !strcmp(rty, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && !strcmp(rn, "Fiber")) is_fiber = 1;
    }
    else if (rty && !strcmp(rty, "CallNode")) {
      const char *rn = nt_str(nt, recv, "name");
      int rr = nt_ref(nt, recv, "receiver");
      if (rn && !strcmp(rn, "current") && rr >= 0) {
        const char *rrty = nt_type(nt, rr);
        const char *rrn = nt_str(nt, rr, "name");
        if (rrty && !strcmp(rrty, "ConstantReadNode") && rrn && !strcmp(rrn, "Fiber"))
          is_fiber = 1;
      }
    }
    if (is_fiber) return infer_type(c, argv[1]);
  }
  /* ENV[key] -> string or nil (use TY_STRING; null means nil) */
  if (recv >= 0 && argc >= 1 && (!strcmp(name, "[]") || !strcmp(name, "fetch"))) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && !strcmp(rn, "ENV")) return TY_STRING;
    }
  }

  /* each_slice(n).map/collect { |...| } chain: return array of block result type */
  if (recv >= 0 && rt == TY_UNKNOWN && (!strcmp(name, "map") || !strcmp(name, "collect")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "each_slice") &&
      nt_ref(nt, recv, "block") < 0) {
    int blk_es = nt_ref(nt, id, "block");
    if (blk_es >= 0) {
      int body_es = nt_ref(nt, blk_es, "body");
      int bn_es = 0; const int *bb_es = body_es >= 0 ? nt_arr(nt, body_es, "body", &bn_es) : NULL;
      return ty_array_of(bn_es > 0 ? infer_type(c, bb_es[bn_es - 1]) : TY_UNKNOWN);
    }
  }

  /* each_cons(n).map/collect { |...| } chain: return array of block result type */
  if (recv >= 0 && rt == TY_UNKNOWN && (!strcmp(name, "map") || !strcmp(name, "collect")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "each_cons") &&
      nt_ref(nt, recv, "block") < 0) {
    int blk_ec = nt_ref(nt, id, "block");
    if (blk_ec >= 0) {
      int body_ec = nt_ref(nt, blk_ec, "body");
      int bn_ec = 0; const int *bb_ec = body_ec >= 0 ? nt_arr(nt, body_ec, "body", &bn_ec) : NULL;
      return ty_array_of(bn_ec > 0 ? infer_type(c, bb_ec[bn_ec - 1]) : TY_UNKNOWN);
    }
  }

  /* each_cons(n).with_index(off).map/collect { |...| } chain */
  if (recv >= 0 && rt == TY_UNKNOWN && (!strcmp(name, "map") || !strcmp(name, "collect")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "with_index") &&
      nt_ref(nt, recv, "block") < 0) {
    int wi_recv = nt_ref(nt, recv, "receiver");
    if (wi_recv >= 0 && nt_type(nt, wi_recv) && !strcmp(nt_type(nt, wi_recv), "CallNode") &&
        nt_str(nt, wi_recv, "name") && !strcmp(nt_str(nt, wi_recv, "name"), "each_cons") &&
        nt_ref(nt, wi_recv, "block") < 0) {
      int blk_wi = nt_ref(nt, id, "block");
      if (blk_wi >= 0) {
        int body_wi = nt_ref(nt, blk_wi, "body");
        int bn_wi = 0; const int *bb_wi = body_wi >= 0 ? nt_arr(nt, body_wi, "body", &bn_wi) : NULL;
        return ty_array_of(bn_wi > 0 ? infer_type(c, bb_wi[bn_wi - 1]) : TY_UNKNOWN);
      }
    }
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
      if (!strcmp(name, "max_by") || !strcmp(name, "min_by") ||
          !strcmp(name, "find") || !strcmp(name, "detect"))
        return ty_array_elem(rt);  /* returns an element */
      if (!strcmp(name, "partition")) return TY_POLY_ARRAY;  /* [[truthy...],[falsy...]] */
    }
    /* grep/grep_v without a block filter by `pattern === e`, preserving the
       receiver's array type. */
    if ((!strcmp(name, "grep") || !strcmp(name, "grep_v")) &&
        nt_ref(nt, id, "block") < 0 && argc == 1)
      return rt;
    if (!strcmp(name, "[]")) {
      /* arr[range] / arr[start, len] -> a subarray; arr[i] -> an element */
      if (argc == 2) return rt;
      if (argc == 1 && nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "RangeNode")) return rt;
      return ty_array_elem(rt);
    }
    if (!strcmp(name, "at") && argc == 1) return ty_array_elem(rt);  /* like [i] */
    if (!strcmp(name, "fetch") && (argc == 1 || argc == 2)) return ty_array_elem(rt);
    if (!strcmp(name, "dig") && argc >= 1) {
      if (argc == 1) return ty_array_elem(rt);
      return TY_POLY;
    }
    /* index returns nil on a miss -> poly (int-or-nil) */
    if ((!strcmp(name, "index") || !strcmp(name, "find_index") || !strcmp(name, "rindex")) &&
        (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY)) return TY_POLY;
    if (!strcmp(name, "length") || !strcmp(name, "size") ||
        !strcmp(name, "count") || !strcmp(name, "index") || !strcmp(name, "find_index")) return TY_INT;
    if (!strcmp(name, "sum")) {
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) {
        int body = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        return bn > 0 ? infer_type(c, bb[bn - 1]) : ty_array_elem(rt);
      }
      return ty_array_elem(rt);
    }
    if (!strcmp(name, "inject") || !strcmp(name, "reduce")) {
      /* When an init argument is provided, the return type matches the init type.
         inject(:op) is the no-init operator form — the sole symbol arg is the
         operator, NOT an init value, so skip the "return argv[0] type" path. */
      if (argc > 0 && argv) {
        const char *a0ty = nt_type(nt, argv[0]);
        int is_sym_op = a0ty && !strcmp(a0ty, "SymbolNode") && argc == 1;
        if (!is_sym_op) {
          TyKind it = infer_type(c, argv[0]);
          if (it != TY_UNKNOWN) return it;
        }
      }
      /* empty array literal `[]` with sym op: codegen treats as int_array → returns int */
      if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ArrayNode")) {
        int en = 0; nt_arr(nt, recv, "elements", &en);
        if (en == 0) return TY_INT;
      }
      /* Block body last expression determines the return type when available. */
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) {
        int body = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn > 0) { TyKind bt = infer_type(c, bb[bn - 1]); if (bt != TY_UNKNOWN) return bt; }
      }
      return ty_array_elem(rt);
    }
    if (!strcmp(name, "each_with_object") && argc > 0 && argv) {
      TyKind at = infer_type(c, argv[0]);
      if (at == TY_UNKNOWN) {
        const char *a0ty = nt_type(nt, argv[0]);
        int an0 = 0;
        if (a0ty && !strcmp(a0ty, "ArrayNode")) nt_arr(nt, argv[0], "elements", &an0);
        if (a0ty && !strcmp(a0ty, "ArrayNode") && an0 == 0) return TY_INT_ARRAY;
      }
      return at;
    }
    if (!strcmp(name, "tally") && argc == 0) {
      if (rt == TY_INT_ARRAY) return TY_INT_INT_HASH;
      if (rt == TY_STR_ARRAY) return TY_STR_INT_HASH;
      if (rt == TY_POLY_ARRAY) return TY_SYM_POLY_HASH;
    }
    if (!strcmp(name, "group_by") && block >= 0 && ty_is_array(rt))
      return TY_POLY_POLY_HASH;
    if ((!strcmp(name, "first") || !strcmp(name, "last")) && argc == 1) return rt;  /* first(n)/last(n) -> subarray */
    if (!strcmp(name, "first") || !strcmp(name, "last") ||
        !strcmp(name, "min") || !strcmp(name, "max") ||
        !strcmp(name, "sample") ||
        !strcmp(name, "pop") || !strcmp(name, "shift")) return ty_array_elem(rt);
    if (!strcmp(name, "minmax")) return rt;  /* [min, max], same element kind */
    if (!strcmp(name, "join"))                        return TY_STRING;
    if (!strcmp(name, "pack") && argc == 1)           return TY_STRING;
    if (!strcmp(name, "inspect") || !strcmp(name, "to_s")) return TY_STRING;
    if (!strcmp(name, "empty?") || !strcmp(name, "include?")) return TY_BOOL;
    if ((!strcmp(name, "all?") || !strcmp(name, "any?") ||
         !strcmp(name, "none?") || !strcmp(name, "one?")) && argc == 0) return TY_BOOL;
    if ((!strcmp(name, "bsearch") || !strcmp(name, "find") || !strcmp(name, "detect")) && block >= 0)
      return ty_array_elem(rt);  /* element or nil */
    if ((!strcmp(name, "map!") || !strcmp(name, "collect!")) && block >= 0) {
      /* Typed arrays (int/str/float): in-place mutation preserves element type.
         The block param may be widened to TY_POLY when shared with other blocks,
         but the array type is determined by the receiver, not the block body. */
      if (ty_array_elem(rt) != TY_POLY)
        return rt;
      int body = nt_ref(nt, block, "body");
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      TyKind bt = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
      return bt != TY_UNKNOWN ? ty_array_of(bt) : rt;
    }
    if ((!strcmp(name, "select!") || !strcmp(name, "filter!") || !strcmp(name, "reject!") ||
         !strcmp(name, "keep_if") || !strcmp(name, "delete_if")) && block >= 0) return rt;
    if (!strcmp(name, "find_index") || !strcmp(name, "index")) return TY_INT;  /* int or nil */
    if (!strcmp(name, "each_index")) return rt;
    if ((!strcmp(name, "push") || !strcmp(name, "<<") || !strcmp(name, "append")) &&
        argc >= 1 && argv && rt != TY_POLY_ARRAY && ty_array_elem(rt) != TY_UNKNOWN) {
      /* Heterogeneous push on a typed-array literal: lift to poly. */
      TyKind elem_t = ty_array_elem(rt);
      const char *rty = nt_type(nt, recv);
      if (rty && !strcmp(rty, "ArrayNode")) {
        for (int ai = 0; ai < argc; ai++) {
          TyKind at = infer_type(c, argv[ai]);
          if (at != TY_UNKNOWN && at != elem_t) return TY_POLY_ARRAY;
        }
      }
      return rt;
    }
    if (!strcmp(name, "push") || !strcmp(name, "<<") || !strcmp(name, "append") ||
        !strcmp(name, "reverse") || !strcmp(name, "sort") || !strcmp(name, "uniq") ||
        !strcmp(name, "to_a") || !strcmp(name, "dup") || !strcmp(name, "clone") ||
        !strcmp(name, "compact") || !strcmp(name, "compact!") || !strcmp(name, "flatten") || !strcmp(name, "clear") ||
        !strcmp(name, "transpose") ||
        !strcmp(name, "shuffle") ||
        (!strcmp(name, "union") && argc == 0) ||
        !strcmp(name, "reverse!") || !strcmp(name, "sort!") || !strcmp(name, "shuffle!") ||
        !strcmp(name, "uniq!") ||
        !strcmp(name, "rotate!") || !strcmp(name, "insert") || !strcmp(name, "freeze") ||
        (!strcmp(name, "fill") && argc >= 1 && argc <= 3) ||
        !strcmp(name, "replace") ||
        !strcmp(name, "values_at")) return rt;
    if (!strcmp(name, "zip") && block < 0) return TY_POLY_ARRAY;
    if (!strcmp(name, "product") && argc == 1) return TY_POLY_ARRAY;
    if (!strcmp(name, "repeated_combination") && argc == 1) return TY_POLY_ARRAY;
    if (!strcmp(name, "frozen?")) return TY_BOOL;
    if ((!strcmp(name, "delete_at") || !strcmp(name, "delete")) && argc == 1)
      return ty_array_elem(rt);
    if (!strcmp(name, "shift") && argc == 0) return ty_array_elem(rt);
    if (!strcmp(name, "slice!") && argc == 2) return rt;  /* removed subarray */
    if (!strcmp(name, "[]=") && argc == 2)            return ty_array_elem(rt);
    if (!strcmp(name, "[]=") && argc == 3)            return infer_type(c, argv[2]);
    if ((!strcmp(name, "assoc") || !strcmp(name, "rassoc")) && rt == TY_POLY_ARRAY)
      return TY_POLY_ARRAY;  /* the matching sub-array, or nil (NULL ptr) */
    if (!strcmp(name, "to_h") && argc == 0 && block < 0) {
      /* Infer hash type from the first pair element of an array literal */
      if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ArrayNode")) {
        int en = 0; const int *els = nt_arr(nt, recv, "elements", &en);
        if (en > 0 && nt_type(nt, els[0]) && !strcmp(nt_type(nt, els[0]), "ArrayNode")) {
          int en2 = 0; const int *els2 = nt_arr(nt, els[0], "elements", &en2);
          if (en2 >= 2) {
            TyKind kt = infer_type(c, els2[0]);
            TyKind vt = infer_type(c, els2[1]);
            if (kt == TY_SYMBOL) return TY_SYM_POLY_HASH;
            if (kt == TY_STRING) {
              TyKind h = ty_hash_of(TY_STRING, vt);
              return h != TY_UNKNOWN ? h : TY_STR_POLY_HASH;
            }
            TyKind h = ty_hash_of(kt, vt);
            if (h != TY_UNKNOWN) return h;
          }
        }
      }
      return TY_SYM_POLY_HASH;
    }
  }

  /* exception receiver methods */
  if (recv >= 0 && rt == TY_EXCEPTION) {
    if (!strcmp(name, "message") || !strcmp(name, "to_s") ||
        !strcmp(name, "to_str") || !strcmp(name, "inspect") ||
        !strcmp(name, "full_message") || !strcmp(name, "class")) return TY_STRING;
    if (!strcmp(name, "backtrace")) return TY_STR_ARRAY;  /* empty: no frames captured */
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
      /* &. on a poly receiver may short-circuit to nil at runtime → always poly */
      {
        const char *call_op = nt_str(nt, id, "call_operator");
        if (recv >= 0 && call_op && !strcmp(call_op, "&.")) return TY_POLY;
      }
      if (!strcmp(name, "to_s") || !strcmp(name, "inspect")) return TY_STRING;
      if ((!strcmp(name, "gsub") || !strcmp(name, "sub")) && argc == 2) return TY_STRING;
      if (!strcmp(name, "join")) return TY_STRING;
      if (!strcmp(name, "to_i") || !strcmp(name, "length") || !strcmp(name, "size")) return TY_INT;
      if (!strcmp(name, "to_f")) return TY_FLOAT;
      if (!strcmp(name, "[]") && argc == 1) return TY_POLY;  /* boxed array element access */
      if (!strcmp(name, "[]") && argc == 2) return TY_POLY;  /* 2-arg poly slice */
      if (!strcmp(name, "dig") && argc >= 1) return TY_POLY;
      {
        int blk = nt_ref(nt, id, "block");
        if (blk >= 0 && (!strcmp(name, "map") || !strcmp(name, "collect"))) {
          int body = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
          TyKind et = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
          return et != TY_UNKNOWN ? ty_array_of(et) : TY_POLY_ARRAY;
        }
      }
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
      /* Fiber/Thread/IO/File instance methods: fallback when no user class defines `name`. */
      if (!strcmp(name, "resume") || !strcmp(name, "value") || !strcmp(name, "join"))
        return TY_POLY;
      if (!strcmp(name, "alive?") || !strcmp(name, "dead?") || !strcmp(name, "closed?") ||
          !strcmp(name, "eof?")) return TY_BOOL;
      if (!strcmp(name, "write") || !strcmp(name, "read") || !strcmp(name, "gets") ||
          !strcmp(name, "readline")) return TY_STRING;
      if (!strcmp(name, "close") || !strcmp(name, "flush")) return TY_NIL;
      if (!strcmp(name, "fileno")) return TY_INT;
      if (!strcmp(name, "synchronize")) {
        int blk_id = nt_ref(nt, id, "block");
        if (blk_id >= 0) {
          int bdy = nt_ref(nt, blk_id, "body");
          int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
          if (bbn > 0) return infer_type(c, bbb[bbn - 1]);
        }
        return TY_NIL;
      }
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
    /* a literal string range ("a".."z") yields strings, not ints */
    if (!strcmp(name, "to_a")) {
      int rn = recv;
      while (rn >= 0 && nt_type(nt, rn) && !strcmp(nt_type(nt, rn), "ParenthesesNode")) {
        int body = nt_ref(nt, rn, "body"); int bn = 0;
        const int *bd = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        rn = bn == 1 ? bd[0] : -1;
      }
      if (rn >= 0 && nt_type(nt, rn) && !strcmp(nt_type(nt, rn), "RangeNode")) {
        int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
        if (lo >= 0 && hi >= 0 && infer_type(c, lo) == TY_STRING && infer_type(c, hi) == TY_STRING)
          return TY_STR_ARRAY;
      }
    }
    if (!strcmp(name, "to_a") || !strcmp(name, "minmax")) return TY_INT_ARRAY;
    if (!strcmp(name, "include?") || !strcmp(name, "member?") ||
        !strcmp(name, "cover?") || !strcmp(name, "exclude_end?") ||
        !strcmp(name, "eql?") || !strcmp(name, "==") || !strcmp(name, "!=") ||
        !strcmp(name, "overlap?")) return TY_BOOL;
    if (!strcmp(name, "step")) return TY_INT_ARRAY;
    if (!strcmp(name, "all?") || !strcmp(name, "any?") ||
        !strcmp(name, "none?") || !strcmp(name, "one?")) return TY_BOOL;
    if (!strcmp(name, "each") && nt_ref(nt, id, "block") < 0) return TY_INT_ARRAY;
    if ((!strcmp(name, "first") || !strcmp(name, "last")) && argc == 1) return TY_INT_ARRAY;
    if (!strcmp(name, "sum") || !strcmp(name, "min") || !strcmp(name, "max") ||
        !strcmp(name, "first") || !strcmp(name, "last") ||
        !strcmp(name, "size") || !strcmp(name, "count") ||
        !strcmp(name, "begin") || !strcmp(name, "end"))  return TY_INT;
    if (!strcmp(name, "bsearch")) return TY_INT;  /* a member, or nil (nullable int) */
    int block = nt_ref(nt, id, "block");
    if (block >= 0 && (!strcmp(name, "map") || !strcmp(name, "collect"))) {
      int body = nt_ref(nt, block, "body");
      int bn = 0;
      const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      return ty_array_of(bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN);
    }
  }

  /* (range).lazy[.select/reject{blk}].first(n) / .first */
  if ((!strcmp(name, "first") || !strcmp(name, "last")) &&
      recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode")) {
    const char *rname = nt_str(nt, recv, "name");
    int lazy_src = -1;
    if (rname && !strcmp(rname, "lazy")) {
      lazy_src = nt_ref(nt, recv, "receiver");
    }
    else if (rname && (!strcmp(rname, "select") || !strcmp(rname, "reject") || !strcmp(rname, "filter"))) {
      int inner = nt_ref(nt, recv, "receiver");
      if (inner >= 0 && nt_type(nt, inner) && !strcmp(nt_type(nt, inner), "CallNode")) {
        const char *iname = nt_str(nt, inner, "name");
        if (iname && !strcmp(iname, "lazy")) lazy_src = nt_ref(nt, inner, "receiver");
      }
    }
    if (lazy_src >= 0 && infer_type(c, lazy_src) == TY_RANGE)
      return (argc == 1) ? TY_INT_ARRAY : TY_INT;
  }

  /* hash receiver methods */
  if (recv >= 0 && !strcmp(name, "default") && argc == 0 &&
      nt_type(nt, recv) && (!strcmp(nt_type(nt, recv), "HashNode") ||
                             !strcmp(nt_type(nt, recv), "KeywordHashNode"))) {
    return TY_POLY; /* {}.default -> nil (poly nil) */
  }
  /* fetch(key, default) on an unknown/empty hash: return the default type */
  if (recv >= 0 && !strcmp(name, "fetch") && argc >= 2 && !ty_is_hash(rt)) {
    TyKind dt = infer_type(c, argv[1]);
    if (dt != TY_UNKNOWN) return dt;
  }
  if (recv >= 0 && ty_is_hash(rt)) {
    if (!strcmp(name, "[]"))     return ty_hash_val(rt);
    if (!strcmp(name, "[]="))    return argc >= 2 ? ty_unify(infer_type(c, argv[1]), ty_hash_val(rt)) : ty_hash_val(rt);
    if (!strcmp(name, "fetch")) {
      TyKind vt = ty_hash_val(rt);
      if (argc == 2) {
        TyKind dt = infer_type(c, argv[1]);
        /* A hash literal default `{}` infers TY_UNKNOWN but is still a hash value
           — incompatible with a non-hash hash-val type like TY_INT. */
        if (dt == TY_UNKNOWN) {
          const char *atn = nt_type(nt, argv[1]);
          if (atn && (!strcmp(atn, "HashNode") || !strcmp(atn, "KeywordHashNode")))
            dt = TY_POLY_POLY_HASH;
        }
        if (ty_unify(vt, dt) == TY_POLY) return TY_POLY;
      }
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) {
        int bbody = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
        TyKind bvt = bn > 0 ? infer_type(c, bb[bn - 1]) : vt;
        if (bvt != vt) return TY_POLY;
      }
      return vt;
    }
    if (!strcmp(name, "delete")) return ty_hash_val(rt);
    if (!strcmp(name, "dig") && argc >= 1) {
      if (argc == 1) return ty_hash_val(rt);
      return TY_POLY;
    }
    if (!strcmp(name, "default") && argc == 0) return TY_POLY;
    if (!strcmp(name, "length") || !strcmp(name, "size") ||
        !strcmp(name, "count")) return TY_INT;
    if (!strcmp(name, "keys"))   return ty_array_of(ty_hash_key(rt));
    if (!strcmp(name, "values")) return ty_array_of(ty_hash_val(rt));
    if (!strcmp(name, "values_at") || !strcmp(name, "fetch_values")) return TY_POLY_ARRAY;
    {
      int block = nt_ref(nt, id, "block");
      if (block >= 0 && (!strcmp(name, "map") || !strcmp(name, "collect"))) {
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        return ty_array_of(bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN);
      }
      if (block >= 0 && (!strcmp(name, "select") || !strcmp(name, "filter") || !strcmp(name, "reject"))) return rt;
      if (block >= 0 && !strcmp(name, "transform_keys")) {
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        TyKind nkt = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
        TyKind r = ty_hash_of(nkt, ty_hash_val(rt));
        return r != TY_UNKNOWN ? r : rt;
      }
      if (block >= 0 && !strcmp(name, "transform_values")) {
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        TyKind nvt = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
        TyKind r = ty_hash_of(ty_hash_key(rt), nvt);
        return r != TY_UNKNOWN ? r : rt;
      }
    }
    if (!strcmp(name, "merge") && argc == 1) {
      TyKind at = argc >= 1 ? infer_type(c, argv[0]) : TY_UNKNOWN;
      if (at == rt) return rt;  /* same type: trivial */
      /* cross-variant str-keyed merge: promote to str_poly_hash */
      if (ty_hash_key(rt) == TY_STRING && ty_is_hash(at) && ty_hash_key(at) == TY_STRING)
        return TY_STR_POLY_HASH;
      /* cross-variant sym-keyed merge: both sym → sym_poly (only sym_poly exists) */
      if (ty_hash_key(rt) == TY_SYMBOL && ty_is_hash(at) && ty_hash_key(at) == TY_SYMBOL)
        return TY_SYM_POLY_HASH;
      return rt;
    }
    if (!strcmp(name, "dup") || !strcmp(name, "clone") || !strcmp(name, "replace") ||
        !strcmp(name, "merge")) return rt;
    if (!strcmp(name, "has_key?") || !strcmp(name, "key?") ||
        !strcmp(name, "include?") || !strcmp(name, "member?") ||
        !strcmp(name, "has_value?") || !strcmp(name, "value?") ||
        !strcmp(name, "empty?")) return TY_BOOL;
    if (!strcmp(name, "each_with_object") && argc > 0 && argv) {
      TyKind at = infer_type(c, argv[0]);
      if (at == TY_UNKNOWN) {
        const char *a0ty = nt_type(nt, argv[0]);
        int an0 = 0;
        if (a0ty && !strcmp(a0ty, "ArrayNode")) nt_arr(nt, argv[0], "elements", &an0);
        if (a0ty && !strcmp(a0ty, "ArrayNode") && an0 == 0) {
          /* When hash values are poly the block pushes poly values, so the
             accumulator widens to poly_array */
          return ty_hash_val(rt) == TY_POLY ? TY_POLY_ARRAY : TY_INT_ARRAY;
        }
      }
      return at;
    }
    if (!strcmp(name, "flatten") && argc <= 1) return TY_POLY_ARRAY;
    if (!strcmp(name, "invert") && argc == 0) {
      /* swap key/value types where we have a typed variant */
      if (rt == TY_STR_STR_HASH) return TY_STR_STR_HASH;
      return TY_POLY_POLY_HASH;
    }
    if ((!strcmp(name, "assoc") || !strcmp(name, "rassoc")) && argc == 1) return TY_POLY_ARRAY;
    if (!strcmp(name, "compact") && argc == 0) return rt;
  }

  /* <str>.encoding.name -> the encoding name string */
  if (!strcmp(name, "name") && argc == 0 && recv >= 0 &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "encoding"))
    return TY_STRING;

  /* string receiver methods */
  if (recv >= 0 && rt == TY_STRING) {
    if (!strcmp(name, "encoding") && argc == 0) return TY_POLY;  /* an Encoding value */
    if (!strcmp(name, "upcase") || !strcmp(name, "downcase") ||
        !strcmp(name, "capitalize") || !strcmp(name, "reverse") ||
        !strcmp(name, "strip") || !strcmp(name, "lstrip") ||
        !strcmp(name, "rstrip") || !strcmp(name, "chomp") ||
        !strcmp(name, "chop") || !strcmp(name, "chr") || !strcmp(name, "clamp") ||
        !strcmp(name, "squeeze") || !strcmp(name, "tr") || !strcmp(name, "tr_s") ||
        !strcmp(name, "succ") || !strcmp(name, "next") ||
        !strcmp(name, "delete")) return TY_STRING;
    if (!strcmp(name, "[]") || !strcmp(name, "slice") || !strcmp(name, "byteslice") ||
        !strcmp(name, "force_encoding") || !strcmp(name, "b") || !strcmp(name, "encode")) return TY_STRING;
    if ((!strcmp(name, "dump") || !strcmp(name, "undump")) && argc == 0) return TY_STRING;
    if (!strcmp(name, "index") && argc == 1) {
      const char *aty = nt_type(nt, argv[0]);
      if (aty && !strcmp(aty, "RegularExpressionNode")) return TY_POLY;  /* nil on no match */
    }
    if (!strcmp(name, "index") || !strcmp(name, "to_i") || !strcmp(name, "count") ||
        !strcmp(name, "oct") || !strcmp(name, "hex") || !strcmp(name, "ord") ||
        !strcmp(name, "casecmp") ||
        !strcmp(name, "bytesize") || !strcmp(name, "setbyte") || !strcmp(name, "getbyte")) return TY_INT;
    if (!strcmp(name, "scrub") || !strcmp(name, "crypt")) return TY_STRING;
    if (!strcmp(name, "rindex")) return TY_INT;
    if (!strcmp(name, "partition") || !strcmp(name, "rpartition")) return TY_STR_ARRAY;
    if (!strcmp(name, "casecmp?") || !strcmp(name, "ascii_only?") || !strcmp(name, "valid_encoding?")) return TY_BOOL;
    if (!strcmp(name, "to_f"))  return TY_FLOAT;
    if (!strcmp(name, "each_char") || !strcmp(name, "each_line") || !strcmp(name, "each_byte")) return TY_STRING;
    { int blk = nt_ref(nt, id, "block");
      if (blk >= 0 && (!strcmp(name, "chars") || !strcmp(name, "lines"))) return TY_STRING;
      if (blk >= 0 && (!strcmp(name, "bytes") || !strcmp(name, "codepoints"))) return TY_STRING; }
    if (!strcmp(name, "split") || !strcmp(name, "lines")) return TY_STR_ARRAY;
    if (!strcmp(name, "scan") && argc == 1) {
      /* scan with capture groups returns poly_array (array of arrays or strings) */
      const char *aty = nt_type(nt, argv[0]);
      if (aty && !strcmp(aty, "RegularExpressionNode")) {
        const char *src = nt_str(nt, argv[0], "unescaped");
        if (src && re_has_captures(src)) return TY_POLY_ARRAY;
      }
      return TY_STR_ARRAY;
    }
    if (!strcmp(name, "upto") && argc == 1) return TY_STR_ARRAY;  /* blockless: materialized sequence */
    if (!strcmp(name, "bytes") || !strcmp(name, "codepoints")) return TY_INT_ARRAY;
    if (!strcmp(name, "unpack") && argc == 1) return TY_POLY_ARRAY;
    if (!strcmp(name, "chars")) return TY_STR_ARRAY;
    if (!strcmp(name, "gsub") || !strcmp(name, "sub") || !strcmp(name, "tr") ||
        !strcmp(name, "center") || !strcmp(name, "ljust") || !strcmp(name, "rjust"))
      return TY_STRING;
    if (!strcmp(name, "*")) return TY_STRING;
    /* in-place append / concat reassign the receiver and evaluate to it */
    if ((!strcmp(name, "<<") || !strcmp(name, "concat") || !strcmp(name, "prepend")) && argc == 1)
      return TY_STRING;
  }
  /* <int_array>.product(<int_array>)[.to_a].inspect -> a string */
  if (!strcmp(name, "inspect") && argc == 0 && recv >= 0) {
    int pr = recv;
    if (nt_type(nt, pr) && !strcmp(nt_type(nt, pr), "CallNode") &&
        nt_str(nt, pr, "name") && !strcmp(nt_str(nt, pr, "name"), "to_a"))
      pr = nt_ref(nt, pr, "receiver");
    if (pr >= 0 && nt_type(nt, pr) && !strcmp(nt_type(nt, pr), "CallNode") && nt_str(nt, pr, "name") &&
        (!strcmp(nt_str(nt, pr, "name"), "product") || !strcmp(nt_str(nt, pr, "name"), "slice_before") ||
         !strcmp(nt_str(nt, pr, "name"), "slice_after") || !strcmp(nt_str(nt, pr, "name"), "slice_when") ||
         !strcmp(nt_str(nt, pr, "name"), "chunk")))
      return TY_STRING;
  }

  /* numeric.step(...) without a block materializes the sequence as an array */
  if (recv >= 0 && ty_is_numeric(rt) && !strcmp(name, "step") && nt_ref(nt, id, "block") < 0) {
    int args = nt_ref(nt, id, "arguments");
    int sc = 0; const int *sv = args >= 0 ? nt_arr(nt, args, "arguments", &sc) : NULL;
    int isf = (rt == TY_FLOAT) || (sc >= 1 && infer_type(c, sv[0]) == TY_FLOAT) ||
              (sc >= 2 && infer_type(c, sv[1]) == TY_FLOAT);
    return isf ? TY_FLOAT_ARRAY : TY_INT_ARRAY;
  }
  /* integer receiver methods */
  if (recv >= 0 && rt == TY_INT) {
    if (!strcmp(name, "ceil") || !strcmp(name, "floor") ||
        !strcmp(name, "round") || !strcmp(name, "truncate")) return TY_INT;  /* no precision arg -> self */
    if (!strcmp(name, "divmod") && argc == 1) return TY_INT_ARRAY;  /* [quotient, remainder] */
    if ((!strcmp(name, "allbits?") || !strcmp(name, "anybits?") || !strcmp(name, "nobits?")) && argc == 1) return TY_BOOL;
    if (!strcmp(name, "even?") || !strcmp(name, "odd?") || !strcmp(name, "zero?") ||
        !strcmp(name, "positive?") || !strcmp(name, "negative?")) return TY_BOOL;
    if ((!strcmp(name, "ceildiv") || !strcmp(name, "pow")) && argc >= 1) return TY_INT;
    if ((!strcmp(name, "pred") || !strcmp(name, "succ") || !strcmp(name, "next")) && argc == 0) return TY_INT;
    if (!strcmp(name, "nonzero?") && argc == 0) return TY_INT;  /* self or nil (nullable int) */
    /* times/upto/downto/step with a block return the receiver (self) */
    if ((!strcmp(name, "times") || !strcmp(name, "upto") || !strcmp(name, "downto") ||
         !strcmp(name, "step")) && nt_ref(nt, id, "block") >= 0) return TY_INT;
    /* times/upto/downto without a block return a range-like enumerator */
    if ((!strcmp(name, "times") || !strcmp(name, "upto") || !strcmp(name, "downto")) &&
        nt_ref(nt, id, "block") < 0) return TY_RANGE;
    if (!strcmp(name, "chr")) return TY_STRING;
    if (!strcmp(name, "[]") && argc == 1) return TY_INT;  /* bit access */
    if (!strcmp(name, "div") && argc == 1) return TY_INT;  /* floor division */
    if (!strcmp(name, "gcd") || !strcmp(name, "lcm") || !strcmp(name, "clamp")) return TY_INT;
    if (!strcmp(name, "magnitude") && argc == 0) return TY_INT;  /* alias for abs */
    if ((!strcmp(name, "modulo") || !strcmp(name, "remainder")) && argc == 1) return TY_INT;
    if (!strcmp(name, "gcdlcm") && argc == 1) return TY_INT_ARRAY;  /* [gcd, lcm] */
    if (!strcmp(name, "digits")) return TY_INT_ARRAY;
    if (!strcmp(name, "to_s") && argc == 1) return TY_STRING;
    if (!strcmp(name, "coerce") && argc == 1) {
      TyKind a0 = infer_type(c, argv[0]);
      return (a0 == TY_FLOAT) ? TY_FLOAT_ARRAY : TY_INT_ARRAY;
    }
  }
  /* float receiver methods */
  if (recv >= 0 && rt == TY_FLOAT) {
    if (!strcmp(name, "coerce") && argc == 1) return TY_FLOAT_ARRAY;  /* [Float(other), self] */
    if (!strcmp(name, "divmod") && argc == 1) return TY_POLY_ARRAY;  /* [Integer, Float] */
    if (!strcmp(name, "infinite?")) return TY_INT;   /* nil / 1 / -1 (nullable int) */
    if (!strcmp(name, "nan?") || !strcmp(name, "finite?") ||
        !strcmp(name, "positive?") || !strcmp(name, "negative?") ||
        !strcmp(name, "zero?")) return TY_BOOL;
    if (!strcmp(name, "next_float") || !strcmp(name, "prev_float") ||
        !strcmp(name, "abs") || !strcmp(name, "magnitude") ||
        !strcmp(name, "modulo") || !strcmp(name, "to_f")) return TY_FLOAT;
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

  /* /re/ === str -> match boolean */
  if (!strcmp(name, "===") && argc == 1 && recv >= 0 &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "RegularExpressionNode"))
    return TY_BOOL;
  /* Class.===(obj) is always bool */
  if (!strcmp(name, "===") && argc == 1 && recv >= 0 &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode"))
    return TY_BOOL;

  if ((!strcmp(name, "-@") || !strcmp(name, "+@")) && recv >= 0 && argc == 0)
    return ty_is_numeric(rt) ? rt : rt == TY_POLY ? TY_POLY : TY_UNKNOWN;
  if (!strcmp(name, "!")) return TY_BOOL;
  if (!strcmp(name, "respond_to?") && recv >= 0) return TY_BOOL;
  if ((!strcmp(name, "method_defined?") || !strcmp(name, "const_defined?")) && recv >= 0) return TY_BOOL;
  if (!strcmp(name, "nil?") && recv >= 0 && argc == 0) return TY_BOOL;
  if (!strcmp(name, "object_id") && recv >= 0 && argc == 0) return TY_INT;
  if (!strcmp(name, "between?") && argc == 2 && (rt == TY_STRING || ty_is_numeric(rt))) return TY_BOOL;
  if ((!strcmp(name, "match?") || !strcmp(name, "!~")) && recv >= 0) return TY_BOOL;
  if (!strcmp(name, "match") && recv >= 0 && (argc == 1 || argc == 2)) {
    const char *rrt = nt_type(nt, recv), *art = argc > 0 ? nt_type(nt, argv[0]) : NULL;
    if ((rrt && !strcmp(rrt, "RegularExpressionNode")) ||
        (art && !strcmp(art, "RegularExpressionNode"))) return TY_MATCHDATA;
  }
  if (!strcmp(name, "=~") && recv >= 0 && argc == 1) {
    const char *rrt = nt_type(nt, recv), *art = nt_type(nt, argv[0]);
    TyKind a0t = argc > 0 ? infer_type(c, argv[0]) : TY_UNKNOWN;
    if ((rrt && !strcmp(rrt, "RegularExpressionNode")) ||
        (art && !strcmp(art, "RegularExpressionNode")) ||
        rt == TY_REGEX || a0t == TY_REGEX) return TY_POLY;
  }
  if (!strcmp(name, "match") && recv >= 0 && (argc == 1 || argc == 2)) {
    TyKind a0t = argc > 0 ? infer_type(c, argv[0]) : TY_UNKNOWN;
    if (rt == TY_REGEX || a0t == TY_REGEX) return TY_MATCHDATA;
  }
  /* /re/.source -> String, /re/.options -> Integer (compile-time constants) */
  if (recv >= 0 && argc == 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "RegularExpressionNode")) {
    if (!strcmp(name, "source")) return TY_STRING;
    if (!strcmp(name, "options")) return TY_INT;
  }

  /* array set operations: &, intersection, |, union(1-arg), -, difference */
  if (recv >= 0 && argc == 1 &&
      (!strcmp(name, "&") || !strcmp(name, "intersection") ||
       !strcmp(name, "|") || !strcmp(name, "union") ||
       !strcmp(name, "-") || !strcmp(name, "difference"))) {
    if (ty_is_array(rt) && a0 == rt) return rt;
    if (ty_is_array(rt) && a0 == TY_POLY_ARRAY) return rt;
    if (ty_is_array(a0) && rt == TY_POLY_ARRAY) return a0;
    /* empty array [] arg (TY_UNKNOWN): result is same kind as receiver */
    if (ty_is_array(rt) && a0 == TY_UNKNOWN) return rt;
  }
  if (recv >= 0 && argc == 1 && is_arith_op(name)) {
    if (rt == TY_STRING) {
      if (!strcmp(name, "%")) return TY_STRING;  /* sprintf (array or single value) */
      if (!strcmp(name, "+") || !strcmp(name, "*")) {
        if (a0 == TY_POLY) return TY_POLY;  /* codegen uses sp_poly_add for poly operand */
        return TY_STRING;
      }
      return TY_UNKNOWN;
    }
    /* array + same-kind -> same kind; different-kind -> poly_array */
    if (!strcmp(name, "+") && ty_is_array(rt) && a0 == rt) return rt;
    if (!strcmp(name, "+") && ty_is_array(rt) && ty_is_array(a0) && a0 != rt) return TY_POLY_ARRAY;
    /* array * int -> same array type (repeat); array * string -> join string */
    if (!strcmp(name, "*") && (ty_is_array(rt) || rt == TY_POLY_ARRAY) && a0 == TY_INT) return rt;
    if (!strcmp(name, "*") && (ty_is_array(rt) || rt == TY_POLY_ARRAY) && a0 == TY_STRING) return TY_STRING;
    if (ty_is_numeric(rt) && ty_is_numeric(a0)) {
      if (rt == TY_FLOAT || a0 == TY_FLOAT) return TY_FLOAT;
      if (rt == TY_BIGINT || a0 == TY_BIGINT) return TY_BIGINT;
      return TY_INT;
    }
    return TY_UNKNOWN;
  }
  if (recv >= 0 && argc == 1 && !strcmp(name, "<=>")) return TY_INT;
  if (recv >= 0 && argc == 1 && is_cmp_op(name)) return TY_BOOL;
  if (argc == 1 && is_eq_op(name)) return TY_BOOL;

  /* integer bitwise operators */
  if (recv >= 0 && argc == 1 && rt == TY_INT &&
      (!strcmp(name, "&") || !strcmp(name, "|") || !strcmp(name, "^") ||
       !strcmp(name, "<<") || !strcmp(name, ">>")))
    return TY_INT;
  /* poly recv bitwise shift: result is int (sp_poly_to_i applied) */
  if (recv >= 0 && argc == 1 && rt == TY_POLY &&
      (!strcmp(name, ">>") || !strcmp(name, "&") || !strcmp(name, "|") || !strcmp(name, "^")))
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

  /* tap: run block, return self */
  if (!strcmp(name, "tap") && recv >= 0) return rt;
  /* then / yield_self: run block, return block result */
  if (!strcmp(name, "then") || !strcmp(name, "yield_self")) {
    int blk_id = nt_ref(nt, id, "block");
    if (blk_id >= 0) {
      int bdy = nt_ref(nt, blk_id, "body");
      int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
      if (bbn <= 0) return TY_NIL;
      /* Pin block param to receiver type so body inference uses the right type */
      const char *bp0 = block_param_name(c, blk_id, 0);
      Scope *bs = bp0 ? comp_scope_of(c, blk_id) : NULL;
      LocalVar *blv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
      TyKind saved_blv = blv ? blv->type : TY_UNKNOWN;
      if (blv && rt != TY_UNKNOWN) blv->type = rt;
      TyKind result = infer_type(c, bbb[bbn - 1]);
      if (blv) blv->type = saved_blv;
      return result;
    }
  }
  if (!strcmp(name, "instance_eval")) {
    int blk_id = nt_ref(nt, id, "block");
    if (blk_id >= 0 && ty_is_object(rt) &&
        comp_method_in_chain(c, ty_object_class(rt), "instance_eval", NULL) < 0) {
      int bdy = nt_ref(nt, blk_id, "body");
      int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
      if (bbn <= 0) return TY_NIL;
      int saved_ie = g_ie_class_id;
      g_ie_class_id = ty_object_class(rt);
      TyKind result = infer_type(c, bbb[bbn - 1]);
      g_ie_class_id = saved_ie;
      return result;
    }
    return TY_POLY;
  }

  /* safe navigation &. with unresolved type: return poly (receiver may be nil at runtime) */
  {
    const char *call_op = nt_str(nt, id, "call_operator");
    if (recv >= 0 && call_op && !strcmp(call_op, "&.")) return TY_POLY;
  }

  /* Builtin class reopening: look up user-defined methods on Array/Numeric/Object
     receivers where no builtin method matched. */
  if (recv >= 0) {
    /* Array reopening: any array-typed receiver */
    if (ty_is_array(rt)) {
      int oc_ci = comp_class_index(c, "Array");
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return c->scopes[oc_mi].ret;
      }
    }
    /* Numeric reopening: integers and floats */
    if (rt == TY_INT || rt == TY_FLOAT) {
      int oc_ci = comp_class_index(c, "Numeric");
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return c->scopes[oc_mi].ret;
      }
    }
    /* FalseClass methods (TrueClass already checked earlier for TY_BOOL) */
    if (rt == TY_BOOL) {
      int oc_ci = comp_class_index(c, "FalseClass");
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return c->scopes[oc_mi].ret;
      }
    }
    /* Object reopening: universal fallback for any receiver type */
    {
      int oc_ci = comp_class_index(c, "Object");
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return c->scopes[oc_mi].ret;
      }
    }
  }

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
  if (!strcmp(ty, "SourceFileNode"))          return TY_STRING;
  if (!strcmp(ty, "SourceLineNode"))          return TY_INT;
  if (!strcmp(ty, "SourceEncodingNode"))      return TY_POLY;
  if (!strcmp(ty, "RegularExpressionNode") ||
      !strcmp(ty, "InterpolatedRegularExpressionNode")) return TY_REGEX;
  if (!strcmp(ty, "InterpolatedStringNode"))  return TY_STRING;
  if (!strcmp(ty, "XStringNode") || !strcmp(ty, "InterpolatedXStringNode")) return TY_STRING;
  if (!strcmp(ty, "InterpolatedSymbolNode"))  return TY_SYMBOL;
  if (!strcmp(ty, "SymbolNode"))              return TY_SYMBOL;
  if (!strcmp(ty, "TrueNode"))                return TY_BOOL;
  if (!strcmp(ty, "FalseNode"))               return TY_BOOL;
  if (!strcmp(ty, "NilNode"))                 return TY_NIL;
  if (!strcmp(ty, "RangeNode")) {
    /* infer the bounds so codegen can tell an int range from a string range */
    int lo = nt_ref(nt, id, "left"), hi = nt_ref(nt, id, "right");
    if (lo >= 0) infer_type(c, lo);
    if (hi >= 0) infer_type(c, hi);
    return TY_RANGE;
  }
  if (!strcmp(ty, "LambdaNode"))              return TY_PROC;
  /* an assignment expression evaluates to the assigned value */
  if (!strcmp(ty, "LocalVariableWriteNode"))  return infer_type(c, nt_ref(nt, id, "value"));
  if (!strcmp(ty, "InstanceVariableWriteNode") ||
      !strcmp(ty, "InstanceVariableOrWriteNode") ||
      !strcmp(ty, "InstanceVariableAndWriteNode") ||
      !strcmp(ty, "InstanceVariableOperatorWriteNode")) {
    /* expression evaluates to the ivar slot's type (same as a read) */
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    if (s->class_id < 0) return infer_type(c, nt_ref(nt, id, "value"));
    ClassInfo *ci = &c->classes[s->class_id];
    int iv = nm ? comp_ivar_index(ci, nm) : -1;
    return iv >= 0 ? ci->ivar_types[iv] : TY_UNKNOWN;
  }
  if (!strcmp(ty, "LocalVariableOperatorWriteNode")) {
    const char *nm2 = nt_str(nt, id, "name");
    Scope *s2 = comp_scope_of(c, id);
    LocalVar *lv2 = nm2 ? scope_local(s2, nm2) : NULL;
    TyKind ct2 = lv2 ? lv2->type : TY_UNKNOWN;
    TyKind vt2 = infer_type(c, nt_ref(nt, id, "value"));
    if (ct2 == TY_STRING) return TY_STRING;
    if (ty_is_numeric(ct2) && ty_is_numeric(vt2))
      return (ct2 == TY_FLOAT || vt2 == TY_FLOAT) ? TY_FLOAT : TY_INT;
    return ct2 != TY_UNKNOWN ? ct2 : vt2;
  }
  if (!strcmp(ty, "LocalVariableOrWriteNode") || !strcmp(ty, "LocalVariableAndWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    LocalVar *lv = nm ? scope_local(s, nm) : NULL;
    TyKind ct = lv ? lv->type : TY_UNKNOWN;
    return ty_unify(ct, infer_type(c, nt_ref(nt, id, "value")));
  }

  if (!strcmp(ty, "LocalVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    /* &block param that escapes (not yield-inlined): the LocalVar slot type is
       TY_UNKNOWN, but the value is a Proc object when the method does not inline
       the block (yields==0). Return TY_PROC so callers can type the return value. */
    if (nm && s && s->blk_param && s->blk_param[0] && !strcmp(nm, s->blk_param)
        && !s->yields)
      return TY_PROC;
    LocalVar *lv = nm ? scope_local(s, nm) : NULL;
    if (lv) return lv->type;
    return TY_UNKNOWN;
  }
  if (!strcmp(ty, "GlobalVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    /* predefined punctuation globals: $/ defaults to "\n"; $! / $; / $, read nil */
    if (nm && !strcmp(nm, "$/")) return TY_STRING;
    if (nm && !strcmp(nm, "$?")) return TY_INT;  /* last child exit status */
    if (nm && (!strcmp(nm, "$PROGRAM_NAME") || !strcmp(nm, "$0"))) return TY_STRING;
    if (nm && (!strcmp(nm, "$!") || !strcmp(nm, "$;") || !strcmp(nm, "$,"))) return TY_NIL;
    /* regex match globals: nullable strings ($~ == $&, $`, $', $+) */
    if (nm && (!strcmp(nm, "$~") || !strcmp(nm, "$&") || !strcmp(nm, "$`") ||
               !strcmp(nm, "$'") || !strcmp(nm, "$+"))) return TY_STRING;
    const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
    LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
    return lv ? lv->type : TY_UNKNOWN;
  }
  if (!strcmp(ty, "ConstantReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? comp_const(c, nm) : NULL;
    if (lv) return lv->type;
    if (nm && (!strcmp(nm, "RUBY_DESCRIPTION") || !strcmp(nm, "RUBY_VERSION") ||
               !strcmp(nm, "RUBY_PLATFORM") || !strcmp(nm, "RUBY_ENGINE") ||
               !strcmp(nm, "RUBY_ENGINE_VERSION") || !strcmp(nm, "RUBY_RELEASE_DATE") ||
               !strcmp(nm, "RUBY_REVISION") || !strcmp(nm, "RUBY_COPYRIGHT"))) return TY_STRING;
    if (nm && !strcmp(nm, "ARGV")) return TY_STR_ARRAY;
    if (nm && comp_class_index(c, nm) >= 0) return TY_CLASS;
    if (nm && is_builtin_class_name(nm)) return TY_CLASS;
    return TY_UNKNOWN;
  }
  if (!strcmp(ty, "DefinedNode")) return TY_STRING;  /* a label string, or nil (NULL) */
  if (!strcmp(ty, "NumberedReferenceReadNode")) return TY_STRING;  /* $1..$9: capture, or nil (NULL) */
  if (!strcmp(ty, "BackReferenceReadNode")) return TY_STRING;  /* $&/$`/$'/$~/$+: nullable string */
  if (!strcmp(ty, "ConstantPathNode")) {
    /* M::CONST -> resolve by the final path component (constants register
       under their unqualified name) */
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? comp_const(c, nm) : NULL;
    if (lv) return lv->type;
    if (nm && !strcmp(nm, "ARGV")) return TY_STR_ARRAY;
    /* well-known module constants */
    int par_id = nt_ref(nt, id, "parent");
    const char *par_ty = par_id >= 0 ? nt_type(nt, par_id) : NULL;
    const char *par_nm = (par_ty && !strcmp(par_ty, "ConstantReadNode")) ? nt_str(nt, par_id, "name") : NULL;
    if (par_nm && !strcmp(par_nm, "Float")) {
      if (nm && (!strcmp(nm, "MAX") || !strcmp(nm, "MIN") || !strcmp(nm, "EPSILON") ||
                 !strcmp(nm, "INFINITY") || !strcmp(nm, "NAN") || !strcmp(nm, "DIG") ||
                 !strcmp(nm, "MANT_DIG") || !strcmp(nm, "RADIX"))) return TY_FLOAT;
    }
    if (par_nm && !strcmp(par_nm, "Math")) {
      if (nm && (!strcmp(nm, "PI") || !strcmp(nm, "E"))) return TY_FLOAT;
    }
    if (par_nm && !strcmp(par_nm, "File")) {
      if (nm && (!strcmp(nm, "SEPARATOR") || !strcmp(nm, "PATH_SEPARATOR") ||
                 !strcmp(nm, "ALT_SEPARATOR"))) return TY_STRING;
    }
    if (par_nm && !strcmp(par_nm, "Integer")) {
      if (nm && (!strcmp(nm, "MAX") || !strcmp(nm, "MIN"))) return TY_UNKNOWN; /* raises NameError */
    }
    if (nm && comp_class_index(c, nm) >= 0) return TY_CLASS;
    if (nm && is_builtin_class_name(nm)) return TY_CLASS;
    /* FFI const: Module::NAME -> int */
    if (par_nm && nm) {
      for (int fci = 0; fci < c->n_ffi_consts; fci++) {
        if (!strcmp(c->ffi_const_mods[fci], par_nm) &&
            !strcmp(c->ffi_const_names[fci], nm))
          return TY_INT;
      }
    }
    return TY_UNKNOWN;
  }
  if (!strcmp(ty, "SelfNode")) {
    Scope *s = comp_scope_of(c, id);
    int self_cls = s->class_id;
    /* `self` inside an instance_eval/exec block is the rebound receiver. */
    if (self_cls < 0) self_cls = (g_ie_class_id >= 0) ? g_ie_class_id : ie_class_of(c, id);
    if (self_cls < 0) return TY_UNKNOWN;
    const char *cn = c->classes[self_cls].name;
    if (!strcmp(cn, "String"))  return TY_STRING;
    if (!strcmp(cn, "Integer")) return TY_INT;
    if (!strcmp(cn, "Float"))   return TY_FLOAT;
    if (!strcmp(cn, "Symbol"))  return TY_SYMBOL;
    if (!strcmp(cn, "TrueClass") || !strcmp(cn, "FalseClass") || !strcmp(cn, "NilClass")) return TY_BOOL;
    if (!strcmp(cn, "Array"))   return TY_POLY_ARRAY;
    if (!strcmp(cn, "Object"))  return TY_POLY;  /* dynamic: called on any receiver type */
    return ty_object(self_cls);
  }
  if (!strcmp(ty, "InstanceVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    int cls_id = (s->class_id >= 0) ? s->class_id : g_ie_class_id;
    if (cls_id < 0) cls_id = ie_class_of(c, id);
    if (cls_id < 0) cls_id = comp_class_index(c, "Toplevel");
    if (cls_id < 0) return TY_UNKNOWN;
    ClassInfo *ci = &c->classes[cls_id];
    int iv = nm ? comp_ivar_index(ci, nm) : -1;
    return iv >= 0 ? ci->ivar_types[iv] : TY_UNKNOWN;
  }
  if (!strcmp(ty, "ClassVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    int cid = s->class_id;
    if (cid < 0) cid = comp_class_index(c, "Toplevel");
    if (cid < 0) return TY_UNKNOWN;
    int idx = nm ? comp_cvar_index(&c->classes[cid], nm) : -1;
    return idx >= 0 ? c->classes[cid].cvar_types[idx] : TY_UNKNOWN;
  }
  if (!strcmp(ty, "ClassVariableWriteNode") ||
      !strcmp(ty, "ClassVariableOperatorWriteNode") ||
      !strcmp(ty, "ClassVariableOrWriteNode") ||
      !strcmp(ty, "ClassVariableAndWriteNode"))
    return infer_type(c, nt_ref(nt, id, "value"));
  if (!strcmp(ty, "IndexOrWriteNode") || !strcmp(ty, "IndexAndWriteNode")) {
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) return TY_UNKNOWN;
    TyKind rt = infer_type(c, recv);
    if (ty_is_array(rt)) return ty_array_elem(rt);
    if (ty_is_hash(rt)) return ty_hash_val(rt);
    return TY_POLY;
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
  if (!strcmp(ty, "CaseNode")) {
    /* value = unify of each when's body; a missing else means a no-match
       falls through to nil */
    int nw = 0; const int *whens = nt_arr(nt, id, "conditions", &nw);
    int else_c = nt_ref(nt, id, "else_clause");
    TyKind r = TY_UNKNOWN;
    for (int w = 0; w < nw; w++) {
      int st = nt_ref(nt, whens[w], "statements");
      r = ty_unify(r, st >= 0 ? infer_type(c, st) : TY_NIL);
    }
    if (else_c >= 0) { int st = nt_ref(nt, else_c, "statements"); r = ty_unify(r, st >= 0 ? infer_type(c, st) : TY_NIL); }
    else r = ty_unify(r, TY_NIL);
    return r;
  }
  if (!strcmp(ty, "IfNode") || !strcmp(ty, "UnlessNode")) {
    int is_unless = !strcmp(ty, "UnlessNode");
    int then_b = nt_ref(nt, id, "statements");
    int else_b = nt_ref(nt, id, is_unless ? "else_clause" : "subsequent");
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
  if (!strcmp(ty, "HashNode") || !strcmp(ty, "KeywordHashNode")) {
    int n = 0;
    const int *els = nt_arr(nt, id, "elements", &n);
    if (n == 0) return TY_UNKNOWN;
    TyKind kt = TY_UNKNOWN, vt = TY_UNKNOWN;
    for (int k = 0; k < n; k++) {
      const char *aty = nt_type(nt, els[k]);
      if (!aty || strcmp(aty, "AssocNode")) return TY_UNKNOWN;
      kt = ty_unify(kt, infer_type(c, nt_ref(nt, els[k], "key")));
      int vnode = nt_ref(nt, els[k], "value");
      TyKind vt_elem = infer_type(c, vnode);
      /* A nested hash literal (even empty `{}`) is a non-scalar value; treat
         it as poly so the outer hash promotes to a poly-valued variant. */
      if (vt_elem == TY_UNKNOWN) {
        const char *vnode_ty = nt_type(nt, vnode);
        if (vnode_ty && (!strcmp(vnode_ty, "HashNode") || !strcmp(vnode_ty, "KeywordHashNode")))
          vt_elem = TY_POLY;
      }
      vt = ty_unify(vt, vt_elem);
    }
    /* symbol keys -> SymPolyHash (boxed values), regardless of value type */
    if (kt == TY_SYMBOL) return TY_SYM_POLY_HASH;
    TyKind hv = ty_hash_of(kt, vt);
    /* unsupported combination -> fall back to poly storage */
    if (hv == TY_UNKNOWN && vt != TY_UNKNOWN) return TY_POLY_POLY_HASH;
    return hv;
  }
  if (!strcmp(ty, "YieldNode"))
    return yield_value_type(c, (int)(comp_scope_of(c, id) - c->scopes));
  if (!strcmp(ty, "SuperNode") || !strcmp(ty, "ForwardingSuperNode")) {
    Scope *s = comp_scope_of(c, id);
    if (s->class_id < 0 || !s->name) return TY_UNKNOWN;
    const char *shadow = comp_prep_chain_target(c, s->class_id, s->name);
    if (shadow) {
      int mi = comp_method_in_class(c, s->class_id, shadow);
      return mi >= 0 ? c->scopes[mi].ret : TY_UNKNOWN;
    }
    const char *uname = comp_prep_user_name(s->name);
    int p = c->classes[s->class_id].parent;
    if (p < 0) return TY_UNKNOWN;
    int mi = comp_method_in_chain(c, p, uname, NULL);
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

  if (!strcmp(ty, "RescueModifierNode")) {
    int e = nt_ref(nt, id, "expression");
    int r = nt_ref(nt, id, "rescue_expression");
    TyKind et = e >= 0 ? infer_type(c, e) : TY_NIL;
    TyKind rt = r >= 0 ? infer_type(c, r) : TY_NIL;
    /* a diverging expression like raise has no real type; use the rescue arm's type */
    if (et == TY_UNKNOWN || et == TY_VOID || et == TY_NIL) return rt;
    return ty_unify(et, rt);
  }

  /* MultiWriteNode as expression: value is the RHS array. */
  if (!strcmp(ty, "MultiWriteNode"))
    return infer_type(c, nt_ref(nt, id, "value"));

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

/* Collect parameters from a DefNode into scope s. */
static void collect_def_params(Compiler *c, int def_id, Scope *s) {
  int pn = nt_ref(c->nt, def_id, "parameters");
  if (pn < 0) return;
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
  int rp = nt_ref(c->nt, pn, "rest");
  if (rp >= 0) {
    const char *rpty = nt_type(c->nt, rp);
    if (rpty && !strcmp(rpty, "RestParameterNode")) {
      const char *rname = nt_str(c->nt, rp, "name");
      if (rname) {
        if (s->nparams % 8 == 0) {
          s->pnames  = realloc(s->pnames,  sizeof(char *) * (size_t)(s->nparams + 8));
          s->pdefault = realloc(s->pdefault, sizeof(int)    * (size_t)(s->nparams + 8));
        }
        s->pdefault[s->nparams] = -1;
        s->pnames[s->nparams++] = strdup(rname);
        LocalVar *lv = scope_local_intern(s, rname);
        lv->is_param = 1;
        lv->type = TY_POLY_ARRAY;
        s->rest_idx = s->nparams - 1;
      }
    }
  }
  /* post-splat required parameters (Prism "posts" array) */
  int postn = 0;
  const int *posts = nt_arr(c->nt, pn, "posts", &postn);
  for (int i = 0; i < postn; i++) {
    const char *pname = nt_str(c->nt, posts[i], "name");
    if (pname) scope_add_param(s, pname, -1);
  }
  if (postn > 0) s->npost_rest = postn;
  int kn = 0;
  const int *kws = nt_arr(c->nt, pn, "keywords", &kn);
  for (int i = 0; i < kn; i++) {
    const char *pty = nt_type(c->nt, kws[i]);
    if (!pty) continue;
    const char *pname = nt_str(c->nt, kws[i], "name");
    int dv = !strcmp(pty, "OptionalKeywordParameterNode") ? nt_ref(c->nt, kws[i], "value") : -1;
    if (pname) scope_add_param(s, pname, dv);
  }
  int kwrp = nt_ref(c->nt, pn, "keyword_rest");
  if (kwrp >= 0) {
    const char *kwrpty = nt_type(c->nt, kwrp);
    if (kwrpty && !strcmp(kwrpty, "KeywordRestParameterNode")) {
      const char *kwrname = nt_str(c->nt, kwrp, "name");
      if (kwrname) {
        LocalVar *lv = scope_local_intern(s, kwrname);
        lv->is_param = 1;
        lv->type = TY_SYM_POLY_HASH;
        if (s->nparams % 8 == 0) {
          s->pnames   = realloc(s->pnames,   sizeof(char *) * (size_t)(s->nparams + 8));
          s->pdefault = realloc(s->pdefault, sizeof(int)    * (size_t)(s->nparams + 8));
        }
        s->pdefault[s->nparams] = -1;
        s->pnames[s->nparams++] = strdup(kwrname);
        s->kwrest_idx = s->nparams - 1;
      }
    }
  }
  int bp = nt_ref(c->nt, pn, "block");
  if (bp >= 0 && nt_type(c->nt, bp) && !strcmp(nt_type(c->nt, bp), "BlockParameterNode")) {
    const char *bn = nt_str(c->nt, bp, "name");
    s->blk_param = strdup(bn ? bn : "");
    /* Register the &block param as a local so mark_proc_captures can find it
       and mark it is_cell when a nested proc body captures it. */
    if (bn && bn[0]) {
      LocalVar *blv = scope_local_intern(s, bn);
      blv->is_param = 1;
      blv->type = TY_PROC;
    }
  }
}

static void walk_scope(Compiler *c, int id, int scope_idx, int class_id);

/* String form of an int/string/symbol literal node, for compile-time
   `define_method` name interpolation. Returns malloc'd, or NULL. */
static char *dm_lit_str(Compiler *c, int lit) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, lit);
  if (!ty) return NULL;
  if (!strcmp(ty, "IntegerNode")) {
    char buf[32]; snprintf(buf, sizeof buf, "%lld", (long long)nt_int(nt, lit, "value", 0));
    return strdup(buf);
  }
  if (!strcmp(ty, "StringNode")) {
    const char *s = nt_str(nt, lit, "content");
    if (!s) s = nt_str(nt, lit, "unescaped");
    return s ? strdup(s) : NULL;
  }
  if (!strcmp(ty, "SymbolNode")) { const char *s = nt_str(nt, lit, "value"); return s ? strdup(s) : NULL; }
  return NULL;
}

/* Evaluate a `define_method(<name-expr>)` name with the each-loop variable
   `bv` bound to literal `lit`. Handles string/symbol literals, a bare loop
   variable, and (interpolated) string/symbol nodes. Returns malloc'd name
   or NULL when not statically resolvable. */
static char *dm_eval_name(Compiler *c, int node, const char *bv, int lit) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  if (!ty) return NULL;
  if (!strcmp(ty, "StringNode")) {
    const char *s = nt_str(nt, node, "content");
    if (!s) s = nt_str(nt, node, "unescaped");
    return s ? strdup(s) : NULL;
  }
  if (!strcmp(ty, "SymbolNode")) { const char *s = nt_str(nt, node, "value"); return s ? strdup(s) : NULL; }
  if (!strcmp(ty, "LocalVariableReadNode")) {
    const char *nm = nt_str(nt, node, "name");
    if (nm && bv && !strcmp(nm, bv)) return dm_lit_str(c, lit);
    return NULL;
  }
  if (!strcmp(ty, "EmbeddedStatementsNode")) {
    int body = nt_ref(nt, node, "statements");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    if (bn != 1) return NULL;
    return dm_eval_name(c, bb[0], bv, lit);
  }
  if (!strcmp(ty, "InterpolatedStringNode") || !strcmp(ty, "InterpolatedSymbolNode")) {
    int pn = 0; const int *parts = nt_arr(nt, node, "parts", &pn);
    char *out = strdup("");
    for (int k = 0; k < pn; k++) {
      char *p = dm_eval_name(c, parts[k], bv, lit);
      if (!p) { free(out); return NULL; }
      size_t no = strlen(out) + strlen(p) + 1;
      char *merged = malloc(no); snprintf(merged, no, "%s%s", out, p);
      free(out); free(p); out = merged;
    }
    return out;
  }
  return NULL;
}

/* TyKind of an int/string/symbol literal node (for the unrolled method's
   subst-var type and return type). */
static TyKind dm_lit_type(Compiler *c, int lit) {
  const char *ty = nt_type(c->nt, lit);
  if (!ty) return TY_UNKNOWN;
  if (!strcmp(ty, "IntegerNode")) return TY_INT;
  if (!strcmp(ty, "StringNode"))  return TY_STRING;
  if (!strcmp(ty, "SymbolNode"))  return TY_SYMBOL;
  return TY_UNKNOWN;
}

/* Detect `[lit, ...].each { |v| define_method("m_#{v}") { body } }` in a
   class body and synthesize one method scope per literal element, each with
   a compile-time substitution of `v`. Returns 1 if handled. */
static int collect_dm_each_unroll(Compiler *c, int id, int class_id) {
  const NodeTable *nt = c->nt;
  if (class_id < 0) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || strcmp(nm, "each")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || strcmp(nt_type(nt, recv), "ArrayNode")) return 0;
  int blk = nt_ref(nt, id, "block");
  if (blk < 0) return 0;
  /* block parameter name */
  int pn = nt_ref(nt, blk, "parameters");
  int inner = pn >= 0 ? nt_ref(nt, pn, "parameters") : -1;
  int pnode = inner >= 0 ? inner : pn;
  int rnp = 0; const int *reqs = pnode >= 0 ? nt_arr(nt, pnode, "requireds", &rnp) : NULL;
  if (rnp < 1) return 0;
  const char *bv = nt_str(nt, reqs[0], "name");
  if (!bv) return 0;
  /* block body must be a single define_method call */
  int body = nt_ref(nt, blk, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn != 1) return 0;
  int dc = bb[0];
  if (!nt_type(nt, dc) || strcmp(nt_type(nt, dc), "CallNode")) return 0;
  const char *dcn = nt_str(nt, dc, "name");
  if (!dcn || strcmp(dcn, "define_method") || nt_ref(nt, dc, "receiver") >= 0) return 0;
  int dargs = nt_ref(nt, dc, "arguments");
  int dan = 0; const int *dav = dargs >= 0 ? nt_arr(nt, dargs, "arguments", &dan) : NULL;
  if (dan < 1) return 0;
  int dblk = nt_ref(nt, dc, "block");
  if (dblk < 0) return 0;
  int dbody = nt_ref(nt, dblk, "body");
  /* iterate the array literal's elements */
  int en = 0; const int *elems = nt_arr(nt, recv, "elements", &en);
  if (en == 0) return 0;
  for (int k = 0; k < en; k++) {
    TyKind lt = dm_lit_type(c, elems[k]);
    if (lt == TY_UNKNOWN) return 0;  /* non-literal element: bail (unhandled) */
    char *mname = dm_eval_name(c, dav[0], bv, elems[k]);
    if (!mname) return 0;
    Scope *ms = comp_scope_new(c, mname, dc);
    free(mname);
    ms->body = dbody;
    ms->class_id = class_id;
    ms->dm_subst_name = strdup(bv);
    ms->dm_subst_node = elems[k];
    /* the loop var reads inside the body resolve to the literal type */
    LocalVar *lv = scope_local_intern(ms, bv);
    lv->type = lt;
    lv->is_param = 1;  /* not a real C param, but keeps it out of decls */
    /* Walk the (shared) define_method body in this synthetic scope so its
       nodes get nscope attribution. The last element wins for the shared
       body nodes; that is fine since all elements share the value type. */
    int ms_idx = c->nscopes - 1;
    if (dbody >= 0) walk_scope(c, dbody, ms_idx, class_id);
  }
  return 1;
}

static void walk_scope(Compiler *c, int id, int scope_idx, int class_id) {
  if (id < 0 || id >= c->nt->count) return;
  c->nscope[id] = scope_idx;
  const char *ty = nt_type(c->nt, id);
  int child = scope_idx;
  int child_class = class_id;

  /* `class << self; def X; ...; end; end` — treat body defs as class methods. */
  if (ty && !strcmp(ty, "SingletonClassNode")) {
    int sbody = nt_ref(c->nt, id, "body");
    if (sbody >= 0) {
      int n = 0;
      const int *stmts = nt_arr(c->nt, sbody, "body", &n);
      for (int k = 0; k < n; k++) {
        int s = stmts[k];
        const char *sty = nt_type(c->nt, s);
        if (!sty) continue;
        if (!strcmp(sty, "DefNode")) {
          const char *name = nt_str(c->nt, s, "name");
          if (!name) continue;
          Scope *sc = comp_scope_new(c, name, s);
          int new_idx = c->nscopes - 1;
          sc->body = nt_ref(c->nt, s, "body");
          sc->class_id = class_id;
          sc->is_cmethod = 1;
          collect_def_params(c, s, sc);
          /* Assign scope to the def node and its body */
          c->nscope[s] = new_idx;
          if (sc->body >= 0) walk_scope(c, sc->body, new_idx, class_id);
        }
        else {
          walk_scope(c, s, scope_idx, class_id);
        }
      }
      c->nscope[id] = scope_idx;
      c->nscope[sbody] = scope_idx;
    }
    return;
  }

  if (ty && (!strcmp(ty, "ClassNode") || !strcmp(ty, "ModuleNode"))) {
    int cp = nt_ref(c->nt, id, "constant_path");
    const char *cname = cp >= 0 ? nt_str(c->nt, cp, "name") : NULL;
    if (cname && comp_class_index(c, cname) < 0) {
      comp_class_new(c, cname, id);
      child_class = c->nclasses - 1;
      c->classes[child_class].enclosing_class = class_id;
    }
    else if (cname) {
      child_class = comp_class_index(c, cname);  /* reopened class/module */
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
    collect_def_params(c, id, s);
    child = new_idx;
  }
  else if (ty && !strcmp(ty, "CallNode") && class_id >= 0) {
    /* [lits].each { |v| define_method("m_#{v}") { body } } -- unroll into one
       method per element. Handled wholesale; skip the generic recursion so
       the inner define_method isn't also processed as a normal call. */
    if (collect_dm_each_unroll(c, id, class_id)) return;
    /* define_method(:literal_name) { ... } at class scope: register as method scope */
    const char *dm_cn = nt_str(c->nt, id, "name");
    int dm_recv = nt_ref(c->nt, id, "receiver");
    if (dm_cn && !strcmp(dm_cn, "define_method") && dm_recv < 0) {
      int dm_args = nt_ref(c->nt, id, "arguments");
      int dm_na = 0;
      const int *dm_argv = dm_args >= 0 ? nt_arr(c->nt, dm_args, "arguments", &dm_na) : NULL;
      if (dm_na >= 1) {
        const char *dm_aty = nt_type(c->nt, dm_argv[0]);
        const char *dm_mname = NULL;
        if (dm_aty && !strcmp(dm_aty, "SymbolNode"))
          dm_mname = nt_str(c->nt, dm_argv[0], "value");
        else if (dm_aty && !strcmp(dm_aty, "StringNode"))
          dm_mname = nt_str(c->nt, dm_argv[0], "content");
        int dm_blk = nt_ref(c->nt, id, "block");
        if (dm_mname && dm_blk >= 0) {
          Scope *dm_s = comp_scope_new(c, dm_mname, id);
          int dm_new_idx = c->nscopes - 1;
          dm_s->body = nt_ref(c->nt, dm_blk, "body");
          dm_s->class_id = class_id;
          /* the block's params are the defined method's params (e.g. the
             `&:to_s`-rewritten `{ |_spx| _spx.to_s }`'s _spx). */
          int dm_pn = nt_ref(c->nt, dm_blk, "parameters");
          int dm_inner = dm_pn >= 0 ? nt_ref(c->nt, dm_pn, "parameters") : -1;
          int dm_pnode = dm_inner >= 0 ? dm_inner : dm_pn;
          int dm_rn = 0; const int *dm_reqs = dm_pnode >= 0 ? nt_arr(c->nt, dm_pnode, "requireds", &dm_rn) : NULL;
          for (int p = 0; p < dm_rn; p++) {
            const char *pnm = nt_str(c->nt, dm_reqs[p], "name");
            if (pnm) scope_add_param(dm_s, pnm, -1);
          }
          child = dm_new_idx;
        }
      }
    }
  }

  int saved_cbody = g_cbody_class_id;
  if (child_class >= 0) g_cbody_class_id = child_class;

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
  g_cbody_class_id = saved_cbody;
}

/* Mark methods following `module_function` in a module body as class-level
   (is_cmethod=1, no self param). This lets them be called as bare functions
   when their module is included at the top level. */
static void register_module_functions(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int ci = 0; ci < c->nclasses; ci++) {
    int dn = c->classes[ci].def_node;
    const char *dt = dn >= 0 ? nt_type(nt, dn) : NULL;
    if (!dt || strcmp(dt, "ModuleNode")) continue;
    int body = nt_ref(nt, dn, "body");
    if (body < 0) continue;
    int bn = 0;
    const int *stmts = nt_arr(nt, body, "body", &bn);
    int in_module_function = 0;
    for (int k = 0; k < bn; k++) {
      int s = stmts[k];
      const char *sty = nt_type(nt, s);
      if (!sty) continue;
      if (!strcmp(sty, "CallNode") && nt_ref(nt, s, "receiver") < 0) {
        const char *nm = nt_str(nt, s, "name");
        if (nm && !strcmp(nm, "module_function")) {
          /* `module_function :m1, :m2` form: mark named methods */
          int an = 0;
          int anode = nt_ref(nt, s, "arguments");
          const int *aargs = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
          if (an == 0) { in_module_function = 1; continue; }
          for (int ai = 0; ai < an; ai++) {
            const char *aty = nt_type(nt, aargs[ai]);
            const char *aval = NULL;
            if (aty && !strcmp(aty, "SymbolNode")) aval = nt_str(nt, aargs[ai], "value");
            if (!aval) continue;
            for (int mi = 0; mi < c->nscopes; mi++) {
              if (c->scopes[mi].class_id == ci && !c->scopes[mi].is_cmethod &&
                  c->scopes[mi].name && !strcmp(c->scopes[mi].name, aval))
                c->scopes[mi].is_cmethod = 1;
            }
          }
          continue;
        }
      }
      if (!strcmp(sty, "DefNode") && in_module_function) {
        const char *mname = nt_str(nt, s, "name");
        if (!mname) continue;
        for (int mi = 0; mi < c->nscopes; mi++) {
          if (c->scopes[mi].def_node == s) { c->scopes[mi].is_cmethod = 1; break; }
        }
      }
    }
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
        !strcmp(ty, "LocalVariableOperatorWriteNode") ||
        !strcmp(ty, "LocalVariableOrWriteNode") ||
        !strcmp(ty, "LocalVariableAndWriteNode")) {
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

/* Is CallNode `val` a `Struct.new(...)` / `Data.define(...)`? */
static int is_struct_call(Compiler *c, int val) {
  const NodeTable *nt = c->nt;
  if (val < 0 || !nt_type(nt, val) || strcmp(nt_type(nt, val), "CallNode")) return 0;
  const char *mn = nt_str(nt, val, "name");
  int vr = nt_ref(nt, val, "receiver");
  const char *rn = vr >= 0 && nt_type(nt, vr) && !strcmp(nt_type(nt, vr), "ConstantReadNode")
                   ? nt_str(nt, vr, "name") : NULL;
  return rn && ((!strcmp(rn, "Struct") && mn && !strcmp(mn, "new")) ||
                (!strcmp(rn, "Data") && mn && !strcmp(mn, "define")));
}

/* Register the symbol members of a Struct.new(...) call onto `cls`. */
static void register_struct_members(Compiler *c, ClassInfo *cls, int val) {
  const NodeTable *nt = c->nt;
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

static void register_structs(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    /* Const = Struct.new(:a, :b) */
    if (!strcmp(ty, "ConstantWriteNode")) {
      const char *cname = nt_str(nt, id, "name");
      int val = nt_ref(nt, id, "value");
      if (!cname || !is_c_ident(cname) || !is_struct_call(c, val)) continue;
      if (comp_class_index(c, cname) >= 0) continue;
      register_struct_members(c, comp_class_new(c, cname, id), val);
    }
    /* class X < Struct.new(:a, :b); ... end */
    else if (!strcmp(ty, "ClassNode")) {
      int sup = nt_ref(nt, id, "superclass");
      if (!is_struct_call(c, sup)) continue;
      int cp = nt_ref(nt, id, "constant_path");
      const char *cname = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
      int ci = cname ? comp_class_index(c, cname) : -1;
      if (ci >= 0) register_struct_members(c, &c->classes[ci], sup);
    }
  }
}

/* Fix scope class_id for DefNodes inside Struct.new { } blocks.
   walk_scope runs before register_structs, so defs in struct blocks get
   class_id=-1. This pass corrects them after the class is registered. */
static void fix_struct_block_scopes(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "ConstantWriteNode")) continue;
    const char *cname = nt_str(nt, id, "name");
    int val = nt_ref(nt, id, "value");
    if (!cname || val < 0 || !is_struct_call(c, val)) continue;
    int blk = nt_ref(nt, val, "block");
    if (blk < 0) continue;
    int ci = comp_class_index(c, cname);
    if (ci < 0) continue;
    /* Walk the block body and fix any DefNode scopes */
    int bbody = nt_ref(nt, blk, "body");
    if (bbody < 0) continue;
    int bn = 0;
    const int *stmts = nt_arr(nt, bbody, "body", &bn);
    for (int k = 0; k < bn; k++) {
      const char *sty = nt_type(nt, stmts[k]);
      if (!sty || strcmp(sty, "DefNode")) continue;
      int dn = stmts[k];
      /* Find the scope whose def_node == dn and fix its class_id */
      for (int s = 0; s < c->nscopes; s++) {
        if (c->scopes[s].def_node == dn) {
          c->scopes[s].class_id = ci;
          break;
        }
      }
    }
  }
}

/* Process attr_accessor/reader/writer call: register ivars + reader/writer names.
   If `singleton` is non-zero, registers singleton (class-level) accessors instead. */
static void register_attr_call(Compiler *c, ClassInfo *cls, int s, int singleton) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, s, "name");
  if (!nm) return;
  int accessor = !strcmp(nm, "attr_accessor") ||
                 !strcmp(nm, "attribute") || !strcmp(nm, "attributes");
  int reader = !strcmp(nm, "attr_reader") || accessor;
  int writer = !strcmp(nm, "attr_writer") || accessor;
  if (!reader && !writer) return;
  int args = nt_ref(nt, s, "arguments");
  int an = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  for (int a = 0; a < an; a++) {
    const char *aty = nt_type(nt, argv[a]);
    if (!aty || strcmp(aty, "SymbolNode")) continue;
    const char *base = nt_str(nt, argv[a], "value");
    if (!base) continue;
    if (singleton) {
      if (reader) comp_add_sg_reader(cls, base);
      if (writer) comp_add_sg_writer(cls, base);
    } else {
      char ivname[256];
      snprintf(ivname, sizeof ivname, "@%s", base);
      comp_ivar_intern(cls, ivname);
      if (reader) comp_add_reader(cls, base);
      if (writer) comp_add_writer(cls, base);
    }
  }
}

/* Collect attr_reader/attr_writer/attr_accessor declarations in class
   bodies, registering backing ivars + reader/writer method names.
   Also scans class << self bodies for singleton-level attr_accessors. */
static void register_attrs_body(Compiler *c, ClassInfo *cls, int body) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
  for (int k = 0; k < n; k++) {
    int s = stmts[k];
    const char *sty = nt_type(nt, s);
    if (!sty) continue;
    if (!strcmp(sty, "CallNode")) {
      register_attr_call(c, cls, s, 0);
    }
    else if (!strcmp(sty, "SingletonClassNode")) {
      /* class << self; attr_accessor :x; end */
      int sbody = nt_ref(nt, s, "body");
      if (sbody < 0) continue;
      int sn = 0;
      const int *sstmts = nt_arr(nt, sbody, "body", &sn);
      for (int j = 0; j < sn; j++) {
        int ss = sstmts[j];
        const char *ssty = nt_type(nt, ss);
        if (ssty && !strcmp(ssty, "CallNode"))
          register_attr_call(c, cls, ss, 1);
      }
    }
  }
}

static void register_attrs(Compiler *c) {
  const NodeTable *nt = c->nt;
  /* Pass 1: process primary definition bodies. */
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cls = &c->classes[ci];
    register_attrs_body(c, cls, nt_ref(nt, cls->def_node, "body"));
  }
  /* Pass 2: scan all ClassNode/ModuleNode reopenings. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || (strcmp(ty, "ClassNode") && strcmp(ty, "ModuleNode"))) continue;
    int cp = nt_ref(nt, id, "constant_path");
    const char *cname = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!cname) continue;
    int ci = comp_class_index(c, cname);
    if (ci < 0) continue;
    if (id == c->classes[ci].def_node) continue;  /* already handled above */
    register_attrs_body(c, &c->classes[ci], nt_ref(nt, id, "body"));
  }
}

/* Collect `alias new old` (AliasMethodNode) and `alias_method :new, :old`
   (CallNode) statements in class bodies into the class alias table. */
static void register_aliases_body(Compiler *c, ClassInfo *cls, int body) {
  const NodeTable *nt = c->nt;
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

static void register_aliases(Compiler *c) {
  const NodeTable *nt = c->nt;
  /* Pass 1: primary definition bodies. */
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cls = &c->classes[ci];
    register_aliases_body(c, cls, nt_ref(nt, cls->def_node, "body"));
  }
  /* Pass 2: reopened class/module bodies. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || (strcmp(ty, "ClassNode") && strcmp(ty, "ModuleNode"))) continue;
    int cp = nt_ref(nt, id, "constant_path");
    const char *cname = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!cname) continue;
    int ci = comp_class_index(c, cname);
    if (ci < 0) continue;
    if (id == c->classes[ci].def_node) continue;
    register_aliases_body(c, &c->classes[ci], nt_ref(nt, id, "body"));
  }
}

static void register_undefs_body(Compiler *c, ClassInfo *cls, int body) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
  for (int k = 0; k < n; k++) {
    int s = stmts[k];
    const char *sty = nt_type(nt, s);
    if (!sty || strcmp(sty, "UndefNode")) continue;
    int names_n = 0;
    const int *names = nt_arr(nt, s, "names", &names_n);
    for (int j = 0; j < names_n; j++) {
      const char *mname = nt_str(nt, names[j], "value");
      if (mname) comp_add_undef(cls, mname);
    }
  }
}

static void register_undefs(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cls = &c->classes[ci];
    register_undefs_body(c, cls, nt_ref(nt, cls->def_node, "body"));
  }
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || (strcmp(ty, "ClassNode") && strcmp(ty, "ModuleNode"))) continue;
    int cp = nt_ref(nt, id, "constant_path");
    const char *cname = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!cname) continue;
    int ci = comp_class_index(c, cname);
    if (ci < 0) continue;
    if (id == c->classes[ci].def_node) continue;
    register_undefs_body(c, &c->classes[ci], nt_ref(nt, id, "body"));
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
  /* Pass 1: collect alias $copy $orig mappings first so pass 2 can skip them. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "AliasGlobalVariableNode")) continue;
    int nw_id  = nt_ref(nt, id, "new_name");
    int old_id = nt_ref(nt, id, "old_name");
    const char *nw  = nw_id  >= 0 ? nt_str(nt, nw_id,  "name") : NULL;
    const char *old = old_id >= 0 ? nt_str(nt, old_id, "name") : NULL;
    if (nw && nw[0] == '$' && is_c_ident(nw + 1) &&
        old && old[0] == '$' && is_c_ident(old + 1)) {
      comp_gvar_intern(c, old + 1);             /* intern the original */
      comp_add_gvar_alias(c, nw + 1, old + 1); /* $new -> $old */
    }
  }
  /* Pass 2: intern all other globals (skipping alias names). */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (!strcmp(ty, "GlobalVariableWriteNode") || !strcmp(ty, "GlobalVariableReadNode") ||
        !strcmp(ty, "GlobalVariableOperatorWriteNode") || !strcmp(ty, "GlobalVariableTargetNode") ||
        !strcmp(ty, "GlobalVariableOrWriteNode") || !strcmp(ty, "GlobalVariableAndWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      /* skip alias names - they resolve to the original and need no separate slot */
      if (nm && nm[0] == '$' && is_c_ident(nm + 1) &&
          !strcmp(nm + 1, comp_resolve_gvar(c, nm + 1)))
        comp_gvar_intern(c, nm + 1);
    }
    else if (!strcmp(ty, "AliasGlobalVariableNode")) {
      /* already handled in pass 1 */
    }
    else if (!strcmp(ty, "ConstantTargetNode")) {
      /* target in a multi-write: A, B = expr */
      const char *nm = nt_str(nt, id, "name");
      if (nm && is_c_ident(nm) && comp_class_index(c, nm) < 0)
        comp_const_intern(c, nm);
    }
    else if (!strcmp(ty, "ConstantWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      /* a constant bound to a regex literal is resolved at compile time to a
         precompiled pattern, not stored as a runtime value */
      int rv = nt_ref(nt, id, "value");
      if (rv >= 0 && nt_type(nt, rv) && !strcmp(nt_type(nt, rv), "CallNode") &&
          nt_str(nt, rv, "name") && !strcmp(nt_str(nt, rv, "name"), "freeze"))
        rv = nt_ref(nt, rv, "receiver");
      int is_regex_const = rv >= 0 && nt_type(nt, rv) && !strcmp(nt_type(nt, rv), "RegularExpressionNode");
      /* regex constants: store with type TY_REGEX so call-type inference works */
      if (nm && is_regex_const) {
        LocalVar *cv = comp_const_intern(c, nm);
        cv->type = TY_REGEX;
      }
      /* a Struct/Data const names a class, not a value constant.
         Do NOT skip when the name collides with a module: M::V = "str" is a
         value constant even though top-level `module V` exists. */
      if (nm && is_c_ident(nm) && !is_regex_const) {
        LocalVar *cv = comp_const_intern(c, nm);
        /* `CONST = SomeClass.new(...)`: reads of CONST during the new()
           (i.e. inside initialize or anything it calls) must raise
           NameError, since CONST is not yet bound. */
        int v = nt_ref(nt, id, "value");
        const char *vty = v >= 0 ? nt_type(nt, v) : NULL;
        if (vty && !strcmp(vty, "CallNode") && nt_str(nt, v, "name") &&
            !strcmp(nt_str(nt, v, "name"), "new")) {
          int vr = nt_ref(nt, v, "receiver");
          if (vr >= 0 && nt_type(nt, vr) && !strcmp(nt_type(nt, vr), "ConstantReadNode") &&
              nt_str(nt, vr, "name") && comp_class_index(c, nt_str(nt, vr, "name")) >= 0)
            cv->init_guarded = 1;
        }
      }
    }
  }
}

/* Extract a symbol or string literal text from a node, or NULL. */
static const char *ffi_arg_str(const NodeTable *nt, int nid) {
  if (nid < 0) return NULL;
  const char *ty = nt_type(nt, nid);
  if (!ty) return NULL;
  if (!strcmp(ty, "SymbolNode")) return nt_str(nt, nid, "value");
  if (!strcmp(ty, "StringNode")) return nt_str(nt, nid, "content");
  return NULL;
}

/* Extract an integer literal value, or -1. */
static int ffi_arg_int(const NodeTable *nt, int nid) {
  if (nid < 0) return -1;
  const char *ty = nt_type(nt, nid);
  if (!ty) return -1;
  if (!strcmp(ty, "IntegerNode")) return (int)nt_int(nt, nid, "value", 0);
  return -1;
}

/* Map an FFI spec string to the Spinel TyKind used for return types. */
static TyKind ffi_spec_to_ty(const char *spec) {
  if (!spec) return TY_UNKNOWN;
  if (!strcmp(spec,"int")||!strcmp(spec,"uint32")||!strcmp(spec,"int32")||
      !strcmp(spec,"uint16")||!strcmp(spec,"int16")||!strcmp(spec,"uint8")||
      !strcmp(spec,"size_t")||!strcmp(spec,"long")||!strcmp(spec,"int64"))
    return TY_INT;
  if (!strcmp(spec,"float")||!strcmp(spec,"double")) return TY_FLOAT;
  if (!strcmp(spec,"str")) return TY_STRING;
  if (!strcmp(spec,"bool")) return TY_BOOL;
  if (!strcmp(spec,"ptr")) return TY_POLY;
  if (!strcmp(spec,"void")) return TY_NIL;
  if (!strcmp(spec,"float_array")) return TY_FLOAT_ARRAY;
  if (!strcmp(spec,"int_array")) return TY_INT_ARRAY;
  return TY_UNKNOWN;
}

/* Register a ffi_func / ffi_const / ffi_buffer / ffi_read_* declared in
   module bodies. Called during analyze_program before fixpoint. */
static void register_ffi_decls(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "ModuleNode")) continue;
    int cp = nt_ref(nt, id, "constant_path");
    const char *mname = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!mname) continue;
    int body = nt_ref(nt, id, "body");
    int sn = 0;
    const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &sn) : NULL;
    for (int k = 0; k < sn; k++) {
      int s = stmts[k];
      const char *sty = nt_type(nt, s);
      if (!sty || strcmp(sty, "CallNode")) continue;
      if (nt_ref(nt, s, "receiver") >= 0) continue;
      const char *dname = nt_str(nt, s, "name");
      if (!dname) continue;
      int anode = nt_ref(nt, s, "arguments");
      int an = 0;
      const int *args = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;

      if (!strcmp(dname, "ffi_lib")) {
        if (an < 1) continue;
        const char *libname = ffi_arg_str(nt, args[0]);
        if (!libname) continue;
        /* find or create lib entry */
        int mi = -1;
        for (int li = 0; li < c->n_ffi_libs; li++)
          if (!strcmp(c->ffi_lib_mods[li], mname)) { mi = li; break; }
        if (mi < 0) {
          if (c->n_ffi_libs >= c->c_ffi_libs) {
            c->c_ffi_libs = c->c_ffi_libs ? c->c_ffi_libs * 2 : 8;
            c->ffi_lib_mods  = realloc(c->ffi_lib_mods,  sizeof(char*) * (size_t)c->c_ffi_libs);
            c->ffi_lib_names = realloc(c->ffi_lib_names, sizeof(char*) * (size_t)c->c_ffi_libs);
          }
          c->ffi_lib_mods[c->n_ffi_libs]  = strdup(mname);
          c->ffi_lib_names[c->n_ffi_libs] = strdup(libname);
          mi = c->n_ffi_libs++;
        }
        else {
          /* append with semicolon */
          size_t old_len = strlen(c->ffi_lib_names[mi]);
          size_t new_len = old_len + 1 + strlen(libname) + 1;
          char *merged = malloc(new_len);
          snprintf(merged, new_len, "%s;%s", c->ffi_lib_names[mi], libname);
          free(c->ffi_lib_names[mi]);
          c->ffi_lib_names[mi] = merged;
        }
        continue;
      }

      if (!strcmp(dname, "ffi_func")) {
        if (an < 3) continue;
        const char *fname = ffi_arg_str(nt, args[0]);
        if (!fname) continue;
        /* arg type array */
        int arr_id = args[1];
        const char *arr_ty = nt_type(nt, arr_id);
        if (!arr_ty || strcmp(arr_ty, "ArrayNode")) continue;
        int en = 0;
        const int *elems = nt_arr(nt, arr_id, "elements", &en);
        char **arg_specs = malloc(sizeof(char*) * (size_t)(en + 1));
        for (int ei = 0; ei < en; ei++) {
          const char *spec = ffi_arg_str(nt, elems[ei]);
          arg_specs[ei] = strdup(spec ? spec : "");
        }
        const char *ret_spec = ffi_arg_str(nt, args[2]);
        if (!ret_spec) { free(arg_specs); continue; }
        /* grow array */
        if (c->n_ffi_funcs >= c->c_ffi_funcs) {
          c->c_ffi_funcs = c->c_ffi_funcs ? c->c_ffi_funcs * 2 : 16;
          c->ffi_func_mods   = realloc(c->ffi_func_mods,   sizeof(char*) * (size_t)c->c_ffi_funcs);
          c->ffi_func_names  = realloc(c->ffi_func_names,  sizeof(char*) * (size_t)c->c_ffi_funcs);
          c->ffi_func_ret    = realloc(c->ffi_func_ret,    sizeof(char*) * (size_t)c->c_ffi_funcs);
          c->ffi_func_args   = realloc(c->ffi_func_args,   sizeof(char**) * (size_t)c->c_ffi_funcs);
          c->ffi_func_nargs  = realloc(c->ffi_func_nargs,  sizeof(int) * (size_t)c->c_ffi_funcs);
        }
        int fi = c->n_ffi_funcs++;
        c->ffi_func_mods[fi]  = strdup(mname);
        c->ffi_func_names[fi] = strdup(fname);
        c->ffi_func_ret[fi]   = strdup(ret_spec);
        c->ffi_func_args[fi]  = arg_specs;
        c->ffi_func_nargs[fi] = en;
        continue;
      }

      if (!strcmp(dname, "ffi_const")) {
        if (an < 2) continue;
        const char *kname = ffi_arg_str(nt, args[0]);
        if (!kname) continue;
        int val = ffi_arg_int(nt, args[1]);
        if (c->n_ffi_consts >= c->c_ffi_consts) {
          c->c_ffi_consts = c->c_ffi_consts ? c->c_ffi_consts * 2 : 16;
          c->ffi_const_mods  = realloc(c->ffi_const_mods,  sizeof(char*) * (size_t)c->c_ffi_consts);
          c->ffi_const_names = realloc(c->ffi_const_names, sizeof(char*) * (size_t)c->c_ffi_consts);
          c->ffi_const_vals  = realloc(c->ffi_const_vals,  sizeof(int) * (size_t)c->c_ffi_consts);
        }
        int ci2 = c->n_ffi_consts++;
        c->ffi_const_mods[ci2]  = strdup(mname);
        c->ffi_const_names[ci2] = strdup(kname);
        c->ffi_const_vals[ci2]  = val;
        continue;
      }

      if (!strcmp(dname, "ffi_buffer")) {
        if (an < 2) continue;
        const char *bname = ffi_arg_str(nt, args[0]);
        if (!bname) continue;
        int bsize = ffi_arg_int(nt, args[1]);
        if (bsize <= 0) continue;
        if (c->n_ffi_bufs >= c->c_ffi_bufs) {
          c->c_ffi_bufs = c->c_ffi_bufs ? c->c_ffi_bufs * 2 : 8;
          c->ffi_buf_mods  = realloc(c->ffi_buf_mods,  sizeof(char*) * (size_t)c->c_ffi_bufs);
          c->ffi_buf_names = realloc(c->ffi_buf_names, sizeof(char*) * (size_t)c->c_ffi_bufs);
          c->ffi_buf_sizes = realloc(c->ffi_buf_sizes, sizeof(int) * (size_t)c->c_ffi_bufs);
        }
        int bi = c->n_ffi_bufs++;
        c->ffi_buf_mods[bi]  = strdup(mname);
        c->ffi_buf_names[bi] = strdup(bname);
        c->ffi_buf_sizes[bi] = bsize;
        continue;
      }

      if (!strncmp(dname, "ffi_read_", 9)) {
        if (an < 2) continue;
        const char *rname = ffi_arg_str(nt, args[0]);
        if (!rname) continue;
        int roff = ffi_arg_int(nt, args[1]);
        if (roff < 0) roff = 0;
        const char *kind = dname + 9;  /* "u32", "i32", "ptr" */
        if (c->n_ffi_readers >= c->c_ffi_readers) {
          c->c_ffi_readers = c->c_ffi_readers ? c->c_ffi_readers * 2 : 8;
          c->ffi_reader_mods    = realloc(c->ffi_reader_mods,    sizeof(char*) * (size_t)c->c_ffi_readers);
          c->ffi_reader_names   = realloc(c->ffi_reader_names,   sizeof(char*) * (size_t)c->c_ffi_readers);
          c->ffi_reader_offsets = realloc(c->ffi_reader_offsets, sizeof(int) * (size_t)c->c_ffi_readers);
          c->ffi_reader_kinds   = realloc(c->ffi_reader_kinds,   sizeof(char*) * (size_t)c->c_ffi_readers);
        }
        int ri = c->n_ffi_readers++;
        c->ffi_reader_mods[ri]    = strdup(mname);
        c->ffi_reader_names[ri]   = strdup(rname);
        c->ffi_reader_offsets[ri] = roff;
        c->ffi_reader_kinds[ri]   = strdup(kind);
        continue;
      }
    }
  }
}

/* Look up an FFI func by (module, name). Returns index or -1. */
static int ffi_find_func(Compiler *c, const char *mod, const char *name) {
  for (int i = 0; i < c->n_ffi_funcs; i++)
    if (!strcmp(c->ffi_func_mods[i], mod) && !strcmp(c->ffi_func_names[i], name))
      return i;
  return -1;
}

/* Look up an FFI buffer by (module, name). Returns index or -1. */
static int ffi_find_buf(Compiler *c, const char *mod, const char *name) {
  for (int i = 0; i < c->n_ffi_bufs; i++)
    if (!strcmp(c->ffi_buf_mods[i], mod) && !strcmp(c->ffi_buf_names[i], name))
      return i;
  return -1;
}

/* Look up an FFI reader by (module, name). Returns index or -1. */
static int ffi_find_reader(Compiler *c, const char *mod, const char *name) {
  for (int i = 0; i < c->n_ffi_readers; i++)
    if (!strcmp(c->ffi_reader_mods[i], mod) && !strcmp(c->ffi_reader_names[i], name))
      return i;
  return -1;
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
      const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
      if (rn) lv = comp_gvar(c, rn);
      vt = infer_type(c, nt_ref(nt, id, "value"));
      if (vt == TY_NIL) continue;
    }
    else if (!strcmp(ty, "GlobalVariableOperatorWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
      if (rn) lv = comp_gvar(c, rn);
      TyKind cur = lv ? lv->type : TY_UNKNOWN;
      TyKind v = infer_type(c, nt_ref(nt, id, "value"));
      if (cur == TY_STRING) vt = TY_STRING;
      else if (ty_is_numeric(cur) && ty_is_numeric(v)) vt = (cur == TY_FLOAT || v == TY_FLOAT) ? TY_FLOAT : TY_INT;
      else vt = cur;
    }
    else if (!strcmp(ty, "GlobalVariableOrWriteNode") || !strcmp(ty, "GlobalVariableAndWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
      if (rn) lv = comp_gvar(c, rn);
      vt = infer_type(c, nt_ref(nt, id, "value"));
      if (vt == TY_NIL) continue;
    }
    else if (!strcmp(ty, "ConstantWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (nm) lv = comp_const(c, nm);
      vt = infer_type(c, nt_ref(nt, id, "value"));
    }
    else if (!strcmp(ty, "MultiWriteNode")) {
      int ln = 0;
      const int *lefts = nt_arr(nt, id, "lefts", &ln);
      int value = nt_ref(nt, id, "value");
      const char *vty = nt_type(nt, value);
      int en = 0;
      const int *els = (vty && !strcmp(vty, "ArrayNode")) ? nt_arr(nt, value, "elements", &en) : NULL;
      int rn_count = 0;
      nt_arr(nt, id, "rights", &rn_count);
      for (int i = 0; i < ln; i++) {
        const char *lty2 = nt_type(nt, lefts[i]);
        if (!lty2 || strcmp(lty2, "GlobalVariableTargetNode")) continue;
        const char *gnm = nt_str(nt, lefts[i], "name");
        const char *rn2 = gnm ? comp_resolve_gvar(c, gnm + 1) : NULL;
        LocalVar *glv = rn2 ? comp_gvar(c, rn2) : NULL;
        if (!glv) continue;
        TyKind vt2 = (els && i < en) ? infer_type(c, els[i]) : TY_UNKNOWN;
        if (vt2 == TY_NIL || vt2 == TY_UNKNOWN) continue;
        TyKind merged2 = ty_unify(glv->type, vt2);
        if (merged2 != glv->type) { glv->type = merged2; changed = 1; }
      }
      /* handle splat-rest global target (*$rest = ...) */
      int rest_nid2 = nt_ref(nt, id, "rest");
      if (rest_nid2 >= 0) {
        const char *rsty2 = nt_type(nt, rest_nid2);
        int rest_inner2 = (rsty2 && !strcmp(rsty2, "SplatNode")) ? nt_ref(nt, rest_nid2, "expression") : -1;
        const char *rinty2 = rest_inner2 >= 0 ? nt_type(nt, rest_inner2) : NULL;
        if (rinty2 && !strcmp(rinty2, "GlobalVariableTargetNode")) {
          const char *gnm2 = nt_str(nt, rest_inner2, "name");
          const char *rn3 = gnm2 ? comp_resolve_gvar(c, gnm2 + 1) : NULL;
          LocalVar *glv2 = rn3 ? comp_gvar(c, rn3) : NULL;
          if (glv2 && els) {
            TyKind rest_elem = TY_UNKNOWN;
            for (int i = ln; i < en - rn_count; i++)
              rest_elem = ty_unify(rest_elem, infer_type(c, els[i]));
            TyKind rest_arr_t = (rest_elem != TY_UNKNOWN) ? ty_array_of(rest_elem) : TY_UNKNOWN;
            if (rest_arr_t != TY_UNKNOWN) {
              TyKind merged3 = ty_unify(glv2->type, rest_arr_t);
              if (merged3 != glv2->type) { glv2->type = merged3; changed = 1; }
            }
          }
        }
      }
      continue;
    }
    else if (!strcmp(ty, "CallNode")) {
      /* CONST << v / CONST.push(v) / CONST.append(v): infer CONST as an
         array whose element type comes from v's type. Only applies when
         the receiver is a direct ConstantReadNode. */
      const char *cnm = nt_str(nt, id, "name");
      if (!cnm) continue;
      int is_push = (!strcmp(cnm, "<<") || !strcmp(cnm, "push") || !strcmp(cnm, "append"));
      if (!is_push) continue;
      int crecv = nt_ref(nt, id, "receiver");
      if (crecv < 0) continue;
      const char *rty = nt_type(nt, crecv);
      if (!rty || strcmp(rty, "ConstantReadNode")) continue;
      const char *cnm2 = nt_str(nt, crecv, "name");
      if (!cnm2) continue;
      lv = comp_const(c, cnm2);
      if (!lv || lv->type != TY_UNKNOWN) continue;
      int cargs = nt_ref(nt, id, "arguments");
      int cac = 0;
      const int *cav = cargs >= 0 ? nt_arr(nt, cargs, "arguments", &cac) : NULL;
      if (cac < 1 || !cav) continue;
      TyKind et = infer_type(c, cav[0]);
      if (et == TY_UNKNOWN || et == TY_NIL) continue;
      vt = ty_array_of(et);
      if (vt == TY_UNKNOWN) vt = TY_POLY_ARRAY;
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

/* Re-infer constants assigned via multi-write with a call/variable RHS.
   The existing infer_write_types pass widened them to TY_POLY early (before
   block params converged); this pass overrides with the now-stable element
   type once it is known and not poly. */
static int infer_multiwrite_const_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "MultiWriteNode")) continue;
    int value = nt_ref(nt, id, "value");
    if (value < 0) continue;
    const char *vty = nt_type(nt, value);
    if (vty && !strcmp(vty, "ArrayNode")) continue; /* literal handled in infer_write_types */
    TyKind st = infer_type(c, value);
    if (!ty_is_array(st)) continue;
    TyKind elem = ty_array_elem(st);
    if (elem == TY_POLY || elem == TY_UNKNOWN) continue; /* not yet settled */
    int ln = 0;
    const int *lefts = nt_arr(nt, id, "lefts", &ln);
    for (int i = 0; i < ln; i++) {
      const char *lty = nt_type(nt, lefts[i]) ? nt_type(nt, lefts[i]) : "";
      if (strcmp(lty, "ConstantTargetNode")) continue;
      const char *nm = nt_str(nt, lefts[i], "name");
      LocalVar *cv = nm ? comp_const(c, nm) : NULL;
      if (!cv || cv->type == elem) continue;
      cv->type = elem; changed = 1;
    }
    int rn = 0;
    const int *rights = nt_arr(nt, id, "rights", &rn);
    for (int j = 0; j < rn; j++) {
      const char *rty2 = nt_type(nt, rights[j]) ? nt_type(nt, rights[j]) : "";
      if (strcmp(rty2, "ConstantTargetNode")) continue;
      const char *nm = nt_str(nt, rights[j], "name");
      LocalVar *cv = nm ? comp_const(c, nm) : NULL;
      if (!cv || cv->type == elem) continue;
      cv->type = elem; changed = 1;
    }
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

/* Process include calls in a single class body, creating scope copies for each
   included module method. We copy (not mutate) so multiple classes can include
   the same module independently. */
static void process_include_body(Compiler *c, int ci, int body_node) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *stmts = body_node >= 0 ? nt_arr(nt, body_node, "body", &n) : NULL;
  for (int k = 0; k < n; k++) {
    int s = stmts[k];
    const char *sty = nt_type(nt, s);
    if (!sty || strcmp(sty, "CallNode")) continue;
    const char *nm = nt_str(nt, s, "name");
    if (!nm || strcmp(nm, "include")) continue;
    if (nt_ref(nt, s, "receiver") >= 0) continue;
    int anode = nt_ref(nt, s, "arguments");
    int an = 0;
    const int *args = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
    for (int j = 0; j < an; j++) {
      const char *aty = nt_type(nt, args[j]);
      const char *mname = NULL;
      if (aty && !strcmp(aty, "ConstantReadNode")) mname = nt_str(nt, args[j], "name");
      else if (aty && !strcmp(aty, "ConstantPathNode")) mname = nt_str(nt, args[j], "name");
      int mod_id = mname ? comp_class_index(c, mname) : -1;
      if (mod_id < 0) continue;
      /* snapshot count before adding new scopes to avoid re-scanning them */
      int snap = c->nscopes;
      for (int ms = 0; ms < snap; ms++) {
        Scope *src = &c->scopes[ms];
        if (src->class_id != mod_id || src->is_cmethod || !src->name) continue;
        if (comp_method_in_class(c, ci, src->name) >= 0) continue;
        /* Create a new scope sharing the same AST nodes but owned by ci. */
        Scope *dst = comp_scope_new(c, src->name, src->def_node);
        /* comp_scope_new may realloc c->scopes; re-derive src pointer. */
        src = &c->scopes[ms];
        dst->body = src->body;
        dst->class_id = ci;
        dst->is_cmethod = 0;
        dst->reachable = src->reachable;
        dst->yields = src->yields;
        dst->nrequired = src->nrequired;
        dst->rest_idx = src->rest_idx;
        dst->kwrest_idx = src->kwrest_idx;
        if (src->blk_param) dst->blk_param = strdup(src->blk_param);
        src->is_transplanted_source = 1;
        /* Copy parameter names and defaults. */
        dst->nparams = src->nparams;
        if (src->nparams > 0) {
          dst->pnames = malloc(sizeof(char *) * (size_t)src->nparams);
          dst->pdefault = malloc(sizeof(int) * (size_t)src->nparams);
          for (int p = 0; p < src->nparams; p++) {
            dst->pnames[p] = src->pnames[p] ? strdup(src->pnames[p]) : NULL;
            dst->pdefault[p] = src->pdefault ? src->pdefault[p] : -1;
          }
          /* Register param locals so infer_param_types can update types. */
          for (int p = 0; p < src->nparams; p++) {
            if (dst->pnames[p]) {
              LocalVar *lv = scope_local_intern(dst, dst->pnames[p]);
              lv->is_param = 1;
            }
          }
        }
        /* Scan source body for ivar accesses and register them in the
           destination class so codegen's struct layout includes them. */
        for (int id2 = 0; id2 < nt->count; id2++) {
          if (c->nscope[id2] != ms) continue;
          const char *bty = nt_type(nt, id2);
          if (!bty) continue;
          if (!strcmp(bty, "InstanceVariableWriteNode") ||
              !strcmp(bty, "InstanceVariableReadNode") ||
              !strcmp(bty, "InstanceVariableOperatorWriteNode") ||
              !strcmp(bty, "InstanceVariableOrWriteNode")) {
            const char *ivnm = nt_str(nt, id2, "name");
            if (ivnm) comp_ivar_intern(&c->classes[ci], ivnm);
          }
        }
      }
    }
  }
}

/* For each class, find `include M` declarations in ALL class bodies
   (including reopenings) and transplant M's instance methods into the
   class so they are reachable via comp_method_in_chain. */
static void register_includes(Compiler *c) {
  const NodeTable *nt = c->nt;
  /* First pass: process def_node bodies (first class definition). */
  for (int ci = 0; ci < c->nclasses; ci++) {
    int body = nt_ref(nt, c->classes[ci].def_node, "body");
    process_include_body(c, ci, body);
  }
  /* Second pass: scan all ClassNode/ModuleNode in the AST for reopenings. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || (strcmp(ty, "ClassNode") && strcmp(ty, "ModuleNode"))) continue;
    int cp = nt_ref(nt, id, "constant_path");
    const char *cname = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!cname) continue;
    int ci = comp_class_index(c, cname);
    if (ci < 0) continue;
    if (id == c->classes[ci].def_node) continue;  /* already processed above */
    int body = nt_ref(nt, id, "body");
    process_include_body(c, ci, body);
  }
}

/* For each class, find `extend M` declarations and transplant M's instance
   methods as class methods (is_cmethod=1) so they are callable as C.m. */
static void register_extends(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int ci = 0; ci < c->nclasses; ci++) {
    int body = nt_ref(nt, c->classes[ci].def_node, "body");
    int n = 0;
    const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    for (int k = 0; k < n; k++) {
      int s = stmts[k];
      const char *sty = nt_type(nt, s);
      if (!sty || strcmp(sty, "CallNode")) continue;
      const char *nm = nt_str(nt, s, "name");
      if (!nm || strcmp(nm, "extend")) continue;
      if (nt_ref(nt, s, "receiver") >= 0) continue;
      int anode = nt_ref(nt, s, "arguments");
      int an = 0;
      const int *args = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
      for (int j = 0; j < an; j++) {
        const char *aty = nt_type(nt, args[j]);
        const char *mname = NULL;
        if (aty && !strcmp(aty, "ConstantReadNode")) mname = nt_str(nt, args[j], "name");
        else if (aty && !strcmp(aty, "ConstantPathNode")) mname = nt_str(nt, args[j], "name");
        int mod_id = mname ? comp_class_index(c, mname) : -1;
        if (mod_id < 0) continue;
        int snap = c->nscopes;
        for (int ms = 0; ms < snap; ms++) {
          Scope *src = &c->scopes[ms];
          /* Only transplant instance methods; self.* on the module stay on it. */
          if (src->class_id != mod_id || src->is_cmethod || !src->name) continue;
          if (comp_cmethod_in_class(c, ci, src->name) >= 0) continue;
          Scope *dst = comp_scope_new(c, src->name, src->def_node);
          src = &c->scopes[ms];
          dst->body = src->body;
          dst->class_id = ci;
          dst->is_cmethod = 1;  /* transplanted as a class method */
          dst->reachable = src->reachable;
          dst->yields = src->yields;
          dst->nrequired = src->nrequired;
          dst->rest_idx = src->rest_idx;
          dst->kwrest_idx = src->kwrest_idx;
          if (src->blk_param) dst->blk_param = strdup(src->blk_param);
          dst->nparams = src->nparams;
          if (src->nparams > 0) {
            dst->pnames = malloc(sizeof(char *) * (size_t)src->nparams);
            dst->pdefault = malloc(sizeof(int) * (size_t)src->nparams);
            for (int p = 0; p < src->nparams; p++) {
              dst->pnames[p] = src->pnames[p] ? strdup(src->pnames[p]) : NULL;
              dst->pdefault[p] = src->pdefault ? src->pdefault[p] : -1;
            }
            for (int p = 0; p < src->nparams; p++) {
              if (dst->pnames[p]) {
                LocalVar *lv = scope_local_intern(dst, dst->pnames[p]);
                lv->is_param = 1;
              }
            }
          }
          src->is_transplanted_source = 1;
        }
      }
    }
  }
}

/* True if class method scope `mi`'s body contains a bare `new` call (which
   must rebind to the calling subclass, not the defining class). */
static int cmethod_has_bare_new(Compiler *c, int mi) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    if (c->nscope[id] != mi) continue;
    const char *ty = nt_type(nt, id);
    if (ty && !strcmp(ty, "CallNode") && nt_ref(nt, id, "receiver") < 0 &&
        nt_str(nt, id, "name") && !strcmp(nt_str(nt, id, "name"), "new"))
      return 1;
  }
  return 0;
}

/* `Subclass.create` where `create` is an inherited class method whose body
   does `new(...)`: Ruby's bare `new` constructs the *calling* class, so copy
   the inherited cls method into each calling subclass (the copy's class_id
   makes codegen's `new` resolve to that subclass). The defining-class source
   is DCE'd unless it is itself called directly. Covers #224 / #229. */
static void specialize_inherited_cls_new(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int snap = c->nscopes;
  int node_count = nt->count;   /* don't scan nodes appended by cloning */
  int did_clone = 0;
  for (int id = 0; id < node_count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "CallNode")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    const char *rty = nt_type(nt, recv);
    if (!rty || (strcmp(rty, "ConstantReadNode") && strcmp(rty, "ConstantPathNode"))) continue;
    const char *cn = nt_str(nt, recv, "name");
    int ci = cn ? comp_class_index(c, cn) : -1;
    if (ci < 0) continue;
    const char *mname = nt_str(nt, id, "name");
    if (!mname || !strcmp(mname, "new")) continue;
    if (comp_cmethod_in_class(c, ci, mname) >= 0) continue;  /* defined on ci */
    int def_cls = -1;
    int mi = comp_cmethod_in_chain(c, ci, mname, &def_cls);
    if (mi < 0 || def_cls == ci || mi >= snap) continue;     /* not inherited */
    if (!cmethod_has_bare_new(c, mi)) continue;

    /* Copy the inherited cls method scope, owned by ci, with its OWN clone of
       the body AST so locals/dispatches inside resolve against ci (the bare
       `new` constructs ci, and `instance = new` is typed obj_ci). */
    int src_body = c->scopes[mi].body;
    int new_body = src_body >= 0 ? nt_clone_subtree(nt, src_body) : -1;
    if (src_body >= 0 && new_body < 0) continue;  /* clone failed: skip */
    comp_grow_node_arrays(c);
    did_clone = 1;
    Scope *src = &c->scopes[mi];
    Scope *dst = comp_scope_new(c, src->name, src->def_node);
    src = &c->scopes[mi];  /* realloc-safe */
    int dst_idx = c->nscopes - 1;
    dst->body = new_body;
    /* attribute the cloned subtree to the new scope */
    if (new_body >= 0) walk_scope(c, new_body, dst_idx, ci);
    dst->class_id = ci;
    dst->is_cmethod = 1;
    dst->yields = src->yields;
    dst->nrequired = src->nrequired;
    dst->rest_idx = src->rest_idx;
    dst->kwrest_idx = src->kwrest_idx;
    /* The bare `new` returns the specialized subclass, so a create-style
       method that returns its instance is typed as that subclass. */
    dst->ret = ty_object(ci);
    dst->ret_specialized = 1;
    if (src->blk_param) dst->blk_param = strdup(src->blk_param);
    dst->nparams = src->nparams;
    if (src->nparams > 0) {
      dst->pnames = malloc(sizeof(char *) * (size_t)src->nparams);
      dst->pdefault = malloc(sizeof(int) * (size_t)src->nparams);
      for (int p = 0; p < src->nparams; p++) {
        dst->pnames[p] = src->pnames[p] ? strdup(src->pnames[p]) : NULL;
        dst->pdefault[p] = src->pdefault ? src->pdefault[p] : -1;
        if (dst->pnames[p]) { LocalVar *lv = scope_local_intern(dst, dst->pnames[p]); lv->is_param = 1; }
      }
    }
  }
  /* DCE the now-shadowed source cls methods that are never called on their
     own defining class. */
  for (int s = 0; s < snap; s++) {
    Scope *src = &c->scopes[s];
    if (!src->is_cmethod || !src->name || src->class_id < 0) continue;
    /* did we specialize this one into a subclass? (a fresh cmethod copy with
       the same name was appended) */
    int specialized = 0;
    for (int d = snap; d < c->nscopes; d++)
      if (c->scopes[d].is_cmethod && c->scopes[d].name &&
          !strcmp(c->scopes[d].name, src->name)) { specialized = 1; break; }
    if (!specialized) continue;
    /* keep it if called directly as <DefiningClass>.<name> */
    int called_direct = 0;
    for (int id = 0; id < nt->count && !called_direct; id++) {
      if (!nt_type(nt, id) || strcmp(nt_type(nt, id), "CallNode")) continue;
      if (!nt_str(nt, id, "name") || strcmp(nt_str(nt, id, "name"), src->name)) continue;
      int r = nt_ref(nt, id, "receiver");
      if (r < 0 || !nt_type(nt, r)) continue;
      if (strcmp(nt_type(nt, r), "ConstantReadNode") && strcmp(nt_type(nt, r), "ConstantPathNode")) continue;
      if (comp_class_index(c, nt_str(nt, r, "name")) == src->class_id) called_direct = 1;
    }
    if (!called_direct) src->is_transplanted_source = 1;
  }
  /* The cloned bodies introduced new local/ivar nodes; intern them. */
  if (did_clone) register_locals(c);
}

/* For each class, find `prepend M` declarations and transplant M's instance
   methods into the class with shadow-chain renaming so `super` can route
   from M's body to the original (now renamed) class body. */
static void register_prepends(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int ci = 0; ci < c->nclasses; ci++) {
    int body = nt_ref(nt, c->classes[ci].def_node, "body");
    int n = 0;
    const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    for (int k = 0; k < n; k++) {
      int s = stmts[k];
      const char *sty = nt_type(nt, s);
      if (!sty || strcmp(sty, "CallNode")) continue;
      const char *nm = nt_str(nt, s, "name");
      if (!nm || strcmp(nm, "prepend")) continue;
      if (nt_ref(nt, s, "receiver") >= 0) continue;
      int anode = nt_ref(nt, s, "arguments");
      int an = 0;
      const int *args = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
      for (int j = 0; j < an; j++) {
        const char *aty = nt_type(nt, args[j]);
        const char *mname = NULL;
        if (aty && !strcmp(aty, "ConstantReadNode")) mname = nt_str(nt, args[j], "name");
        else if (aty && !strcmp(aty, "ConstantPathNode")) mname = nt_str(nt, args[j], "name");
        int mod_id = mname ? comp_class_index(c, mname) : -1;
        if (mod_id < 0) continue;
        /* Transplant each instance method of the module into class ci. */
        for (int ms = 0; ms < c->nscopes; ms++) {
          Scope *sc = &c->scopes[ms];
          if (sc->class_id != mod_id || sc->is_cmethod || !sc->name) continue;
          const char *method_name = sc->name;
          int active_mi = comp_method_in_class(c, ci, method_name);
          if (active_mi >= 0) {
            Scope *active = &c->scopes[active_mi];
            char shadow[256];
            snprintf(shadow, sizeof shadow, "__prep_%d_%s",
                     c->classes[ci].prep_shadow_count++, method_name);
            /* Rename any existing chain entry for method_name to use shadow. */
            ClassInfo *cif = &c->classes[ci];
            for (int kk = 0; kk < cif->nprep_chain; kk++) {
              if (!strcmp(cif->prep_from[kk], method_name)) {
                free(cif->prep_from[kk]);
                cif->prep_from[kk] = strdup(shadow);
                break;
              }
            }
            /* Rename the currently active scope to the shadow name. */
            free(active->name);
            active->name = strdup(shadow);
            /* Record the new dispatch chain entry: method_name -> shadow. */
            comp_prep_chain_add(&c->classes[ci], method_name, shadow);
          }
          /* Transplant the module scope into class ci. */
          sc->class_id = ci;
        }
      }
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
/* Register each class variable (@@x) in its owning class and infer its type
   from the write sites' RHS. */
static int infer_cvar_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  /* Pass 1: class body-level writes (comp_scope_of returns scope 0, class_id=-1,
     so use the class's def_node to find which class owns them). */
  for (int ci = 0; ci < c->nclasses; ci++) {
    int body = nt_ref(nt, c->classes[ci].def_node, "body");
    int n = 0;
    const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    for (int k = 0; k < n; k++) {
      int s = stmts[k];
      const char *sty = nt_type(nt, s);
      if (!sty) continue;
      if (!strcmp(sty, "ClassVariableWriteNode")) {
        const char *nm = nt_str(nt, s, "name");
        if (!nm) continue;
        int idx = comp_cvar_intern(&c->classes[ci], nm);
        TyKind vt = infer_type(c, nt_ref(nt, s, "value"));
        if (vt == TY_NIL) continue;
        TyKind merged = ty_unify(c->classes[ci].cvar_types[idx], vt);
        if (merged != c->classes[ci].cvar_types[idx]) { c->classes[ci].cvar_types[idx] = merged; changed = 1; }
      }
      else if (!strcmp(sty, "MultiWriteNode")) {
        int mln = 0;
        const int *mlefts = nt_arr(nt, s, "lefts", &mln);
        int mval = nt_ref(nt, s, "value");
        const char *mvty = nt_type(nt, mval);
        int men = 0;
        const int *mels = (mvty && !strcmp(mvty, "ArrayNode")) ? nt_arr(nt, mval, "elements", &men) : NULL;
        for (int mi = 0; mi < mln; mi++) {
          const char *mlty = nt_type(nt, mlefts[mi]);
          if (!mlty || strcmp(mlty, "ClassVariableTargetNode")) continue;
          const char *cnm = nt_str(nt, mlefts[mi], "name");
          if (!cnm) continue;
          int midx = comp_cvar_intern(&c->classes[ci], cnm);
          TyKind mvt2 = (mels && mi < men) ? infer_type(c, mels[mi]) : TY_UNKNOWN;
          if (mvt2 == TY_NIL || mvt2 == TY_UNKNOWN) continue;
          TyKind mmerged = ty_unify(c->classes[ci].cvar_types[midx], mvt2);
          if (mmerged != c->classes[ci].cvar_types[midx]) { c->classes[ci].cvar_types[midx] = mmerged; changed = 1; }
        }
      }
    }
  }
  /* Pass 2: method-level writes (comp_scope_of has class_id set). */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "ClassVariableWriteNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    if (!nm || s->class_id < 0) continue;
    ClassInfo *ci = &c->classes[s->class_id];
    int idx = comp_cvar_intern(ci, nm);
    TyKind vt = infer_type(c, nt_ref(nt, id, "value"));
    if (vt == TY_NIL) continue;
    TyKind merged = ty_unify(ci->cvar_types[idx], vt);
    if (merged != ci->cvar_types[idx]) { ci->cvar_types[idx] = merged; changed = 1; }
  }
  /* Pass 3: top-level writes (class_id == -1 in scope 0) -- use Toplevel pseudo-class. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "ClassVariableWriteNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    if (!nm || s->class_id >= 0) continue;
    int tl_idx = comp_class_index(c, "Toplevel");
    if (tl_idx < 0) { comp_class_new(c, "Toplevel", -1); tl_idx = c->nclasses - 1; }
    ClassInfo *ci = &c->classes[tl_idx];
    int idx = comp_cvar_intern(ci, nm);
    TyKind vt = infer_type(c, nt_ref(nt, id, "value"));
    if (vt == TY_NIL) continue;
    TyKind merged = ty_unify(ci->cvar_types[idx], vt);
    if (merged != ci->cvar_types[idx]) { ci->cvar_types[idx] = merged; changed = 1; }
  }
  return changed;
}

static int infer_ivar_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (!strcmp(ty, "InstanceVariableWriteNode") ||
        !strcmp(ty, "InstanceVariableOrWriteNode") ||
        !strcmp(ty, "InstanceVariableAndWriteNode") ||
        !strcmp(ty, "InstanceVariableOperatorWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      TyKind vt = infer_type(c, nt_ref(nt, id, "value"));
      if (vt == TY_NIL) continue;  /* nil write doesn't pin the ivar type */
      Scope *s = comp_scope_of(c, id);
      int cls_id2 = s->class_id;
      if (!nm) continue;
      if (cls_id2 < 0) {
        /* Top-level method: track ivars in the Toplevel pseudo-class */
        int old_nc = c->nclasses;
        cls_id2 = comp_class_index(c, "Toplevel");
        if (cls_id2 < 0) { comp_class_new(c, "Toplevel", -1); cls_id2 = c->nclasses - 1; }
        if (c->nclasses != old_nc) changed = 1;  /* new class created, need another pass */
      }
      ClassInfo *ci = &c->classes[cls_id2];
      int old_ni = ci->nivars;
      int iv = comp_ivar_intern(ci, nm);
      if (ci->nivars != old_ni) changed = 1;  /* new ivar registered, need another pass */
      /* For operator-write (@b += rhs), vt is the RHS type, not the result type.
         When the slot holds a user object, the result is the method's return type. */
      if (!strcmp(ty, "InstanceVariableOperatorWriteNode") && ty_is_object(ci->ivar_types[iv])) {
        const char *op2 = nt_str(nt, id, "binary_operator");
        int cid2 = ty_object_class(ci->ivar_types[iv]);
        int mi2 = op2 ? comp_method_in_chain(c, cid2, op2, NULL) : -1;
        if (mi2 >= 0 && c->scopes[mi2].ret != TY_UNKNOWN)
          vt = c->scopes[mi2].ret;
        else
          vt = ci->ivar_types[iv];  /* keep existing type, don't widen */
      }
      TyKind merged = ty_unify(ci->ivar_types[iv], vt);
      if (merged != ci->ivar_types[iv]) { ci->ivar_types[iv] = merged; changed = 1; }
      /* Propagate to transplanted copies (module included into a class).
         Body nodes still point to the module scope, so cls_id2 is the module.
         Any scope sharing the same def_node but with a different class_id is
         a transplanted copy that must see the same ivar type. */
      if (s->class_id >= 0 && s->def_node >= 0) {
        int sdef = s->def_node;
        int orig_cid = s->class_id;
        for (int si = 0; si < c->nscopes; si++) {
          Scope *ts = &c->scopes[si];
          if (ts->def_node != sdef || ts->class_id == orig_cid || ts->class_id < 0) continue;
          ClassInfo *tc = &c->classes[ts->class_id];
          int tiv = comp_ivar_intern(tc, nm);
          TyKind tmerged = ty_unify(tc->ivar_types[tiv], vt);
          if (tmerged != tc->ivar_types[tiv]) { tc->ivar_types[tiv] = tmerged; changed = 1; }
        }
      }
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
      int val_id = nt_ref(nt, id, "value");
      newt = infer_type(c, val_id);
      /* a `x = nil` write doesn't pin the type: nil is the absent/default
         value, so the variable takes its non-nil assignments' type */
      if (newt == TY_NIL) newt = TY_POLY;
      /* Empty-collection literal `x = []` / `x = {}` returns TY_UNKNOWN from
         infer_type. If the container-fold from a prior iteration already gave
         this local a meaningful type (stored in gc_root), preserve it so that
         downstream uses like `x.map {...}` are not starved of type information. */
      if (newt == TY_UNKNOWN && nm) {
        const char *vty2 = nt_type(nt, val_id);
        int is_empty_col = vty2 && ((!strcmp(vty2, "ArrayNode") &&
          ({ int _n = 0; nt_arr(nt, val_id, "elements", &_n); _n; }) == 0) ||
          (!strcmp(vty2, "HashNode") &&
          ({ int _n2 = 0; nt_arr(nt, val_id, "elements", &_n2); _n2; }) == 0));
        if (is_empty_col) {
          Scope *s2 = comp_scope_of(c, id);
          LocalVar *lv2 = scope_local(s2, nm);
          if (lv2 && (TyKind)lv2->gc_root != TY_UNKNOWN) newt = (TyKind)lv2->gc_root;
        }
        /* `d = h.dup/clone`: inherit receiver's hash type from prior iteration */
        if (newt == TY_UNKNOWN) {
          const char *rvty2 = nt_type(nt, val_id);
          if (rvty2 && !strcmp(rvty2, "CallNode")) {
            const char *rvnm2 = nt_str(nt, val_id, "name");
            int rvrecv2 = nt_ref(nt, val_id, "receiver");
            if (rvrecv2 >= 0 && rvnm2 &&
                (!strcmp(rvnm2, "dup") || !strcmp(rvnm2, "clone"))) {
              const char *rrt2 = nt_type(nt, rvrecv2);
              if (rrt2 && !strcmp(rrt2, "LocalVariableReadNode")) {
                const char *rrn2 = nt_str(nt, rvrecv2, "name");
                LocalVar *rlv2 = rrn2 ? scope_local(comp_scope_of(c, rvrecv2), rrn2) : NULL;
                if (rlv2 && ty_is_hash((TyKind)rlv2->gc_root)) newt = (TyKind)rlv2->gc_root;
              }
            }
          }
        }
      }
    }
    else if (!strcmp(ty, "LocalVariableOperatorWriteNode")) {
      nm = nt_str(nt, id, "name");
      Scope *s = comp_scope_of(c, id);
      LocalVar *cur = nm ? scope_local(s, nm) : NULL;
      TyKind vt = infer_type(c, nt_ref(nt, id, "value"));
      TyKind ct = cur ? (TyKind)cur->gc_root : TY_UNKNOWN; /* old type */
      if (ct == TY_STRING) newt = TY_STRING;
      else if (ty_is_numeric(ct) && ty_is_numeric(vt)) {
        if (ct == TY_FLOAT || vt == TY_FLOAT) newt = TY_FLOAT;
        else if (ct == TY_BIGINT || vt == TY_BIGINT) newt = TY_BIGINT;
        else newt = TY_INT;
      }
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
    if (!lv || lv->is_block_param) continue;
    /* Params are typed from call sites (monotonic widen); a body assignment
       of a different type widens them too (e.g. `x = "s"` in an int param's
       body -> poly). Only widen -- never let an unknown RHS reset them. */
    if (lv->is_param) {
      if (newt != TY_UNKNOWN) {
        TyKind m2 = ty_unify(lv->type, newt);
        if (m2 != lv->type) { lv->type = m2; changed = 1; }
      }
      continue;
    }
    lv->type = ty_unify(lv->type, newt);
  }

  /* Second targeted pass for `x = recv.instance_eval/exec { ... }` (and
     trampoline calls): the call's value is the block's last expression, which
     may read a block-body local defined at a higher node id than this write.
     Those locals were just typed by the main loop above, so recompute here so
     `x` is not stranded at UNKNOWN by within-pass node ordering. */
  for (int id = 0; id < nt->count; id++) {
    if (strcmp(nt_type(nt, id) ? nt_type(nt, id) : "", "LocalVariableWriteNode")) continue;
    int val_id = nt_ref(nt, id, "value");
    if (val_id < 0 || strcmp(nt_type(nt, val_id) ? nt_type(nt, val_id) : "", "CallNode")) continue;
    if (nt_ref(nt, val_id, "block") < 0) continue;
    const char *vnm = nt_str(nt, val_id, "name");
    int vrecv = nt_ref(nt, val_id, "receiver");
    if (!vnm || vrecv < 0) continue;
    int is_ie = !strcmp(vnm, "instance_eval") || !strcmp(vnm, "instance_exec");
    if (!is_ie) {
      TyKind vrt = infer_type(c, vrecv);
      if (!ty_is_object(vrt) || !comp_trampoline_kind(c, ty_object_class(vrt), vnm, NULL)) continue;
    }
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? scope_local(comp_scope_of(c, id), nm) : NULL;
    if (!lv || lv->is_param || lv->is_block_param) continue;
    TyKind newt = infer_type(c, val_id);
    if (newt == TY_NIL) newt = TY_POLY;
    TyKind m2 = ty_unify(lv->type, newt);
    if (m2 != lv->type) { lv->type = m2; changed = 1; }
  }

  /* Multiple assignment `a, b = e0, e1`: each target gets its element's
     type (the RHS ArrayNode is a tuple here, not an array value). */
  for (int id = 0; id < nt->count; id++) {
    if (strcmp(nt_type(nt, id) ? nt_type(nt, id) : "", "MultiWriteNode")) continue;
    int ln = 0;
    const int *lefts = nt_arr(nt, id, "lefts", &ln);
    int value = nt_ref(nt, id, "value");
    const char *vty = nt_type(nt, value);
    if (!vty || strcmp(vty, "ArrayNode")) {
      /* scalar RHS (`a, b = 1`): the first target gets the scalar, the rest
         their slot default. Type every target as the scalar's kind. Array /
         hash RHS would splat and is handled elsewhere, so skip those. */
      int multi_src = vty && (!strcmp(vty, "CallNode") || !strcmp(vty, "SuperNode") ||
                              !strcmp(vty, "ForwardingSuperNode") || !strcmp(vty, "YieldNode"));
      if (vty && value >= 0 && !multi_src) {
        TyKind st = infer_type(c, value);
        if (st != TY_UNKNOWN && st != TY_NIL && !ty_is_array(st) && !ty_is_hash(st)) {
          for (int i = 0; i < ln; i++) {
            if (strcmp(nt_type(nt, lefts[i]) ? nt_type(nt, lefts[i]) : "", "LocalVariableTargetNode")) continue;
            const char *lnm = nt_str(nt, lefts[i], "name");
            LocalVar *lv = lnm ? scope_local(comp_scope_of(c, id), lnm) : NULL;
            if (!lv || lv->is_param || lv->is_block_param) continue;
            lv->type = ty_unify(lv->type, st);
          }
        }
      }
      /* any expression returning a typed array: assign element types to targets */
      if (value >= 0) {
        TyKind st = infer_type(c, value);
        /* poly RHS: destructure gives poly elements */
        if (st == TY_POLY || st == TY_POLY_ARRAY) {
          Scope *ms_poly = comp_scope_of(c, id);
          for (int i = 0; i < ln; i++) {
            const char *lty_p = nt_type(nt, lefts[i]) ? nt_type(nt, lefts[i]) : "";
            if (!strcmp(lty_p, "LocalVariableTargetNode")) {
              const char *lnm_p = nt_str(nt, lefts[i], "name");
              LocalVar *lv_p = lnm_p ? scope_local(ms_poly, lnm_p) : NULL;
              if (!lv_p || lv_p->is_param || lv_p->is_block_param) continue;
              TyKind mg_p = ty_unify(lv_p->type, TY_POLY);
              if (mg_p != lv_p->type) { lv_p->type = mg_p; changed = 1; }
            }
          }
        }
        if (ty_is_array(st)) {
          TyKind elem = ty_array_elem(st);
          int rn2 = 0;
          const int *rights2 = nt_arr(nt, id, "rights", &rn2);
          Scope *ms_arr = comp_scope_of(c, id);
          for (int i = 0; i < ln; i++) {
            const char *lty_ms = nt_type(nt, lefts[i]) ? nt_type(nt, lefts[i]) : "";
            if (!strcmp(lty_ms, "LocalVariableTargetNode")) {
              const char *lnm = nt_str(nt, lefts[i], "name");
              LocalVar *lv = lnm ? scope_local(ms_arr, lnm) : NULL;
              if (!lv || lv->is_param || lv->is_block_param) continue;
              lv->type = ty_unify(lv->type, elem);
            }
            else if (!strcmp(lty_ms, "InstanceVariableTargetNode") &&
                     ms_arr && ms_arr->class_id >= 0) {
              const char *ivnm = nt_str(nt, lefts[i], "name");
              int iv_ms = ivnm ? comp_ivar_index(&c->classes[ms_arr->class_id], ivnm) : -1;
              if (iv_ms < 0) continue;
              TyKind mg = ty_unify(c->classes[ms_arr->class_id].ivar_types[iv_ms], elem);
              if (mg != c->classes[ms_arr->class_id].ivar_types[iv_ms]) {
                c->classes[ms_arr->class_id].ivar_types[iv_ms] = mg; changed = 1;
              }
            }
            else if (!strcmp(lty_ms, "ConstantTargetNode")) {
              const char *cnm_ms = nt_str(nt, lefts[i], "name");
              LocalVar *cv_ms = cnm_ms ? comp_const(c, cnm_ms) : NULL;
              if (!cv_ms) continue;
              TyKind mg_ms = ty_unify(cv_ms->type, elem);
              if (mg_ms != cv_ms->type) { cv_ms->type = mg_ms; changed = 1; }
            }
          }
          for (int j = 0; j < rn2; j++) {
            const char *lty_ms = nt_type(nt, rights2[j]) ? nt_type(nt, rights2[j]) : "";
            if (!strcmp(lty_ms, "LocalVariableTargetNode")) {
              const char *rnm2 = nt_str(nt, rights2[j], "name");
              LocalVar *lv = rnm2 ? scope_local(ms_arr, rnm2) : NULL;
              if (!lv || lv->is_param || lv->is_block_param) continue;
              lv->type = ty_unify(lv->type, elem);
            }
            else if (!strcmp(lty_ms, "InstanceVariableTargetNode") &&
                     ms_arr && ms_arr->class_id >= 0) {
              const char *ivnm2 = nt_str(nt, rights2[j], "name");
              int iv_ms2 = ivnm2 ? comp_ivar_index(&c->classes[ms_arr->class_id], ivnm2) : -1;
              if (iv_ms2 < 0) continue;
              TyKind mg2 = ty_unify(c->classes[ms_arr->class_id].ivar_types[iv_ms2], elem);
              if (mg2 != c->classes[ms_arr->class_id].ivar_types[iv_ms2]) {
                c->classes[ms_arr->class_id].ivar_types[iv_ms2] = mg2; changed = 1;
              }
            }
            else if (!strcmp(lty_ms, "ConstantTargetNode")) {
              const char *cnm_ms2 = nt_str(nt, rights2[j], "name");
              LocalVar *cv_ms2 = cnm_ms2 ? comp_const(c, cnm_ms2) : NULL;
              if (!cv_ms2) continue;
              TyKind mg_ms2 = ty_unify(cv_ms2->type, elem);
              if (mg_ms2 != cv_ms2->type) { cv_ms2->type = mg_ms2; changed = 1; }
            }
          }
          int rest_nid2 = nt_ref(nt, id, "rest");
          if (rest_nid2 >= 0) {
            const char *rsty2 = nt_type(nt, rest_nid2);
            int inner2 = -1;
            if (rsty2 && !strcmp(rsty2, "SplatNode"))
              inner2 = nt_ref(nt, rest_nid2, "expression");
            if (inner2 >= 0 && nt_type(nt, inner2) &&
                !strcmp(nt_type(nt, inner2), "LocalVariableTargetNode")) {
              const char *rnm3 = nt_str(nt, inner2, "name");
              LocalVar *lv3 = rnm3 ? scope_local(comp_scope_of(c, id), rnm3) : NULL;
              if (lv3 && !lv3->is_param && !lv3->is_block_param)
                lv3->type = ty_unify(lv3->type, st);
            }
          }
        }
      }
      continue;
    }
    int en = 0;
    const int *els = nt_arr(nt, value, "elements", &en);
    for (int i = 0; i < ln && i < en; i++) {
      const char *lty = nt_type(nt, lefts[i]);
      if (!lty) continue;
      if (!strcmp(lty, "LocalVariableTargetNode")) {
        const char *lnm = nt_str(nt, lefts[i], "name");
        TyKind et = infer_type(c, els[i]);
        if (et == TY_NIL) continue;
        LocalVar *lv = lnm ? scope_local(comp_scope_of(c, id), lnm) : NULL;
        if (!lv || lv->is_param || lv->is_block_param) continue;
        lv->type = ty_unify(lv->type, et);
      }
      else if (!strcmp(lty, "ConstantTargetNode")) {
        const char *cnm = nt_str(nt, lefts[i], "name");
        LocalVar *cv = cnm ? comp_const(c, cnm) : NULL;
        if (!cv) continue;
        TyKind et = infer_type(c, els[i]);
        if (et == TY_NIL) continue;
        TyKind mg = ty_unify(cv->type, et);
        if (mg != cv->type) { cv->type = mg; changed = 1; }
      }
      else if (!strcmp(lty, "InstanceVariableTargetNode")) {
        Scope *iv_sc = comp_scope_of(c, id);
        int iv_cid = iv_sc ? iv_sc->class_id : -1;
        if (iv_cid < 0) continue;
        const char *ivnm = nt_str(nt, lefts[i], "name");
        int iv_idx = ivnm ? comp_ivar_index(&c->classes[iv_cid], ivnm) : -1;
        if (iv_idx < 0) continue;
        TyKind et = infer_type(c, els[i]);
        if (et == TY_NIL) continue;
        TyKind mg = ty_unify(c->classes[iv_cid].ivar_types[iv_idx], et);
        if (mg != c->classes[iv_cid].ivar_types[iv_idx]) {
          c->classes[iv_cid].ivar_types[iv_idx] = mg; changed = 1;
        }
      }
      else if (!strcmp(lty, "MultiTargetNode")) {
        /* (b, c) nested target: inner RHS must be an ArrayNode literal */
        const char *ety = nt_type(nt, els[i]);
        if (!ety || strcmp(ety, "ArrayNode")) continue;
        int inn = 0;
        const int *inner_els = nt_arr(nt, els[i], "elements", &inn);
        int inn2 = 0;
        const int *inner_lefts = nt_arr(nt, lefts[i], "lefts", &inn2);
        for (int j = 0; j < inn2 && j < inn; j++) {
          const char *ilty = nt_type(nt, inner_lefts[j]);
          if (!ilty || strcmp(ilty, "LocalVariableTargetNode")) continue;
          const char *lnm2 = nt_str(nt, inner_lefts[j], "name");
          TyKind et2 = infer_type(c, inner_els[j]);
          if (et2 == TY_NIL) continue;
          LocalVar *lv2 = lnm2 ? scope_local(comp_scope_of(c, id), lnm2) : NULL;
          if (!lv2 || lv2->is_param || lv2->is_block_param) continue;
          lv2->type = ty_unify(lv2->type, et2);
        }
      }
    }
    /* rights targets (post-splat fixed targets) */
    int rn = 0;
    const int *rights = nt_arr(nt, id, "rights", &rn);
    for (int j = 0; j < rn; j++) {
      int ridx = en - rn + j;
      if (ridx < 0 || ridx >= en) continue;
      const char *rty3 = nt_type(nt, rights[j]);
      if (!rty3) continue;
      TyKind et = infer_type(c, els[ridx]);
      if (et == TY_NIL) continue;
      if (!strcmp(rty3, "LocalVariableTargetNode")) {
        const char *rnm2 = nt_str(nt, rights[j], "name");
        LocalVar *lv = rnm2 ? scope_local(comp_scope_of(c, id), rnm2) : NULL;
        if (!lv || lv->is_param || lv->is_block_param) continue;
        lv->type = ty_unify(lv->type, et);
      }
      else if (!strcmp(rty3, "ConstantTargetNode")) {
        const char *cnm2 = nt_str(nt, rights[j], "name");
        LocalVar *cv2 = cnm2 ? comp_const(c, cnm2) : NULL;
        if (!cv2) continue;
        TyKind mg3 = ty_unify(cv2->type, et);
        if (mg3 != cv2->type) { cv2->type = mg3; changed = 1; }
      }
      else if (!strcmp(rty3, "InstanceVariableTargetNode")) {
        Scope *iv_sc3 = comp_scope_of(c, id);
        int iv_cid3 = iv_sc3 ? iv_sc3->class_id : -1;
        if (iv_cid3 < 0) continue;
        const char *ivnm3 = nt_str(nt, rights[j], "name");
        int iv_idx3 = ivnm3 ? comp_ivar_index(&c->classes[iv_cid3], ivnm3) : -1;
        if (iv_idx3 < 0) continue;
        TyKind mg4 = ty_unify(c->classes[iv_cid3].ivar_types[iv_idx3], et);
        if (mg4 != c->classes[iv_cid3].ivar_types[iv_idx3]) {
          c->classes[iv_cid3].ivar_types[iv_idx3] = mg4; changed = 1;
        }
      }
    }
    /* rest (splat) target: elements [ln, en-rn) become a typed array */
    int rest_nid = nt_ref(nt, id, "rest");
    if (rest_nid >= 0) {
      const char *rsty = nt_type(nt, rest_nid);
      int inner = -1;
      if (rsty && !strcmp(rsty, "SplatNode"))
        inner = nt_ref(nt, rest_nid, "expression");
      if (inner >= 0 && nt_type(nt, inner) &&
          !strcmp(nt_type(nt, inner), "LocalVariableTargetNode")) {
        const char *rnm = nt_str(nt, inner, "name");
        int rstart = ln, rend = en - rn;
        if (rend < rstart) rend = rstart;
        TyKind rest_elem = TY_UNKNOWN;
        for (int i = rstart; i < rend; i++)
          rest_elem = ty_unify(rest_elem, infer_type(c, els[i]));
        TyKind rest_arr = (rest_elem != TY_UNKNOWN) ? ty_array_of(rest_elem) : TY_INT_ARRAY;
        LocalVar *lv = rnm ? scope_local(comp_scope_of(c, id), rnm) : NULL;
        if (lv && !lv->is_param && !lv->is_block_param)
          lv->type = ty_unify(lv->type, rest_arr);
      }
    }
  }

  /* MatchRequiredNode: `value => pattern` — infer locals from pattern shape. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "MatchRequiredNode")) continue;
    int value = nt_ref(nt, id, "value");
    int pattern = nt_ref(nt, id, "pattern");
    if (value < 0 || pattern < 0) continue;
    const char *pty = nt_type(nt, pattern);
    if (!pty) continue;
    Scope *ms = comp_scope_of(c, id);
    if (!strcmp(pty, "ArrayPatternNode")) {
      int rn = 0;
      const int *reqs = nt_arr(nt, pattern, "requireds", &rn);
      /* Try to get types from a literal ArrayNode value. */
      const char *vty = nt_type(nt, value);
      int en = 0;
      const int *els = (vty && !strcmp(vty, "ArrayNode")) ? nt_arr(nt, value, "elements", &en) : NULL;
      TyKind arr_elem = TY_UNKNOWN;
      if (ty_is_array(infer_type(c, value))) arr_elem = ty_array_elem(infer_type(c, value));
      for (int i = 0; i < rn; i++) {
        const char *lty2 = nt_type(nt, reqs[i]);
        if (!lty2 || strcmp(lty2, "LocalVariableTargetNode")) continue;
        const char *lnm = nt_str(nt, reqs[i], "name");
        LocalVar *lv = lnm ? scope_local(ms, lnm) : NULL;
        if (!lv || lv->is_param || lv->is_block_param) continue;
        TyKind et = (els && i < en) ? infer_type(c, els[i]) : arr_elem;
        if (et == TY_UNKNOWN || et == TY_NIL) continue;
        TyKind mg = ty_unify(lv->type, et);
        if (mg != lv->type) { lv->type = mg; changed = 1; }
      }
    }
    else if (!strcmp(pty, "HashPatternNode")) {
      int pn = 0;
      const int *pelms = nt_arr(nt, pattern, "elements", &pn);
      /* Try to match keys from a literal HashNode value. */
      const char *vty = nt_type(nt, value);
      int vn = 0;
      const int *velms = (vty && !strcmp(vty, "HashNode")) ? nt_arr(nt, value, "elements", &vn) : NULL;
      for (int i = 0; i < pn; i++) {
        const char *ety = nt_type(nt, pelms[i]);
        if (!ety || strcmp(ety, "AssocNode")) continue;
        int pkey = nt_ref(nt, pelms[i], "key");
        int ptgt = nt_ref(nt, pelms[i], "value");
        if (ptgt < 0) continue;
        const char *tty = nt_type(nt, ptgt);
        if (!tty || strcmp(tty, "LocalVariableTargetNode")) continue;
        const char *lnm = nt_str(nt, ptgt, "name");
        LocalVar *lv = lnm ? scope_local(ms, lnm) : NULL;
        if (!lv || lv->is_param || lv->is_block_param) continue;
        /* find matching key in value hash */
        const char *pkey_val = (pkey >= 0 && nt_type(nt, pkey) &&
          !strcmp(nt_type(nt, pkey), "SymbolNode")) ? nt_str(nt, pkey, "value") : NULL;
        TyKind et = TY_UNKNOWN;
        if (pkey_val && velms) {
          for (int j = 0; j < vn; j++) {
            int vkey = nt_ref(nt, velms[j], "key");
            const char *vkty = vkey >= 0 ? nt_type(nt, vkey) : NULL;
            const char *vkval = (vkty && !strcmp(vkty, "SymbolNode")) ? nt_str(nt, vkey, "value") : NULL;
            if (vkval && !strcmp(vkval, pkey_val)) { et = infer_type(c, nt_ref(nt, velms[j], "value")); break; }
          }
        }
        if (et == TY_UNKNOWN || et == TY_NIL) continue;
        TyKind mg = ty_unify(lv->type, et);
        if (mg != lv->type) { lv->type = mg; changed = 1; }
      }
    }
  }

  /* CaseMatchNode: `case X; in PATTERN; ...` — infer locals bound by pattern.
     Handles: bare LV (`in x`), guard (`in x if cond`), capture (`in P => x`),
     and array patterns (`in [first, *rest]` / `in Array(head, *tail)`). */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "CaseMatchNode")) continue;
    int pred = nt_ref(nt, id, "predicate");
    if (pred < 0) continue;
    TyKind scrutinee_t = infer_type(c, pred);
    int cn = 0;
    const int *conds = nt_arr(nt, id, "conditions", &cn);
    for (int ci = 0; ci < cn; ci++) {
      const char *cty = nt_type(nt, conds[ci]);
      if (!cty || strcmp(cty, "InNode")) continue;
      int pat = nt_ref(nt, conds[ci], "pattern");
      if (pat < 0) continue;
      Scope *ms = comp_scope_of(c, conds[ci]);
      const char *pty = nt_type(nt, pat);
      if (!pty) continue;
      int bind_lv_node = -1;
      int array_pat = -1;
      TyKind array_scrutinee = TY_UNKNOWN;
      if (!strcmp(pty, "LocalVariableTargetNode")) {
        /* in x */
        bind_lv_node = pat;
      }
      else if (!strcmp(pty, "IfNode")) {
        /* in x if guard — binding is in IfNode.statements body */
        int stmts = nt_ref(nt, pat, "statements");
        if (stmts >= 0 && nt_type(nt, stmts) &&
            !strcmp(nt_type(nt, stmts), "StatementsNode")) {
          int bn = 0;
          const int *body = nt_arr(nt, stmts, "body", &bn);
          for (int k = 0; k < bn; k++) {
            const char *bty = nt_type(nt, body[k]);
            if (bty && !strcmp(bty, "LocalVariableTargetNode")) {
              bind_lv_node = body[k]; break;
            }
          }
        }
      }
      else if (!strcmp(pty, "CapturePatternNode")) {
        /* in PATTERN => var */
        int tgt = nt_ref(nt, pat, "target");
        if (tgt >= 0 && nt_type(nt, tgt) &&
            !strcmp(nt_type(nt, tgt), "LocalVariableTargetNode"))
          bind_lv_node = tgt;
        /* inner ArrayPatternNode also gets element-level types */
        int val = nt_ref(nt, pat, "value");
        if (val >= 0 && nt_type(nt, val) &&
            !strcmp(nt_type(nt, val), "ArrayPatternNode")) {
          array_pat = val; array_scrutinee = scrutinee_t;
        }
      }
      else if (!strcmp(pty, "ArrayPatternNode")) {
        /* in [first, *rest] or in Array(head, *tail) */
        array_pat = pat; array_scrutinee = scrutinee_t;
      }
      /* Bind simple LV target to scrutinee type */
      if (bind_lv_node >= 0 && scrutinee_t != TY_UNKNOWN) {
        const char *lnm = nt_str(nt, bind_lv_node, "name");
        LocalVar *lv = lnm ? scope_local(ms, lnm) : NULL;
        if (lv && !lv->is_param && !lv->is_block_param) {
          TyKind mg = ty_unify(lv->type, scrutinee_t);
          if (mg != lv->type) { lv->type = mg; changed = 1; }
        }
      }
      /* Handle ArrayPatternNode requireds and rest splat */
      if (array_pat >= 0) {
        TyKind elem_t = ty_is_array(array_scrutinee) ? ty_array_elem(array_scrutinee) : TY_UNKNOWN;
        int apn = 0;
        const int *reqs = nt_arr(nt, array_pat, "requireds", &apn);
        for (int k = 0; k < apn; k++) {
          const char *lty2 = nt_type(nt, reqs[k]);
          if (!lty2 || strcmp(lty2, "LocalVariableTargetNode")) continue;
          const char *lnm = nt_str(nt, reqs[k], "name");
          LocalVar *lv = lnm ? scope_local(ms, lnm) : NULL;
          if (!lv || lv->is_param || lv->is_block_param) continue;
          TyKind et = (elem_t != TY_UNKNOWN) ? elem_t : TY_INT;
          TyKind mg = ty_unify(lv->type, et);
          if (mg != lv->type) { lv->type = mg; changed = 1; }
        }
        /* rest splat: *name gets array type */
        int rest_nid = nt_ref(nt, array_pat, "rest");
        if (rest_nid >= 0) {
          const char *rsty2 = nt_type(nt, rest_nid);
          int inner = -1;
          if (rsty2 && !strcmp(rsty2, "SplatNode"))
            inner = nt_ref(nt, rest_nid, "expression");
          if (inner >= 0 && nt_type(nt, inner) &&
              !strcmp(nt_type(nt, inner), "LocalVariableTargetNode")) {
            const char *rnm = nt_str(nt, inner, "name");
            LocalVar *lv = rnm ? scope_local(ms, rnm) : NULL;
            if (lv && !lv->is_param && !lv->is_block_param) {
              TyKind rest_arr = ty_is_array(array_scrutinee) ? array_scrutinee : TY_INT_ARRAY;
              TyKind mg = ty_unify(lv->type, rest_arr);
              if (mg != lv->type) { lv->type = mg; changed = 1; }
            }
          }
        }
      }
    }
  }

  /* Fold container usage into the local type so an empty `[]` / `{}` gets
     its element / key+value type from how it is filled. `a << x` /
     `a.push(x)` / `a[i] = x` (int key) -> array; `h[k] = v` / `h[k] op= v`
     (string key) -> hash. Part of the recompute frame so it survives reset. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    int recv, kt = TY_UNKNOWN, vt = TY_UNKNOWN, is_push = 0, is_idx_write = 0;
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
        is_idx_write = 1; kt = infer_type(c, argv[0]); vt = infer_type(c, argv[1]);
      }
      else if (name && (!strcmp(name, "fetch") || !strcmp(name, "[]")) && an >= 1) {
        /* hash.fetch(key,..) / hash[key]: promote TY_UNKNOWN local to a typed hash.
           Only fires when the slot is currently TY_UNKNOWN (empty hash). */
        TyKind rslot = TY_UNKNOWN;
        const char *rrty = nt_type(nt, recv);
        const char *rnm2 = NULL;
        if (rrty && !strcmp(rrty, "LocalVariableReadNode")) {
          rnm2 = nt_str(nt, recv, "name");
          LocalVar *lv2 = rnm2 ? scope_local(comp_scope_of(c, recv), rnm2) : NULL;
          if (lv2) rslot = lv2->type;
        }
        else if (rrty && !strcmp(rrty, "InstanceVariableReadNode")) {
          /* handled via slot below if it's TY_UNKNOWN */
        }
        if (rslot != TY_UNKNOWN) continue;  /* already typed, skip */
        /* Only promote via [] read if the receiver local has at least one
           write site in its scope. Pure block params have no write site and
           get their type from infer_block_params; promoting them here to
           TY_STR_POLY_HASH before is_block_param is set creates a TY_POLY
           that ty_unify can never narrow back to the yield arg type. */
        if (rrty && !strcmp(rrty, "LocalVariableReadNode") && rnm2) {
          Scope *recv_scope = comp_scope_of(c, recv);
          int has_write = 0;
          for (int _wi = 0; _wi < nt->count && !has_write; _wi++) {
            const char *_wty = nt_type(nt, _wi);
            if (!_wty) continue;
            if ((!strcmp(_wty, "LocalVariableWriteNode") ||
                 !strcmp(_wty, "LocalVariableOrWriteNode") ||
                 !strcmp(_wty, "LocalVariableAndWriteNode") ||
                 !strcmp(_wty, "LocalVariableOperatorWriteNode")) &&
                comp_scope_of(c, _wi) == recv_scope) {
              const char *_wnm = nt_str(nt, _wi, "name");
              if (_wnm && !strcmp(_wnm, rnm2)) has_write = 1;
            }
          }
          if (!has_write) continue;
        }
        kt = infer_type(c, argv[0]);
        if (kt == TY_SYMBOL) { vt = TY_INT; /* dummy: sym hash val is always poly */ }
        else if (kt == TY_STRING) { vt = TY_POLY; /* will map to STR_POLY */ }
        else if (kt == TY_INT)    { vt = TY_POLY; /* will map to POLY_POLY */ }
        else continue;
      }
      else continue;
    }
    else if (!strcmp(ty, "IndexOperatorWriteNode")) {
      is_idx_write = 1;
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
    if (recv < 0) continue;
    const char *rty = nt_type(nt, recv);
    /* fold into a local's type or an ivar's type (an empty `@buf=[]` filled by
       `@buf << x` infers its element type the same way a local does) */
    TyKind *slot = NULL;
    if (rty && !strcmp(rty, "LocalVariableReadNode")) {
      const char *rnm = nt_str(nt, recv, "name");
      LocalVar *lv = rnm ? scope_local(comp_scope_of(c, recv), rnm) : NULL;
      if (!lv || lv->is_param || lv->is_block_param) continue;
      slot = &lv->type;
    }
    else if (rty && !strcmp(rty, "InstanceVariableReadNode")) {
      const char *inm = nt_str(nt, recv, "name");
      Scope *s = comp_scope_of(c, recv);
      int ivar_cls_id = s->class_id;
      if (ivar_cls_id < 0) ivar_cls_id = comp_class_index(c, "Toplevel");
      if (ivar_cls_id < 0) continue;
      ClassInfo *ci = &c->classes[ivar_cls_id];
      int iv = inm ? comp_ivar_index(ci, inm) : -1;
      if (iv < 0) continue;
      slot = &ci->ivar_types[iv];
      /* If the slot is TY_UNKNOWN but has a direct InstanceVariableWriteNode
         that assigns a typed value OR an empty array/hash literal (e.g.
         @buf = [nil]*7 or @free = []), skip usage-driven hash promotion
         (but allow push-driven array promotion through). Without this guard,
         @free[0] read promotes @free to poly_poly_hash before @free = []
         has been processed as an array. */
      if (!is_push && !is_idx_write && *slot == TY_UNKNOWN && inm) {
        int has_typed_write = 0;
        for (int _wi = 0; _wi < nt->count && !has_typed_write; _wi++) {
          if (!nt_type(nt, _wi) || strcmp(nt_type(nt, _wi), "InstanceVariableWriteNode")) continue;
          const char *_wnm = nt_str(nt, _wi, "name");
          if (!_wnm || strcmp(_wnm, inm)) continue;
          Scope *_ws = comp_scope_of(c, _wi);
          int _ws_cls = _ws ? _ws->class_id : -1;
          if (_ws_cls < 0) _ws_cls = comp_class_index(c, "Toplevel");
          if (_ws_cls != ivar_cls_id) continue;
          int _wval = nt_ref(nt, _wi, "value");
          if (_wval < 0) continue;
          TyKind _wt = infer_type(c, _wval);
          if (_wt != TY_UNKNOWN && _wt != TY_NIL) { has_typed_write = 1; break; }
          /* @ivar = [] literal: this slot is an array, not subject to
             hash-promotion from [] read or [0]= write. Empty {} does NOT
             block promotion — the hash type is determined by key/value usage. */
          const char *_wvty = nt_type(nt, _wval);
          if (_wvty && !strcmp(_wvty, "ArrayNode"))
            has_typed_write = 1;
        }
        if (has_typed_write) continue;
      }
    }
    else if (is_push && rty && !strcmp(rty, "CallNode")) {
      /* `getter_method << x` where getter returns @ivar: trace through
         to that ivar so cross-class lazy-init getters get widened. */
      int recv_args = nt_ref(nt, recv, "arguments");
      int recv_argc = 0;
      if (recv_args >= 0) nt_arr(nt, recv_args, "arguments", &recv_argc);
      if (recv_argc != 0) continue;
      const char *mname = nt_str(nt, recv, "name");
      if (!mname) continue;
      Scope *caller = comp_scope_of(c, recv);
      if (!caller || caller->class_id < 0) continue;
      int defcls2 = caller->class_id;
      int getter_mi = comp_method_in_chain(c, caller->class_id, mname, &defcls2);
      if (getter_mi < 0) continue;
      int last2 = scope_body_last(c, getter_mi);
      if (last2 < 0 || !nt_type(nt, last2) ||
          strcmp(nt_type(nt, last2), "InstanceVariableReadNode")) continue;
      const char *inm2 = nt_str(nt, last2, "name");
      if (!inm2) continue;
      ClassInfo *ci2 = &c->classes[defcls2];
      int iv2 = comp_ivar_index(ci2, inm2);
      if (iv2 < 0) continue;
      slot = &ci2->ivar_types[iv2];
    }
    else continue;

    TyKind before = *slot;
    if (is_push) {
      /* explicit push/append: definitely array.  A PolyArray stays PolyArray
         regardless of the pushed value type; mixing typed arrays widens to
         PolyArray (ty_unify would return TY_POLY scalar, so use array-aware
         widening instead). */
      if (vt == TY_UNKNOWN) continue;
      /* If a [] read already promoted this slot to a hash type, the push
         wins: a variable that is pushed to is an array, not a hash.
         Reset the slot so the array promotion below can fire. */
      if (ty_is_hash(*slot)) *slot = TY_UNKNOWN;
      if (*slot != TY_UNKNOWN && !ty_is_array(*slot)) continue;
      if (*slot == TY_POLY_ARRAY) continue;  /* already widest array type */
      TyKind want = ty_array_of(vt);
      if (*slot != TY_UNKNOWN && want != *slot) want = TY_POLY_ARRAY;
      *slot = want;
    }
    else if (*slot == TY_POLY_POLY_HASH) {
      /* already widest hash type; no further promotion needed */
    }
    else if (kt == TY_INT) {
      /* int key []=: if slot already array, leave it; otherwise infer int-keyed hash */
      if (vt == TY_UNKNOWN) continue;
      if (*slot != TY_UNKNOWN && ty_is_array(*slot)) continue;
      if (*slot != TY_UNKNOWN && !ty_is_hash(*slot)) continue;
      TyKind hv = ty_hash_of(TY_INT, vt);
      if (hv == TY_UNKNOWN) hv = TY_POLY_POLY_HASH;  /* int key + unknown val type */
      if (*slot != TY_UNKNOWN && *slot != hv) {
        /* widen to poly-poly if mismatch */
        if (ty_is_hash(*slot)) { *slot = TY_POLY_POLY_HASH; }
        continue;
      }
      *slot = hv;
    }
    else if (kt == TY_STRING) {
      if (vt == TY_UNKNOWN) continue;
      TyKind hv = ty_hash_of(TY_STRING, vt);
      if (hv == TY_UNKNOWN) hv = TY_STR_POLY_HASH;  /* mixed values */
      if (*slot != TY_UNKNOWN && !ty_is_hash(*slot)) continue;
      /* a str-keyed hash that has seen >1 value type widens to StrPoly */
      if (*slot != TY_UNKNOWN && *slot != hv &&
          (*slot == TY_STR_INT_HASH || *slot == TY_STR_STR_HASH || *slot == TY_STR_POLY_HASH))
        hv = TY_STR_POLY_HASH;
      *slot = hv;
    }
    else if (kt == TY_SYMBOL) {
      /* symbol key -> SymPolyHash (boxed values) */
      if (vt == TY_UNKNOWN) continue;
      if (*slot != TY_UNKNOWN && *slot != TY_SYM_POLY_HASH) continue;
      *slot = TY_SYM_POLY_HASH;
    }
    else if (kt != TY_UNKNOWN) {
      /* non-standard key type (array, object, etc.): heterogeneous hash */
      if (vt == TY_UNKNOWN) continue;
      if (*slot != TY_UNKNOWN && !ty_is_hash(*slot)) continue;
      *slot = TY_POLY_POLY_HASH;
    }
    if (*slot != before) changed = 1;
  }

  /* Second pass: re-compute proc_ret for proc-typed locals after body-internal
     locals have been typed. The first pass resets all locals to TY_UNKNOWN, so
     computing proc_ret there would see stale TY_UNKNOWN for variables assigned
     inside the proc body. Running after the first pass ensures those locals
     have their correct types (e.g. `x = 10` -> TY_INT) before proc_node_ret
     evaluates the body's return type. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "LocalVariableWriteNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
    if (!lv || lv->type != TY_PROC) continue;
    int vnode = nt_ref(nt, id, "value");
    TyKind pr = vnode >= 0 ? proc_ret_of(c, vnode) : TY_UNKNOWN;
    if (pr != TY_UNKNOWN && (TyKind)lv->proc_ret != pr) { lv->proc_ret = (int)pr; changed = 1; }
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
  int changed = 0;
  /* Separate positional args from the trailing keyword-hash arg (if any). */
  int kwh = -1;
  int pos_argc = argc;
  if (argc > 0 && nt_type(nt, argv[argc - 1]) &&
      !strcmp(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) {
    kwh = argv[argc - 1];
    pos_argc = argc - 1;
  }
  /* Don't bind individual args to the *rest slot; it stays TY_POLY_ARRAY. */
  int max_bind = m->nparams;
  if (m->rest_idx >= 0 && max_bind > m->rest_idx) max_bind = m->rest_idx;
  int n = pos_argc < max_bind ? pos_argc : max_bind;
  for (int k = 0; k < n; k++) {
    /* When the call has a single SplatNode covering this fixed param,
       infer the element type of the splatted array instead. */
    TyKind at;
    const char *apty = argv ? nt_type(nt, argv[k]) : NULL;
    if (apty && !strcmp(apty, "SplatNode")) {
      int inner = nt_ref(nt, argv[k], "expression");
      TyKind arr = inner >= 0 ? infer_type(c, inner) : TY_UNKNOWN;
      at = ty_is_array(arr) ? ty_array_elem(arr) : TY_POLY;
    } else {
      at = infer_type(c, argv[k]);
      /* nil is not a first-class param type; a nil arg widens the param to poly */
      if (at == TY_NIL) at = TY_POLY;
    }
    LocalVar *p = scope_local(m, m->pnames[k]);
    if (!p) continue;
    TyKind merged = ty_unify(p->type, at);
    if (merged != p->type) { p->type = merged; changed = 1; }
    if (merged == TY_PROC) {
      TyKind pr = proc_ret_of(c, argv[k]);
      if (pr != TY_UNKNOWN && p->proc_ret != (int)pr) { p->proc_ret = (int)pr; changed = 1; }
    }
  }
  /* Post-splat required params: bind from the end of the positional args. */
  if (m->rest_idx >= 0 && m->npost_rest > 0) {
    for (int j = 0; j < m->npost_rest; j++) {
      int pi = m->rest_idx + 1 + j;
      int ai = pos_argc - m->npost_rest + j;
      if (pi >= m->nparams || ai < 0 || ai >= pos_argc || !argv) continue;
      TyKind at = infer_type(c, argv[ai]);
      if (at == TY_NIL) at = TY_POLY;
      LocalVar *p = scope_local(m, m->pnames[pi]);
      if (!p) continue;
      TyKind merged = ty_unify(p->type, at);
      if (merged != p->type) { p->type = merged; changed = 1; }
    }
  }
  /* Keyword arguments: match KeywordHashNode elements to named params. */
  if (kwh >= 0) {
    int en = 0;
    const int *elems = nt_arr(nt, kwh, "elements", &en);
    /* Check for a double-splat (**h) covering all keyword params. */
    TyKind ds_val = TY_UNKNOWN;
    for (int e = 0; e < en; e++) {
      const char *ety = nt_type(nt, elems[e]);
      if (ety && !strcmp(ety, "AssocSplatNode")) {
        int inner = nt_ref(nt, elems[e], "value");
        if (inner >= 0) {
          TyKind ht = infer_type(c, inner);
          if (ty_is_hash(ht)) ds_val = ty_hash_val(ht);
        }
        break;
      }
    }
    if (ds_val != TY_UNKNOWN) {
      /* Bind all keyword params of the callee from the splat hash value type. */
      TyKind at = (ds_val == TY_POLY) ? TY_POLY : ds_val;
      if (at == TY_NIL) at = TY_POLY;
      for (int i = 0; i < m->nparams; i++) {
        if (!m->pnames[i]) continue;
        LocalVar *p = scope_local(m, m->pnames[i]);
        if (!p) continue;
        TyKind merged = ty_unify(p->type, at);
        if (merged != p->type) { p->type = merged; changed = 1; }
      }
    } else {
      int any_kw_bound = 0;
      for (int e = 0; e < en; e++) {
        int key = nt_ref(nt, elems[e], "key");
        int val = nt_ref(nt, elems[e], "value");
        if (key < 0 || val < 0) continue;
        const char *kty = nt_type(nt, key);
        const char *kname = (kty && !strcmp(kty, "SymbolNode")) ? nt_str(nt, key, "value") : NULL;
        if (!kname) continue;
        LocalVar *p = scope_local(m, kname);
        if (!p) continue;
        TyKind at = infer_type(c, val);
        TyKind merged = ty_unify(p->type, at);
        if (merged != p->type) { p->type = merged; changed = 1; }
        any_kw_bound = 1;
      }
      /* Ruby collapses trailing kwargs into a positional hash parameter when
         the callee has no named keyword params (e.g. `def f(opts = {})`
         called as `f(key: val)`). Bind the next unbound positional param
         to TY_SYM_POLY_HASH so the backstop doesn't kill the method. */
      if (!any_kw_bound && pos_argc < max_bind && max_bind > 0) {
        LocalVar *p = m->pnames[pos_argc] ? scope_local(m, m->pnames[pos_argc]) : NULL;
        if (p) {
          TyKind merged = ty_unify(p->type, TY_SYM_POLY_HASH);
          if (merged != p->type) { p->type = merged; changed = 1; }
        }
      }
    }
  }
  return changed;
}

/* Propagate param types from each prep-chain source scope (the transplanted
   module method) to the shadow scope it calls via super. The shadow scope has
   no AST call site, so bind_call_params never runs for it. */
static int propagate_prep_params(Compiler *c) {
  int changed = 0;
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cls = &c->classes[ci];
    for (int k = 0; k < cls->nprep_chain; k++) {
      const char *from_name = cls->prep_from[k];
      const char *to_name   = cls->prep_to[k];
      int from_mi = comp_method_in_class(c, ci, from_name);
      int to_mi = -1;
      for (int s = 0; s < c->nscopes; s++) {
        if (c->scopes[s].class_id == ci && !c->scopes[s].is_cmethod &&
            c->scopes[s].name && !strcmp(c->scopes[s].name, to_name)) {
          to_mi = s; break;
        }
      }
      if (from_mi < 0 || to_mi < 0) continue;
      Scope *fs = &c->scopes[from_mi];
      Scope *ts = &c->scopes[to_mi];
      int n = fs->nparams < ts->nparams ? fs->nparams : ts->nparams;
      for (int i = 0; i < n; i++) {
        LocalVar *fp = scope_local(fs, fs->pnames[i]);
        LocalVar *tp = scope_local(ts, ts->pnames[i]);
        if (!fp || !tp || fp->type == TY_UNKNOWN) continue;
        TyKind merged = ty_unify(tp->type, fp->type);
        if (merged != tp->type) { tp->type = merged; changed = 1; }
      }
    }
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
      /* An empty hash `{}` default returns TY_UNKNOWN from infer_type; treat
         it as TY_SYM_POLY_HASH since it is used as a kwargs receiver. */
      if (dt == TY_UNKNOWN) {
        const char *dty = nt_type(c->nt, sc->pdefault[i]);
        if (dty && (!strcmp(dty, "HashNode") || !strcmp(dty, "KeywordHashNode"))) {
          int dn = 0; nt_arr(c->nt, sc->pdefault[i], "elements", &dn);
          if (dn == 0) dt = TY_SYM_POLY_HASH;
        }
      }
      if (dt == TY_NIL || dt == TY_UNKNOWN) continue;
      LocalVar *p = scope_local(sc, sc->pnames[i]);
      if (!p) continue;
      TyKind merged = ty_unify(p->type, dt);
      if (merged != p->type) { p->type = merged; changed = 1; }
    }
  }
  return changed;
}

/* Methods that only Strings respond to -- definitive evidence that a
   receiver is a String. (length/size/etc are shared with containers and so
   are deliberately excluded to keep the inference conservative.) */
static int is_string_only_method(const char *m) {
  static const char *const set[] = {
    "split", "strip", "lstrip", "rstrip", "chomp", "chop", "upcase",
    "downcase", "capitalize", "swapcase", "gsub", "sub", "tr", "tr_s",
    "squeeze", "scan", "start_with?", "end_with?", "each_char", "chars",
    "center", "ljust", "rjust", "to_str", "encode", "unpack", "match?",
    "partition", "rpartition", "succ", "hex", "oct", "codepoints", "scrub",
    "crypt", "delete_prefix", "delete_suffix", "casecmp", "casecmp?",
    "force_encoding", NULL };
  for (int i = 0; set[i]; i++) if (!strcmp(m, set[i])) return 1;
  return 0;
}

/* Infer still-unknown params from ivar hash operations in the method body.
   For `def []=(key, val); @h[key] = val; end` where @h is a known hash type,
   infer key/val from the hash's key/value types.  Also handles `[]` reads.
   Runs post-fixpoint so ivar types are stable before this fires. */
static int infer_params_from_ivar_hash_ops(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "CallNode")) continue;
    const char *name = nt_str(nt, id, "name");
    if (!name) continue;
    int is_set = !strcmp(name, "[]=");
    int is_get = !strcmp(name, "[]");
    if (!is_set && !is_get) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    const char *rty = nt_type(nt, recv);
    if (!rty || strcmp(rty, "InstanceVariableReadNode")) continue;
    const char *inm = nt_str(nt, recv, "name");
    if (!inm) continue;
    Scope *s = comp_scope_of(c, id);
    if (!s || s->class_id < 0) continue;
    ClassInfo *ci = &c->classes[s->class_id];
    int iv = comp_ivar_index(ci, inm);
    if (iv < 0) continue;
    TyKind ht = ci->ivar_types[iv];
    if (!ty_is_hash(ht) || ht == TY_POLY_POLY_HASH) continue;
    TyKind hk = ty_hash_key(ht);
    TyKind hv = ty_hash_val(ht);
    int args = nt_ref(nt, id, "arguments");
    int an = 0;
    const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    /* [](key) => key is hash key type; []=(key, val) => key + val */
    if (an >= 1 && argv && hk != TY_UNKNOWN) {
      const char *aty = nt_type(nt, argv[0]);
      if (aty && !strcmp(aty, "LocalVariableReadNode")) {
        const char *anm = nt_str(nt, argv[0], "name");
        LocalVar *lv = anm ? scope_local(s, anm) : NULL;
        if (lv && lv->is_param && lv->type == TY_UNKNOWN) {
          lv->type = hk; changed = 1;
        }
      }
    }
    if (is_set && an >= 2 && argv && hv != TY_UNKNOWN) {
      const char *aty = nt_type(nt, argv[1]);
      if (aty && !strcmp(aty, "LocalVariableReadNode")) {
        const char *anm = nt_str(nt, argv[1], "name");
        LocalVar *lv = anm ? scope_local(s, anm) : NULL;
        if (lv && lv->is_param && lv->type == TY_UNKNOWN) {
          lv->type = hv; changed = 1;
        }
      }
    }
  }
  return changed;
}

/* Infer a still-unknown parameter as a typed hash when the body indexes
   it with a literal key: `param["key"]` → str_poly_hash,
   `param[:sym]` → sym_poly_hash. Runs in the fixpoint alongside
   infer_string_params so methods with no concrete-typed caller still
   resolve their hash param type from body usage. */
static int infer_hash_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  static const char *const hash_only_meths[] = {
    "keys","values","each_pair","merge","has_key?","key?","fetch","store",
    "delete","transform_values","transform_keys","to_h","each_with_object",NULL
  };
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "CallNode")) continue;
    const char *name = nt_str(nt, id, "name");
    if (!name) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    const char *rty = nt_type(nt, recv);
    if (!rty || strcmp(rty, "LocalVariableReadNode")) continue;
    Scope *s = comp_scope_of(c, id);
    LocalVar *lv = scope_local(s, nt_str(nt, recv, "name"));
    if (!lv || !lv->is_param || lv->type != TY_UNKNOWN) continue;
    /* Literal-key [] / fetch: infer specific variant */
    if (!strcmp(name, "[]") || !strcmp(name, "fetch")) {
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an < 1) continue;
      const char *kty = argv ? nt_type(nt, argv[0]) : NULL;
      if (!kty) continue;
      TyKind want = TY_UNKNOWN;
      if (!strcmp(kty, "StringNode") || !strcmp(kty, "InterpolatedStringNode"))
        want = TY_STR_POLY_HASH;
      else if (!strcmp(kty, "SymbolNode"))
        want = TY_SYM_POLY_HASH;
      if (want == TY_UNKNOWN) continue;
      lv->type = want; changed = 1;
      continue;
    }
    /* []=: infer hash variant from key + value types */
    if (!strcmp(name, "[]=")) {
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an < 2) continue;
      TyKind kt2 = infer_type(c, argv[0]);
      TyKind vt2 = infer_type(c, argv[1]);
      TyKind want = TY_UNKNOWN;
      if (kt2 == TY_STRING) {
        want = (vt2 == TY_STRING) ? TY_STR_STR_HASH : TY_STR_POLY_HASH;
      }
      else if (kt2 == TY_SYMBOL) want = TY_SYM_POLY_HASH;
      else if (kt2 == TY_INT)    want = TY_POLY_POLY_HASH;
      if (want == TY_UNKNOWN) continue;
      lv->type = want; changed = 1;
      continue;
    }
    /* Hash-only methods: widen to str_poly_hash (most common variant) */
    for (int k = 0; hash_only_meths[k]; k++) {
      if (!strcmp(name, hash_only_meths[k])) { lv->type = TY_STR_POLY_HASH; changed = 1; break; }
    }
  }
  return changed;
}

/* Infer a still-unknown parameter as poly_array when the body calls an
   array-only method on it: push/pop/shift/unshift/concat/length/size/empty?.
   Does NOT fire on << (overlaps with Integer/String) or arithmetic ops.
   Runs inside the fixpoint so array params without typed callers still resolve. */
static int infer_array_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  static const char *const arr_meths[] = {
    "push","pop","shift","unshift","concat","flatten","compact","transpose",
    "each_with_index","each_with_object","zip","combination","permutation",NULL
  };
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "CallNode")) continue;
    const char *name = nt_str(nt, id, "name");
    if (!name) continue;
    int is_arr = 0;
    for (int k = 0; arr_meths[k]; k++) if (!strcmp(name, arr_meths[k])) { is_arr = 1; break; }
    if (!is_arr) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    const char *rty = nt_type(nt, recv);
    if (!rty || strcmp(rty, "LocalVariableReadNode")) continue;
    Scope *s = comp_scope_of(c, id);
    LocalVar *lv = scope_local(s, nt_str(nt, recv, "name"));
    /* If caller-side [] read already widened this param to a hash type,
       push wins: a param that receives push() is an array, not a hash. */
    if (lv && lv->is_param && (lv->type == TY_UNKNOWN || ty_is_hash(lv->type))) { lv->type = TY_POLY_ARRAY; changed = 1; }
  }
  return changed;
}

/* Infer a still-unknown parameter as String when the body calls a
   String-only method on it (a param with no concrete-typed caller). */
static int infer_string_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "CallNode")) continue;
    const char *name = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");
    if (!name || recv < 0 || !is_string_only_method(name)) continue;
    const char *rty = nt_type(nt, recv);
    if (!rty || strcmp(rty, "LocalVariableReadNode")) continue;
    Scope *s = comp_scope_of(c, id);
    LocalVar *lv = scope_local(s, nt_str(nt, recv, "name"));
    if (lv && lv->is_param && lv->type == TY_UNKNOWN) { lv->type = TY_STRING; changed = 1; }
  }
  return changed;
}

static int infer_param_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (!strcmp(ty, "SuperNode") || !strcmp(ty, "ForwardingSuperNode")) {
      Scope *s = comp_scope_of(c, id);
      if (s->class_id < 0 || !s->name) continue;
      int p = c->classes[s->class_id].parent;
      if (p < 0) continue;
      int pmi = comp_method_in_chain(c, p, s->name, NULL);
      if (pmi < 0) continue;
      if (!strcmp(ty, "ForwardingSuperNode")) {
        /* bare `super` forwards all current params to parent */
        Scope *pm = &c->scopes[pmi];
        int n = s->nparams < pm->nparams ? s->nparams : pm->nparams;
        if (pm->rest_idx >= 0 && n > pm->rest_idx) n = pm->rest_idx;
        for (int k = 0; k < n; k++) {
          LocalVar *src = scope_local(s, s->pnames[k]);
          LocalVar *dst = scope_local(pm, pm->pnames[k]);
          if (!src || !dst) continue;
          TyKind at = src->type;
          if (at == TY_UNKNOWN) continue;
          TyKind mg = ty_unify(dst->type, at);
          if (mg != dst->type) { dst->type = mg; changed = 1; }
        }
      }
      else {
        changed |= bind_call_params(c, id, pmi);
      }
      continue;
    }
    /* op-assign on an object slot: `lv OP= rhs` / `@iv OP= rhs` is an
       implicit call to `lv.OP(rhs)` -- bind the RHS type to the method param. */
    if (!strcmp(ty, "LocalVariableOperatorWriteNode") ||
        !strcmp(ty, "InstanceVariableOperatorWriteNode")) {
      const char *nm  = nt_str(nt, id, "name");
      const char *op  = nt_str(nt, id, "binary_operator");
      int val         = nt_ref(nt, id, "value");
      if (!op || val < 0) continue;
      TyKind slot_t = TY_UNKNOWN;
      if (!strcmp(ty, "LocalVariableOperatorWriteNode")) {
        Scope *s2 = comp_scope_of(c, id);
        LocalVar *lv2 = nm ? scope_local(s2, nm) : NULL;
        slot_t = lv2 ? lv2->type : TY_UNKNOWN;
      }
      else {
        Scope *s2 = comp_scope_of(c, id);
        if (s2->class_id < 0) continue;
        int iidx = nm ? comp_ivar_index(&c->classes[s2->class_id], nm) : -1;
        slot_t = iidx >= 0 ? c->classes[s2->class_id].ivar_types[iidx] : TY_UNKNOWN;
      }
      /* For TY_POLY slots, scan all user classes for a matching operator method. */
      int cid2 = -1;
      if (ty_is_object(slot_t)) cid2 = ty_object_class(slot_t);
      else if (slot_t == TY_POLY) {
        for (int _sc = 0; _sc < c->nclasses; _sc++) {
          if (comp_method_in_chain(c, _sc, op, NULL) >= 0) { cid2 = _sc; break; }
        }
      }
      if (cid2 < 0) continue;
      int mi2 = comp_method_in_chain(c, cid2, op, NULL);
      if (mi2 < 0) continue;
      Scope *ms2 = &c->scopes[mi2];
      if (ms2->nparams < 1) continue;
      LocalVar *pp = scope_local(ms2, ms2->pnames[0]);
      if (!pp) continue;
      TyKind at2 = infer_type(c, val);
      TyKind mg2 = ty_unify(pp->type, at2);
      if (mg2 != pp->type) { pp->type = mg2; changed = 1; }
      continue;
    }
    if (strcmp(ty, "CallNode")) continue;
    const char *name = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");

    /* <method>.call(args): bind the call-site arg types to the target
       method's params (the Method ABI is the only call site for a method
       reached solely via method(:sym)). */
    if (recv >= 0 && name && (!strcmp(name, "call") || !strcmp(name, "[]") || !strcmp(name, "()")) &&
        infer_type(c, recv) == TY_METHOD) {
      int mn = method_recv_node(c, recv);
      int tmi = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
      if (tmi >= 0) changed |= bind_call_params(c, id, tmi);
      continue;
    }

    if (recv < 0) {
      /* bare `new(args)` inside a class method constructs the enclosing
         (possibly specialized) class -> bind args to that class's
         initialize, so the subclass constructor's params get typed. */
      if (name && !strcmp(name, "new")) {
        Scope *s = comp_scope_of(c, id);
        if (s && s->is_cmethod && s->class_id >= 0) {
          int initmi = comp_method_in_chain(c, s->class_id, "initialize", NULL);
          if (initmi >= 0) changed |= bind_call_params(c, id, initmi);
        }
        continue;
      }
      int mi = comp_method_index(c, name);
      int caller_cid = -1;
      /* bare call inside an instance_eval/exec block: dispatch on the
         receiver's class so its params get the call-site arg types. */
      int iec = ie_class_of(c, id);
      if (mi < 0 && iec >= 0) {
        int def_cid = -1;
        mi = comp_method_in_chain(c, iec, name, &def_cid);
        if (mi >= 0) caller_cid = def_cid >= 0 ? def_cid : iec;
      }
      if (mi < 0) {
        Scope *self = comp_scope_of(c, id);
        if (self->class_id >= 0) {
          caller_cid = self->class_id;
          int def_cid = -1;
          mi = comp_method_in_chain(c, self->class_id, name, &def_cid);
          if (mi >= 0 && def_cid >= 0) caller_cid = def_cid;
          /* inside a class method: also check sibling class methods */
          if (mi < 0 && self->is_cmethod)
            mi = comp_cmethod_in_chain(c, self->class_id, name, NULL);
        }
      }
      if (mi < 0) mi = comp_included_method_index(c, name);
      changed |= bind_call_params(c, id, mi);
      /* Propagate to descendant classes that directly override the same method.
         When Base#foo calls bar(arg), and Sub overrides bar, Sub#bar must also
         receive the same arg types so the cls_id-switch dispatch is type-safe.
         Also handles the case where only descendants define the method (mi < 0
         from base chain, e.g. Base.find calls adapter_find defined only in
         Article and Comment descendants). */
      if (caller_cid >= 0) {
        Scope *caller_sc = comp_scope_of(c, id);
        int is_cm = caller_sc ? caller_sc->is_cmethod : 0;
        for (int k = 0; k < c->nclasses; k++) {
          if (k == caller_cid) continue;
          int is_desc = 0;
          for (int p = c->classes[k].parent; p >= 0; p = c->classes[p].parent)
            if (p == caller_cid) { is_desc = 1; break; }
          if (!is_desc) continue;
          int dmi = is_cm ? comp_cmethod_in_class(c, k, name) :
                            comp_method_in_class(c, k, name);
          if (dmi >= 0) changed |= bind_call_params(c, id, dmi);
        }
      }
      continue;
    }
    /* Class.new -> initialize params; Class.cmethod -> cmethod params */
    {
      const char *rty = nt_type(nt, recv);
      /* M::Sub.new(...) — resolve by the final path component */
      if (rty && !strcmp(rty, "ConstantPathNode")) {
        const char *cn = nt_str(nt, recv, "name");
        int ci = cn ? comp_class_index(c, cn) : -1;
        if (ci >= 0 && !strcmp(name, "new")) {
          int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
          if (ucnew >= 0)
            changed |= bind_call_params(c, id, ucnew);
          else
            changed |= bind_call_params(c, id, comp_method_in_chain(c, ci, "initialize", NULL));
        }
        else if (ci >= 0)
          changed |= bind_call_params(c, id, comp_cmethod_in_chain(c, ci, name, NULL));
      }
      if (rty && !strcmp(rty, "ConstantReadNode")) {
        int ci = comp_class_index(c, nt_str(nt, recv, "name"));
        if (ci >= 0) {
          if (!strcmp(name, "new") && c->classes[ci].is_struct) {
            /* Struct construction: positional args set member ivars in order. */
            ClassInfo *cls = &c->classes[ci];
            int args = nt_ref(nt, id, "arguments");
            int an = 0;
            const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
            int kwh = (an == 1 && nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "KeywordHashNode")) ? argv[0] : -1;
            for (int a = 0; a < cls->nivars; a++) {
              /* a member not supplied at this construction can be nil */
              const char *mname = cls->ivars[a] + 1;
              int kn = 0;
              const int *ke = kwh >= 0 ? nt_arr(nt, kwh, "elements", &kn) : NULL;
              int vnode = -1;
              if (kwh >= 0) {
                for (int e = 0; e < kn; e++) {
                  int key = nt_ref(nt, ke[e], "key");
                  if (key >= 0 && nt_type(nt, key) && !strcmp(nt_type(nt, key), "SymbolNode") &&
                      nt_str(nt, key, "value") && !strcmp(nt_str(nt, key, "value"), mname)) { vnode = nt_ref(nt, ke[e], "value"); break; }
                }
              }
              else if (a < an) vnode = argv[a];
              TyKind at = vnode >= 0 ? infer_type(c, vnode) : TY_NIL;
              TyKind m = ty_unify(cls->ivar_types[a], at);
              if (m != cls->ivar_types[a]) { cls->ivar_types[a] = m; changed = 1; }
            }
            continue;
          }
          if (!strcmp(name, "new")) {
            int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
            if (ucnew >= 0)
              changed |= bind_call_params(c, id, ucnew);
            else
              changed |= bind_call_params(c, id, comp_method_in_chain(c, ci, "initialize", NULL));
          }
          else
            changed |= bind_call_params(c, id, comp_cmethod_in_chain(c, ci, name, NULL));
          continue;
        }
      }
      if (!strcmp(name, "new")) continue;
    }
    /* obj.method -> instance method params */
    TyKind rt = infer_type(c, recv);
    if (ty_is_object(rt)) {
      int cid3 = ty_object_class(rt);
      int mi3 = comp_method_in_chain(c, cid3, name, NULL);
      /* Comparable: `a < b` etc. on an object with `<=>` but no direct `<`
         bind the argument to `<=>` param instead. */
      if (mi3 < 0 && (!strcmp(name, "<") || !strcmp(name, ">") ||
                      !strcmp(name, "<=") || !strcmp(name, ">=")))
        mi3 = comp_method_in_chain(c, cid3, "<=>", NULL);
      changed |= bind_call_params(c, id, mi3);
      /* Also propagate to descendant overrides: codegen will emit a cls_id
         switch that calls each override, so each must have the right param
         types. */
      for (int k = 0; k < c->nclasses; k++) {
        int is_desc = 0;
        for (int p = c->classes[k].parent; p >= 0; p = c->classes[p].parent)
          if (p == cid3) { is_desc = 1; break; }
        if (!is_desc) continue;
        int dmi3 = comp_method_in_class(c, k, name);
        if (dmi3 >= 0) changed |= bind_call_params(c, id, dmi3);
      }
    }
    else if (rt == TY_POLY) {
      /* poly receiver: the call may dispatch to any user method of this name,
         so bind every candidate's params (they would otherwise stay UNKNOWN
         and fail to compile). */
      for (int k = 0; k < c->nclasses; k++)
        changed |= bind_call_params(c, id, comp_method_in_chain(c, k, name, NULL));
    }
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
    const char *idx_ty = nt_type(nt, idx);
    /* for a, b in coll: MultiTargetNode with LocalVariableTargetNode children */
    if (idx_ty && !strcmp(idx_ty, "MultiTargetNode")) {
      int ln = 0;
      const int *lefts = nt_arr(nt, idx, "lefts", &ln);
      TyKind ct2 = infer_type(c, coll);
      /* Each destructured variable gets the element type of the inner array,
         or TY_POLY if the collection element is not a concrete typed array. */
      TyKind inner = TY_POLY;
      if (ty_is_array(ct2)) {
        TyKind et2 = ty_array_elem(ct2);
        if (ty_is_array(et2)) inner = ty_array_elem(et2);
      }
      Scope *ms = comp_scope_of(c, idx);
      for (int i = 0; i < ln; i++) {
        const char *lnm = nt_str(nt, lefts[i], "name");
        if (!lnm) continue;
        LocalVar *lv = scope_local_intern(ms, lnm);
        lv->is_block_param = 1;
        if (lv->type != inner) { lv->type = inner; changed = 1; }
      }
      continue;
    }
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
  /* numbered block params: `{ _1 }`, `{ it }` → NumberedParametersNode */
  const char *bpty = nt_type(c->nt, bp);
  if (bpty && !strcmp(bpty, "NumberedParametersNode")) {
    int max = (int)nt_int(c->nt, bp, "maximum", 0);
    if (idx >= max) return NULL;
    static const char *names[] = {"_1","_2","_3","_4","_5","_6","_7","_8","_9"};
    return (idx < 9) ? names[idx] : NULL;
  }
  int pn = nt_ref(c->nt, bp, "parameters");          /* ParametersNode */
  if (pn < 0) return NULL;
  int n = 0;
  const int *reqs = nt_arr(c->nt, pn, "requireds", &n);
  if (idx < n) return nt_str(c->nt, reqs[idx], "name");
  return NULL;
}

int block_param_is_multi(Compiler *c, int block, int idx) {
  int bp = nt_ref(c->nt, block, "parameters");
  if (bp < 0) return 0;
  int pn = nt_ref(c->nt, bp, "parameters");
  if (pn < 0) return 0;
  int n = 0;
  const int *reqs = nt_arr(c->nt, pn, "requireds", &n);
  if (idx >= n) return 0;
  const char *ty = nt_type(c->nt, reqs[idx]);
  return (ty && !strcmp(ty, "MultiTargetNode"));
}

int block_param_multi_count(Compiler *c, int block, int idx) {
  int bp = nt_ref(c->nt, block, "parameters");
  if (bp < 0) return 0;
  int pn = nt_ref(c->nt, bp, "parameters");
  if (pn < 0) return 0;
  int n = 0;
  const int *reqs = nt_arr(c->nt, pn, "requireds", &n);
  if (idx >= n) return 0;
  int lc = 0;
  nt_arr(c->nt, reqs[idx], "lefts", &lc);
  return lc;
}

const char *block_param_multi_leaf(Compiler *c, int block, int idx, int leaf_idx) {
  int bp = nt_ref(c->nt, block, "parameters");
  if (bp < 0) return NULL;
  int pn = nt_ref(c->nt, bp, "parameters");
  if (pn < 0) return NULL;
  int n = 0;
  const int *reqs = nt_arr(c->nt, pn, "requireds", &n);
  if (idx >= n) return NULL;
  int lc = 0;
  const int *lefts = nt_arr(c->nt, reqs[idx], "lefts", &lc);
  if (!lefts || leaf_idx >= lc) return NULL;
  return nt_str(c->nt, lefts[leaf_idx], "name");
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

static int a_proc_params_node(Compiler *c, int create); /* forward decl */

/* Bind block parameter types for supported iteration methods. */
static int infer_block_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;

  /* `->(x, ...) {}` (LambdaNode): its params live in the enclosing scope (no
     separate scope), like block params. Register and type them; default to
     int (the proc-literal slice default) until call-site arg inference lands. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "LambdaNode")) continue;
    int pn = nt_ref(nt, id, "parameters");      /* ParametersNode (1 level, unlike blocks) */
    if (pn < 0) continue;
    int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
    Scope *bs = comp_scope_of(c, id);
    for (int k = 0; k < rn; k++) {
      const char *p = nt_str(nt, reqs[k], "name");
      if (!p) continue;
      LocalVar *lv = scope_local_intern(bs, p); lv->is_block_param = 1;
      if (lv->type == TY_UNKNOWN) { lv->type = TY_INT; changed = 1; }
    }
  }

  /* Hash.new { |hash, key| } : hash is the StrPolyHash, key the string key. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "CallNode")) continue;
    const char *cname = nt_str(nt, id, "name");
    if (!cname || strcmp(cname, "new")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || !nt_type(nt, recv) || strcmp(nt_type(nt, recv), "ConstantReadNode")) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn || strcmp(rn, "Hash")) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0) continue;
    int pn = nt_ref(nt, blk, "parameters");
    if (pn < 0) continue;
    int inner = nt_ref(nt, pn, "parameters");
    int pnode = inner >= 0 ? inner : pn;
    int rnp = 0; const int *reqs = nt_arr(nt, pnode, "requireds", &rnp);
    Scope *bs = comp_scope_of(c, blk);
    for (int k = 0; k < rnp; k++) {
      const char *p = nt_str(nt, reqs[k], "name");
      if (!p) continue;
      TyKind want = (k == 0) ? TY_STR_POLY_HASH : TY_STRING;
      LocalVar *lv = scope_local_intern(bs, p); lv->is_block_param = 1;
      if (lv->type != want) { lv->type = want; changed = 1; }
    }
  }

  /* recv.instance_eval { |me| } : the block params all receive the receiver
     (Ruby yields self), typed as the receiver's object type. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "CallNode")) continue;
    const char *cname = nt_str(nt, id, "name");
    if (!cname) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    TyKind rt = infer_type(c, recv);
    if (!ty_is_object(rt)) continue;
    if (strcmp(cname, "instance_eval") &&
        comp_trampoline_kind(c, ty_object_class(rt), cname, NULL) != 1) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0) continue;
    int pn = nt_ref(nt, blk, "parameters");
    if (pn < 0) continue;
    Scope *bs = comp_scope_of(c, blk);
    const char *pnty = nt_type(nt, pn);
    if (pnty && !strcmp(pnty, "NumberedParametersNode")) {
      /* `{ _1.method }` : _1.._N all receive self (the receiver). */
      int maxn = (int)nt_int(nt, pn, "maximum", 0);
      for (int k = 1; k <= maxn; k++) {
        char nm[8]; snprintf(nm, sizeof nm, "_%d", k);
        LocalVar *lv = scope_local_intern(bs, nm); lv->is_block_param = 1;
        if (lv->type != rt) { lv->type = rt; changed = 1; }
      }
      continue;
    }
    int inner = nt_ref(nt, pn, "parameters");
    int pnode = inner >= 0 ? inner : pn;
    int rnp = 0; const int *reqs = nt_arr(nt, pnode, "requireds", &rnp);
    for (int k = 0; k < rnp; k++) {
      const char *p = nt_str(nt, reqs[k], "name");
      if (!p) continue;
      LocalVar *lv = scope_local_intern(bs, p); lv->is_block_param = 1;
      if (lv->type != rt) { lv->type = rt; changed = 1; }
    }
  }

  /* recv.instance_exec(args) { |params| } : block params take the call-site
     arg types (strict arity). */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "CallNode")) continue;
    const char *cname = nt_str(nt, id, "name");
    if (!cname) continue;
    int xrecv = nt_ref(nt, id, "receiver");
    if (xrecv < 0) continue;
    if (strcmp(cname, "instance_exec")) {
      TyKind xrt = infer_type(c, xrecv);
      if (!ty_is_object(xrt) ||
          comp_trampoline_kind(c, ty_object_class(xrt), cname, NULL) != 2) continue;
    }
    int blk = nt_ref(nt, id, "block");
    if (blk < 0) continue;
    int pn = nt_ref(nt, blk, "parameters");
    if (pn < 0) continue;
    int inner = nt_ref(nt, pn, "parameters");
    int pnode = inner >= 0 ? inner : pn;
    int rnp = 0; const int *reqs = nt_arr(nt, pnode, "requireds", &rnp);
    int iargs = nt_ref(nt, id, "arguments");
    int iac = 0; const int *iav = iargs >= 0 ? nt_arr(nt, iargs, "arguments", &iac) : NULL;
    Scope *bs = comp_scope_of(c, blk);
    for (int k = 0; k < rnp && k < iac; k++) {
      const char *p = nt_str(nt, reqs[k], "name");
      if (!p) continue;
      TyKind at = infer_type(c, iav[k]);
      LocalVar *lv = scope_local_intern(bs, p); lv->is_block_param = 1;
      if (at != TY_UNKNOWN && lv->type != at) { lv->type = at; changed = 1; }
    }
  }

  /* Fiber.new { |first| ... }: the block param receives the resume value,
     which is always a poly (boxed) value at the runtime ABI boundary. */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "CallNode")) continue;
    const char *cname = nt_str(nt, id, "name");
    if (!cname || strcmp(cname, "new")) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || !nt_type(nt, recv) || strcmp(nt_type(nt, recv), "ConstantReadNode")) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn || strcmp(rn, "Fiber")) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0) continue;
    int pn = nt_ref(nt, blk, "parameters");
    if (pn < 0) continue;
    int inner = nt_ref(nt, pn, "parameters");
    int pnode = inner >= 0 ? inner : pn;
    int rnp = 0; const int *reqs = nt_arr(nt, pnode, "requireds", &rnp);
    Scope *bs = comp_scope_of(c, blk);
    for (int k = 0; k < rnp; k++) {
      const char *p = nt_str(nt, reqs[k], "name");
      if (!p) continue;
      LocalVar *lv = scope_local_intern(bs, p); lv->is_block_param = 1;
      if (lv->type == TY_UNKNOWN) { lv->type = TY_POLY; changed = 1; }
    }
  }

  /* Proc/lambda call-site param inference: `f.call(:a)` propagates arg types
     to the proc's params (e.g. `t` gets TY_SYMBOL instead of the default TY_INT). */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "CallNode")) continue;
    const char *cname = nt_str(nt, id, "name");
    if (!cname || (strcmp(cname, "call") && strcmp(cname, "()") && strcmp(cname, "[]"))) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || infer_type(c, recv) != TY_PROC) continue;
    const char *rty = nt_type(nt, recv);
    if (!rty || strcmp(rty, "LocalVariableReadNode")) continue;
    const char *varname = nt_str(nt, recv, "name");
    if (!varname) continue;
    int call_args = nt_ref(nt, id, "arguments");
    int argc = 0; const int *argv = NULL;
    if (call_args >= 0) argv = nt_arr(nt, call_args, "arguments", &argc);
    if (argc == 0) continue;
    Scope *call_scope = comp_scope_of(c, id);
    /* Find proc literal(s) assigned to varname in the same scope */
    for (int w = 0; w < nt->count; w++) {
      const char *wty = nt_type(nt, w);
      if (!wty || strcmp(wty, "LocalVariableWriteNode")) continue;
      const char *wname = nt_str(nt, w, "name");
      if (!wname || strcmp(wname, varname)) continue;
      if (comp_scope_of(c, w) != call_scope) continue;
      int val = nt_ref(nt, w, "value");
      if (val < 0 || !is_proc_create(c, val)) continue;
      int pn = a_proc_params_node(c, val);
      if (pn < 0) continue;
      int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
      Scope *bs = comp_scope_of(c, val);
      for (int k = 0; k < rn && k < argc; k++) {
        const char *p = nt_str(nt, reqs[k], "name");
        if (!p) continue;
        LocalVar *lv = scope_local(bs, p);
        if (!lv) continue;
        TyKind at = infer_type(c, argv[k]);
        if (at == TY_UNKNOWN || at == lv->type) continue;
        TyKind merged = ty_unify(lv->type, at);
        if (merged != lv->type) { lv->type = merged; changed = 1; }
      }
    }
  }

  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "CallNode")) continue;
    int block = nt_ref(nt, id, "block");
    if (block < 0) continue;
    const char *name = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");
    if (!name) continue;

    /* proc {} / lambda {} / Proc.new {}: type the literal's block params.
       Without call-site arg-type inference (a later slice) default required
       params to int -- covers the common arithmetic proc and is overridden
       by any stronger inference that runs first. */
    if (is_proc_literal(c, id)) {
      Scope *bs = comp_scope_of(c, block);
      for (int k = 0; ; k++) {
        const char *bp = block_param_name(c, block, k);
        if (!bp) break;
        LocalVar *lv = scope_local_intern(bs, bp); lv->is_block_param = 1;
        if (lv->type == TY_UNKNOWN) { lv->type = TY_INT; changed = 1; }
      }
      continue;
    }

    /* Array.new(n) { |i| ... }: i is the integer index */
    if (recv >= 0 && !strcmp(name, "new") && nt_type(nt, recv) &&
        !strcmp(nt_type(nt, recv), "ConstantReadNode") && nt_str(nt, recv, "name") &&
        !strcmp(nt_str(nt, recv, "name"), "Array")) {
      const char *p0 = block_param_name(c, block, 0);
      if (p0) { LocalVar *l = scope_local_intern(comp_scope_of(c, block), p0); l->is_block_param = 1;
                if (l->type != TY_INT) { l->type = TY_INT; changed = 1; } }
      continue;
    }

    /* File.open(args) { |f| ... }: f is a TY_POLY file handle */
    if (recv >= 0 && !strcmp(name, "open") && nt_type(nt, recv) &&
        !strcmp(nt_type(nt, recv), "ConstantReadNode") && nt_str(nt, recv, "name") &&
        (!strcmp(nt_str(nt, recv, "name"), "File") ||
         !strcmp(nt_str(nt, recv, "name"), "IO"))) {
      const char *p0 = block_param_name(c, block, 0);
      if (p0) { LocalVar *l = scope_local_intern(comp_scope_of(c, block), p0); l->is_block_param = 1;
                if (l->type != TY_POLY) { l->type = TY_POLY; changed = 1; } }
      continue;
    }

    /* StringIO.open(args) { |io| ... }: io is a StringIO */
    if (recv >= 0 && !strcmp(name, "open") && nt_type(nt, recv) &&
        !strcmp(nt_type(nt, recv), "ConstantReadNode") && nt_str(nt, recv, "name") &&
        !strcmp(nt_str(nt, recv, "name"), "StringIO")) {
      const char *p0 = block_param_name(c, block, 0);
      if (p0) { LocalVar *l = scope_local_intern(comp_scope_of(c, block), p0); l->is_block_param = 1;
                if (l->type != TY_STRINGIO) { l->type = TY_STRINGIO; changed = 1; } }
      continue;
    }

    /* struct.to_h { |k, v| ... }: k is a member symbol, v its (poly) value */
    if (recv >= 0 && !strcmp(name, "to_h")) {
      TyKind rt0 = infer_type(c, recv);
      if (ty_is_object(rt0) && c->classes[ty_object_class(rt0)].is_struct) {
        const char *kp = block_param_name(c, block, 0);
        const char *vp = block_param_name(c, block, 1);
        Scope *bs = comp_scope_of(c, block);
        if (kp) { LocalVar *l = scope_local_intern(bs, kp); l->is_block_param = 1; if (l->type != TY_SYMBOL) { l->type = TY_SYMBOL; changed = 1; } }
        if (vp) { LocalVar *l = scope_local_intern(bs, vp); l->is_block_param = 1; if (l->type != TY_POLY) { l->type = TY_POLY; changed = 1; } }
        continue;
      }
    }

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
        /* Class.new { |...| }: the yielding method is Class#initialize */
        if (mi < 0 && !strcmp(name, "new") &&
            nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode")) {
          const char *cname = nt_str(nt, recv, "name");
          int cid = cname ? comp_class_index(c, cname) : -1;
          if (cid >= 0) mi = comp_method_in_chain(c, cid, "initialize", NULL);
        }
        /* Class.method { ... }: look up the class method */
        if (mi < 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode")) {
          const char *cname = nt_str(nt, recv, "name");
          int cid = cname ? comp_class_index(c, cname) : -1;
          if (cid >= 0) mi = comp_cmethod_in_chain(c, cid, name, NULL);
        }
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
        /* Params beyond the first yield's arity might still be nil if there
           are other yields with fewer args. Find the min yield arity. */
        int min_yc = yc;
        for (int _yi = 0; _yi < nt->count; _yi++) {
          if (!nt_type(nt, _yi) || strcmp(nt_type(nt, _yi), "YieldNode")) continue;
          if (c->nscope[_yi] != mi) continue;
          int _ya = nt_ref(nt, _yi, "arguments");
          int _yc = 0;
          if (_ya >= 0) nt_arr(nt, _ya, "arguments", &_yc);
          if (_yc < min_yc) min_yc = _yc;
        }
        /* Block params at index >= min_yc can receive nil — widen to poly. */
        for (int k = min_yc; ; k++) {
          const char *bp = block_param_name(c, block, k);
          if (!bp) break;
          LocalVar *lv = scope_local_intern(bs, bp); lv->is_block_param = 1;
          TyKind m = ty_unify(lv->type, TY_POLY);
          if (m != lv->type) { lv->type = m; changed = 1; }
        }
        continue;
      }
      /* Method with a named &block param (not inlined): blk_param.call(args)
         inside the method body determines the arg types for the call-site block. */
      if (mi >= 0 && !c->scopes[mi].yields &&
          c->scopes[mi].blk_param && c->scopes[mi].blk_param[0]) {
        const char *bpname = c->scopes[mi].blk_param;
        Scope *bs = comp_scope_of(c, block);
        for (int bid = 0; bid < nt->count; bid++) {
          const char *bty2 = nt_type(nt, bid);
          if (!bty2 || strcmp(bty2, "CallNode")) continue;
          const char *bcn = nt_str(nt, bid, "name");
          if (!bcn || strcmp(bcn, "call")) continue;
          int brecv = nt_ref(nt, bid, "receiver");
          if (brecv < 0) continue;
          const char *brecvty = nt_type(nt, brecv);
          if (!brecvty || strcmp(brecvty, "LocalVariableReadNode")) continue;
          const char *brecvnm = nt_str(nt, brecv, "name");
          if (!brecvnm || strcmp(brecvnm, bpname)) continue;
          if (comp_scope_of(c, bid) != &c->scopes[mi]) continue;
          int ba = nt_ref(nt, bid, "arguments");
          int barc = 0; const int *barg = NULL;
          if (ba >= 0) barg = nt_arr(nt, ba, "arguments", &barc);
          if (barc == 0) continue;
          for (int k = 0; k < barc; k++) {
            const char *bp = block_param_name(c, block, k);
            if (!bp) continue;
            LocalVar *lv = scope_local_intern(bs, bp); lv->is_block_param = 1;
            TyKind at = infer_type(c, barg[k]);
            if (at == TY_UNKNOWN || at == lv->type) continue;
            TyKind merged = ty_unify(lv->type, at);
            if (merged != lv->type) { lv->type = merged; changed = 1; }
          }
        }
        continue;
      }
    }

    if (recv < 0) continue;
    TyKind rt = infer_type(c, recv);
    const char *p0 = block_param_name(c, block, 0);
    if (!p0 && !block_param_is_multi(c, block, 0)) continue;

    /* then / yield_self: block param receives the receiver value */
    if ((!strcmp(name, "then") || !strcmp(name, "yield_self")) && p0) {
      Scope *bs = comp_scope_of(c, block);
      LocalVar *lv = scope_local_intern(bs, p0); lv->is_block_param = 1;
      TyKind m = ty_unify(lv->type, rt);
      if (m != lv->type) { lv->type = m; changed = 1; }
      continue;
    }

    TyKind pt = TY_UNKNOWN;
    if (!strcmp(name, "step") && (rt == TY_INT || rt == TY_FLOAT)) {
      /* a float receiver or float limit/step yields floats */
      int args = nt_ref(nt, id, "arguments");
      int sc = 0; const int *sv = args >= 0 ? nt_arr(nt, args, "arguments", &sc) : NULL;
      int isf = (rt == TY_FLOAT) || (sc >= 1 && infer_type(c, sv[0]) == TY_FLOAT) ||
                (sc >= 2 && infer_type(c, sv[1]) == TY_FLOAT);
      pt = isf ? TY_FLOAT : TY_INT;
    }
    else if ((!strcmp(name, "times") || !strcmp(name, "upto") ||
         !strcmp(name, "downto")) && rt == TY_INT)
      pt = TY_INT;
    else if (rt == TY_POLY && !strcmp(name, "each_line"))
      pt = TY_STRING;  /* File/IO object yielding lines */
    else if (rt == TY_POLY && !strcmp(name, "each_byte"))
      pt = TY_INT;
    else if (rt == TY_STRING && (!strcmp(name, "each_char") || !strcmp(name, "each_line") || !strcmp(name, "upto") ||
                                 !strcmp(name, "chars") || !strcmp(name, "lines")))
      pt = TY_STRING;
    else if (rt == TY_STRING && (!strcmp(name, "gsub") || !strcmp(name, "sub")))
      pt = TY_STRING;  /* block receives the matched substring */
    else if (rt == TY_STRING && (!strcmp(name, "each_byte") || !strcmp(name, "bytes") || !strcmp(name, "codepoints")))
      pt = TY_INT;
    else if (rt == TY_STRING && !strcmp(name, "scan")) {
      /* scan { |m| } yields each match; m is string (no captures) or str_array (captures) */
      int scan_args_id = nt_ref(nt, id, "arguments");
      int scan_argc = 0;
      const int *scan_argv = scan_args_id >= 0 ? nt_arr(nt, scan_args_id, "arguments", &scan_argc) : NULL;
      int has_cap = 0;
      if (scan_argc == 1 && scan_argv) {
        const char *apty = nt_type(nt, scan_argv[0]);
        if (apty && !strcmp(apty, "RegularExpressionNode")) {
          const char *src = nt_str(nt, scan_argv[0], "unescaped");
          if (src && re_has_captures(src)) has_cap = 1;
        }
      }
      pt = has_cap ? TY_STR_ARRAY : TY_STRING;
    }
    else if ((!strcmp(name, "each") || !strcmp(name, "map") || !strcmp(name, "collect") ||
              !strcmp(name, "select") || !strcmp(name, "reject") || !strcmp(name, "filter") ||
              !strcmp(name, "find") || !strcmp(name, "detect") || !strcmp(name, "each_with_index") ||
              !strcmp(name, "sort_by") || !strcmp(name, "find_all") || !strcmp(name, "count") ||
              !strcmp(name, "any?") || !strcmp(name, "all?") || !strcmp(name, "none?") ||
              !strcmp(name, "one?") || !strcmp(name, "sum") || !strcmp(name, "min_by") ||
              !strcmp(name, "max_by") || !strcmp(name, "bsearch")) && rt == TY_RANGE)
      pt = TY_INT;
    /* (range).lazy.select/reject/filter { |x| } : x is an integer range element */
    else if ((!strcmp(name, "select") || !strcmp(name, "reject") || !strcmp(name, "filter")) &&
             rt == TY_UNKNOWN && recv >= 0 &&
             nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
             nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "lazy")) {
      int lsrc = nt_ref(nt, recv, "receiver");
      if (lsrc >= 0 && infer_type(c, lsrc) == TY_RANGE) pt = TY_INT;
    }
    else if ((!strcmp(name, "each") || !strcmp(name, "map") || !strcmp(name, "collect") ||
              !strcmp(name, "select") || !strcmp(name, "reject") || !strcmp(name, "filter") ||
              !strcmp(name, "find") || !strcmp(name, "detect") ||
              !strcmp(name, "max_by") || !strcmp(name, "min_by") || !strcmp(name, "sort_by") ||
              !strcmp(name, "take_while") || !strcmp(name, "drop_while") ||
              !strcmp(name, "reverse_each") || !strcmp(name, "each_entry") ||
              !strcmp(name, "sum") || !strcmp(name, "count") ||
              !strcmp(name, "any?") || !strcmp(name, "all?") || !strcmp(name, "none?") ||
              !strcmp(name, "one?") || !strcmp(name, "each_with_index") ||
              !strcmp(name, "bsearch") || !strcmp(name, "find_index") ||
              !strcmp(name, "map!") || !strcmp(name, "collect!") ||
              !strcmp(name, "select!") || !strcmp(name, "filter!") || !strcmp(name, "reject!") ||
              !strcmp(name, "keep_if") || !strcmp(name, "delete_if") || !strcmp(name, "each_index") ||
              !strcmp(name, "flat_map") || !strcmp(name, "each_with_object") ||
              !strcmp(name, "chunk") || !strcmp(name, "group_by") ||
              !strcmp(name, "tally_by") || !strcmp(name, "min_by_all") ||
              !strcmp(name, "filter_map") || !strcmp(name, "count_by") ||
              !strcmp(name, "partition") || !strcmp(name, "each_slice") ||
              !strcmp(name, "each_cons") || !strcmp(name, "cycle")) &&
             ty_is_array(rt))
      pt = ty_array_elem(rt);
    /* TY_POLY receiver with iteration methods: element type is TY_POLY */
    else if (rt == TY_POLY &&
             (!strcmp(name, "each") || !strcmp(name, "map") || !strcmp(name, "collect") ||
              !strcmp(name, "select") || !strcmp(name, "reject") || !strcmp(name, "find") ||
              !strcmp(name, "detect") || !strcmp(name, "any?") || !strcmp(name, "all?")))
      pt = TY_POLY;

    /* array.each_cons(n) / each_slice(n) { |a, b, ...| } -- a single param
       binds the n-element sub-array; multiple params destructure elements.
       Also handles |(a, b)| destructuring: leaves bind to element type. */
    if ((!strcmp(name, "each_cons") || !strcmp(name, "each_slice")) && ty_is_array(rt)) {
      Scope *es = comp_scope_of(c, block);
      int np = 0; while (block_param_name(c, block, np)) np++;
      if (np == 0 && block_param_is_multi(c, block, 0)) {
        TyKind elem = ty_array_elem(rt);
        int lc = block_param_multi_count(c, block, 0);
        for (int li = 0; li < lc; li++) {
          const char *ln = block_param_multi_leaf(c, block, 0, li);
          if (!ln) continue;
          LocalVar *lp = scope_local_intern(es, ln); lp->is_block_param = 1;
          TyKind m = ty_unify(lp->type, elem);
          if (m != lp->type) { lp->type = m; changed = 1; }
        }
      }
      else {
        for (int pj = 0; pj < np; pj++) {
          const char *pn = block_param_name(c, block, pj);
          LocalVar *lp = scope_local_intern(es, pn); lp->is_block_param = 1;
          TyKind want = (np == 1) ? rt : ty_array_elem(rt);
          TyKind m = ty_unify(lp->type, want);
          if (m != lp->type) { lp->type = m; changed = 1; }
        }
      }
      continue;
    }

    /* array.each_slice(n).map/collect { |x, y, ...| } chain: each block param
       gets the element type of the original array (slice elements).
       array.each_cons(n).map { |pair| } chain: block param gets the array type.
       Also handles |(a, b)| destructuring as the first param. */
    if ((!strcmp(name, "map") || !strcmp(name, "collect")) && rt == TY_UNKNOWN &&
        nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
        nt_str(nt, recv, "name") && (!strcmp(nt_str(nt, recv, "name"), "each_slice") ||
                                     !strcmp(nt_str(nt, recv, "name"), "each_cons")) &&
        nt_ref(nt, recv, "block") < 0) {
      int es_recv2 = nt_ref(nt, recv, "receiver");
      TyKind arr_t2 = es_recv2 >= 0 ? infer_type(c, es_recv2) : TY_UNKNOWN;
      int is_cons2 = !strcmp(nt_str(nt, recv, "name"), "each_cons");
      if (ty_is_array(arr_t2)) {
        TyKind bp_t2 = is_cons2 ? arr_t2 : ty_array_elem(arr_t2);
        if (bp_t2 != TY_UNKNOWN) {
          Scope *es2 = comp_scope_of(c, block);
          int np2 = 0; while (block_param_name(c, block, np2)) np2++;
          if (np2 == 0 && is_cons2 && block_param_is_multi(c, block, 0)) {
            /* |(a, b)| destructuring: each leaf gets element type */
            TyKind elem2 = ty_array_elem(arr_t2);
            if (elem2 != TY_UNKNOWN) {
              int lc2 = block_param_multi_count(c, block, 0);
              for (int li = 0; li < lc2; li++) {
                const char *ln = block_param_multi_leaf(c, block, 0, li);
                if (!ln) continue;
                LocalVar *lp = scope_local_intern(es2, ln); lp->is_block_param = 1;
                TyKind m2 = ty_unify(lp->type, elem2);
                if (m2 != lp->type) { lp->type = m2; changed = 1; }
              }
            }
          }
          else {
            for (int pj2 = 0; pj2 < np2; pj2++) {
              const char *pn2 = block_param_name(c, block, pj2);
              if (!pn2) break;
              LocalVar *lp2 = scope_local_intern(es2, pn2); lp2->is_block_param = 1;
              TyKind m2 = ty_unify(lp2->type, bp_t2);
              if (m2 != lp2->type) { lp2->type = m2; changed = 1; }
            }
          }
          continue;
        }
      }
    }

    /* array.each_cons(n).with_index(off).map { |pair, i| } or { |(a,b), i| } chain */
    if ((!strcmp(name, "map") || !strcmp(name, "collect")) && rt == TY_UNKNOWN &&
        nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "with_index") &&
        nt_ref(nt, recv, "block") < 0) {
      int wi_recv = nt_ref(nt, recv, "receiver");
      if (wi_recv >= 0 && nt_type(nt, wi_recv) && !strcmp(nt_type(nt, wi_recv), "CallNode") &&
          nt_str(nt, wi_recv, "name") && !strcmp(nt_str(nt, wi_recv, "name"), "each_cons") &&
          nt_ref(nt, wi_recv, "block") < 0) {
        int ec_recv = nt_ref(nt, wi_recv, "receiver");
        TyKind ec_arr_t = ec_recv >= 0 ? infer_type(c, ec_recv) : TY_UNKNOWN;
        if (ty_is_array(ec_arr_t)) {
          Scope *wi_es = comp_scope_of(c, block);
          TyKind elem_t = ty_array_elem(ec_arr_t);
          /* p0 is the pair (array) or |(a,b)| multi-target; p1 is the int index */
          const char *idx_p = block_param_name(c, block, 1);
          if (idx_p) {
            LocalVar *ip = scope_local_intern(wi_es, idx_p); ip->is_block_param = 1;
            TyKind im = ty_unify(ip->type, TY_INT);
            if (im != ip->type) { ip->type = im; changed = 1; }
          }
          if (block_param_is_multi(c, block, 0)) {
            /* |(a, b), i|: destructure first multi-target param */
            int lc3 = block_param_multi_count(c, block, 0);
            for (int li = 0; li < lc3; li++) {
              const char *ln = block_param_multi_leaf(c, block, 0, li);
              if (!ln) continue;
              LocalVar *lp = scope_local_intern(wi_es, ln); lp->is_block_param = 1;
              TyKind m3 = ty_unify(lp->type, elem_t);
              if (m3 != lp->type) { lp->type = m3; changed = 1; }
            }
          }
          else {
            /* |pair, i|: pair gets the sub-array type */
            const char *pair_p = block_param_name(c, block, 0);
            if (pair_p) {
              LocalVar *pp = scope_local_intern(wi_es, pair_p); pp->is_block_param = 1;
              TyKind m3 = ty_unify(pp->type, ec_arr_t);
              if (m3 != pp->type) { pp->type = m3; changed = 1; }
            }
          }
          continue;
        }
      }
    }

    /* array.combination(k) { |c| } binds the k-element sub-array (same kind) */
    if (!strcmp(name, "combination") && ty_is_array(rt)) {
      LocalVar *lp = scope_local_intern(comp_scope_of(c, block), p0); lp->is_block_param = 1;
      TyKind m = ty_unify(lp->type, rt);
      if (m != lp->type) { lp->type = m; changed = 1; }
      continue;
    }

    /* array.sort/min/max/minmax/slice_when { |a, b| cmp } -- a comparator block
       binds both parameters to the element type */
    if ((!strcmp(name, "sort") || !strcmp(name, "sort!") || !strcmp(name, "min") || !strcmp(name, "max") ||
         !strcmp(name, "minmax") || !strcmp(name, "slice_when")) && ty_is_array(rt)) {
      Scope *cs = comp_scope_of(c, block);
      for (int pj = 0; pj < 2; pj++) {
        const char *pn = block_param_name(c, block, pj);
        if (!pn) continue;
        LocalVar *lp = scope_local_intern(cs, pn); lp->is_block_param = 1;
        TyKind m = ty_unify(lp->type, ty_array_elem(rt));
        if (m != lp->type) { lp->type = m; changed = 1; }
      }
      continue;
    }

    /* array.reduce(init) { |acc, elem| } or inject: p0=acc type, p1=elem type */
    if ((!strcmp(name, "reduce") || !strcmp(name, "inject")) && ty_is_array(rt)) {
      if (!p0) continue;
      Scope *rs = comp_scope_of(c, block);
      TyKind et2 = ty_array_elem(rt);
      /* Determine accumulator type from initial value argument (if any) */
      int rargs = nt_ref(nt, id, "arguments");
      int rargc = 0;
      const int *rargv = rargs >= 0 ? nt_arr(nt, rargs, "arguments", &rargc) : NULL;
      TyKind acc_t = (rargc > 0 && rargv) ? infer_type(c, rargv[0]) : et2;
      if (acc_t == TY_UNKNOWN) acc_t = et2;
      LocalVar *ap = scope_local_intern(rs, p0); ap->is_block_param = 1;
      TyKind am = ty_unify(ap->type, acc_t);
      if (am != ap->type) { ap->type = am; changed = 1; }
      const char *rp1 = block_param_name(c, block, 1);
      if (rp1) {
        LocalVar *ep2 = scope_local_intern(rs, rp1); ep2->is_block_param = 1;
        TyKind em2 = ty_unify(ep2->type, et2);
        if (em2 != ep2->type) { ep2->type = em2; changed = 1; }
      }
      continue;
    }

    /* array.each_with_index { |x, i| } binds element + int index */
    if (!strcmp(name, "each_with_index") && ty_is_array(rt)) {
      Scope *es = comp_scope_of(c, block);
      if (!p0) continue;
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

    /* array.zip(other) { |a, b| } binds element of recv + element of other */
    if (!strcmp(name, "zip") && ty_is_array(rt)) {
      Scope *zs = comp_scope_of(c, block);
      LocalVar *ep0 = scope_local_intern(zs, p0); ep0->is_block_param = 1;
      TyKind em0 = ty_unify(ep0->type, ty_array_elem(rt));
      if (em0 != ep0->type) { ep0->type = em0; changed = 1; }
      const char *zp1 = block_param_name(c, block, 1);
      if (zp1) {
        int zargs = nt_ref(nt, id, "arguments");
        int zargc = 0; const int *zargv = zargs >= 0 ? nt_arr(nt, zargs, "arguments", &zargc) : NULL;
        TyKind et2 = (zargc > 0 && zargv && ty_is_array(infer_type(c, zargv[0])))
                     ? ty_array_elem(infer_type(c, zargv[0])) : ty_array_elem(rt);
        LocalVar *ep1 = scope_local_intern(zs, zp1); ep1->is_block_param = 1;
        TyKind em1 = ty_unify(ep1->type, et2);
        if (em1 != ep1->type) { ep1->type = em1; changed = 1; }
      }
      continue;
    }

    /* array.each_with_object(init) { |x, acc| } binds element + accumulator */
    if (!strcmp(name, "each_with_object") && ty_is_array(rt)) {
      Scope *es = comp_scope_of(c, block);
      if (p0) {
        TyKind et = ty_array_elem(rt);
        LocalVar *ep = scope_local_intern(es, p0); ep->is_block_param = 1;
        if (!(ty_is_array(ep->type) && !ty_is_array(et))) {
          TyKind em = ty_unify(ep->type, et);
          if (em != ep->type) { ep->type = em; changed = 1; }
        }
      }
      const char *p1_name = block_param_name(c, block, 1);
      if (p1_name) {
        int ewobj_args = nt_ref(nt, id, "arguments");
        int ewobj_argc = 0;
        const int *ewobj_argv = ewobj_args >= 0 ? nt_arr(nt, ewobj_args, "arguments", &ewobj_argc) : NULL;
        if (ewobj_argc > 0 && ewobj_argv) {
          TyKind at = infer_type(c, ewobj_argv[0]);
          if (at == TY_UNKNOWN) {
            const char *a0ty = nt_type(nt, ewobj_argv[0]);
            int an0 = 0;
            if (a0ty && !strcmp(a0ty, "ArrayNode")) nt_arr(nt, ewobj_argv[0], "elements", &an0);
            if (a0ty && !strcmp(a0ty, "ArrayNode") && an0 == 0) at = TY_INT_ARRAY;
          }
          if (at != TY_UNKNOWN) {
            LocalVar *ap = scope_local_intern(es, p1_name); ap->is_block_param = 1;
            TyKind am = ty_unify(ap->type, at);
            if (am != ap->type) { ap->type = am; changed = 1; }
          }
        }
      }
      continue;
    }

    /* hash.merge(other) { |k, v1, v2| } binds key + both conflicting values */
    if (!strcmp(name, "merge") && ty_is_hash(rt)) {
      Scope *ms = comp_scope_of(c, block);
      LocalVar *kp = scope_local_intern(ms, p0); kp->is_block_param = 1;
      TyKind km = ty_unify(kp->type, ty_hash_key(rt));
      if (km != kp->type) { kp->type = km; changed = 1; }
      const char *mp1 = block_param_name(c, block, 1);
      const char *mp2 = block_param_name(c, block, 2);
      const char *mps[2]; mps[0] = mp1; mps[1] = mp2;
      for (int mi2 = 0; mi2 < 2; mi2++) {
        if (!mps[mi2]) continue;
        LocalVar *vp = scope_local_intern(ms, mps[mi2]); vp->is_block_param = 1;
        TyKind vm = ty_unify(vp->type, ty_hash_val(rt));
        if (vm != vp->type) { vp->type = vm; changed = 1; }
      }
      continue;
    }

    /* hash.fetch(key) { |k| } binds the looked-up key */
    if (!strcmp(name, "fetch") && ty_is_hash(rt)) {
      Scope *fs = comp_scope_of(c, block);
      LocalVar *kp = scope_local_intern(fs, p0); kp->is_block_param = 1;
      TyKind km = ty_unify(kp->type, ty_hash_key(rt));
      if (km != kp->type) { kp->type = km; changed = 1; }
      continue;
    }

    /* hash.transform_keys { |k| } binds key; transform_values { |v| } value */
    if ((!strcmp(name, "transform_keys") || !strcmp(name, "transform_values")) && ty_is_hash(rt)) {
      Scope *hs = comp_scope_of(c, block);
      LocalVar *vp = scope_local_intern(hs, p0); vp->is_block_param = 1;
      TyKind want = !strcmp(name, "transform_keys") ? ty_hash_key(rt) : ty_hash_val(rt);
      TyKind vm = ty_unify(vp->type, want);
      if (vm != vp->type) { vp->type = vm; changed = 1; }
      continue;
    }

    /* hash.each_value { |v| } binds value; each_key { |k| } binds key */
    if ((!strcmp(name, "each_value") || !strcmp(name, "each_key")) && ty_is_hash(rt)) {
      Scope *hs = comp_scope_of(c, block);
      LocalVar *vp = scope_local_intern(hs, p0); vp->is_block_param = 1;
      TyKind want = !strcmp(name, "each_value") ? ty_hash_val(rt) : ty_hash_key(rt);
      TyKind vm = ty_unify(vp->type, want);
      if (vm != vp->type) { vp->type = vm; changed = 1; }
      continue;
    }

    /* hash.each / each_pair { |k, v| } or { |(k,v)| } binds two params.
       Also handles each_with_object { |(k,v), memo| } and mutating
       iteration (delete_if / select! / reject! / keep_if). */
    if ((!strcmp(name, "each") || !strcmp(name, "each_pair") || !strcmp(name, "map") ||
         !strcmp(name, "collect") || !strcmp(name, "flat_map") || !strcmp(name, "select") ||
         !strcmp(name, "filter") || !strcmp(name, "reject") || !strcmp(name, "find") ||
         !strcmp(name, "detect") || !strcmp(name, "sort_by") || !strcmp(name, "min_by") ||
         !strcmp(name, "max_by") || !strcmp(name, "count") || !strcmp(name, "sum") ||
         !strcmp(name, "any?") || !strcmp(name, "all?") || !strcmp(name, "none?") ||
         !strcmp(name, "delete_if") || !strcmp(name, "select!") || !strcmp(name, "reject!") ||
         !strcmp(name, "filter!") || !strcmp(name, "keep_if") ||
         !strcmp(name, "each_with_index") || !strcmp(name, "each_with_object")) && ty_is_hash(rt)) {
      Scope *hs = comp_scope_of(c, block);
      /* |(k,v)| or |(k,v), memo| destructuring (MultiTargetNode first param) */
      if (block_param_is_multi(c, block, 0)) {
        int lc = block_param_multi_count(c, block, 0);
        if (lc >= 1) {
          const char *kn = block_param_multi_leaf(c, block, 0, 0);
          if (kn) {
            LocalVar *kp2 = scope_local_intern(hs, kn); kp2->is_block_param = 1;
            TyKind km2 = ty_unify(kp2->type, ty_hash_key(rt));
            if (km2 != kp2->type) { kp2->type = km2; changed = 1; }
          }
        }
        if (lc >= 2) {
          const char *vn = block_param_multi_leaf(c, block, 0, 1);
          if (vn) {
            LocalVar *vp2 = scope_local_intern(hs, vn); vp2->is_block_param = 1;
            TyKind vm2 = ty_unify(vp2->type, ty_hash_val(rt));
            if (vm2 != vp2->type) { vp2->type = vm2; changed = 1; }
          }
        }
        /* for each_with_object: bind the memo param (position 1) */
        if (!strcmp(name, "each_with_object")) {
          const char *mp = block_param_name(c, block, 1);
          if (mp) {
            int ewobj_args = nt_ref(nt, id, "arguments");
            int ewobj_argc = 0;
            const int *ewobj_argv = ewobj_args >= 0 ? nt_arr(nt, ewobj_args, "arguments", &ewobj_argc) : NULL;
            if (ewobj_argc > 0 && ewobj_argv) {
              TyKind at2 = infer_type(c, ewobj_argv[0]);
              if (at2 != TY_UNKNOWN) {
                LocalVar *mp_lv = scope_local_intern(hs, mp); mp_lv->is_block_param = 1;
                TyKind mm = ty_unify(mp_lv->type, at2);
                if (mm != mp_lv->type) { mp_lv->type = mm; changed = 1; }
              }
            }
          }
        }
      }
      else {
        if (p0) {
          LocalVar *kp = scope_local_intern(hs, p0); kp->is_block_param = 1;
          TyKind km = ty_unify(kp->type, ty_hash_key(rt));
          if (km != kp->type) { kp->type = km; changed = 1; }
        }
        const char *p1 = block_param_name(c, block, 1);
        if (p1) {
          LocalVar *vp = scope_local_intern(hs, p1); vp->is_block_param = 1;
          TyKind vm = ty_unify(vp->type, ty_hash_val(rt));
          if (vm != vp->type) { vp->type = vm; changed = 1; }
        }
      }
      continue;
    }

    /* array.each/map with 2+ params: auto-destructure sub-array elements.
       Handles `[[1,2],[3,4]].each { |a,b| }` and numbered `{ _1; _2 }`. */
    if (pt != TY_UNKNOWN && ty_is_array(rt)) {
      int np = 0;
      while (block_param_name(c, block, np)) np++;
      if (np >= 2) {
        TyKind inner_elem = TY_UNKNOWN;
        if (ty_is_array(pt)) {
          inner_elem = ty_array_elem(pt);
        }
        else if (pt == TY_POLY && recv >= 0) {
          const char *rty2 = nt_type(nt, recv);
          if (rty2 && !strcmp(rty2, "ArrayNode")) {
            int re_n2 = 0;
            const int *re_els2 = nt_arr(nt, recv, "elements", &re_n2);
            TyKind common_at = TY_UNKNOWN;
            for (int ri = 0; ri < re_n2; ri++)
              common_at = ty_unify(common_at, infer_type(c, re_els2[ri]));
            if (ty_is_array(common_at)) inner_elem = ty_array_elem(common_at);
            else inner_elem = TY_POLY;
          }
          else { inner_elem = TY_POLY; }
        }
        if (inner_elem != TY_UNKNOWN) {
          Scope *ds = comp_scope_of(c, block);
          for (int pj = 0; pj < np; pj++) {
            const char *pname2 = block_param_name(c, block, pj);
            if (!pname2) continue;
            LocalVar *lp2 = scope_local_intern(ds, pname2); lp2->is_block_param = 1;
            TyKind m2 = ty_unify(lp2->type, inner_elem);
            if (m2 != lp2->type) { lp2->type = m2; changed = 1; }
          }
          continue;
        }
      }
    }

    if (pt == TY_UNKNOWN) continue;
    Scope *s = comp_scope_of(c, block);
    /* When iterating a poly receiver (TY_POLY) with 2+ block params, all params
       are poly (auto-splat from the poly element). Assign TY_POLY to all. */
    if (pt == TY_POLY) {
      int npp2 = 0; while (block_param_name(c, block, npp2)) npp2++;
      if (npp2 >= 2) {
        for (int pj2 = 0; pj2 < npp2; pj2++) {
          const char *pnj2 = block_param_name(c, block, pj2);
          if (!pnj2) continue;
          LocalVar *lp2 = scope_local_intern(s, pnj2); lp2->is_block_param = 1;
          TyKind m2 = ty_unify(lp2->type, TY_POLY);
          if (m2 != lp2->type) { lp2->type = m2; changed = 1; }
        }
        continue;
      }
    }
    if (!p0) continue;
    LocalVar *lv = scope_local_intern(s, p0); lv->is_block_param = 1;
    /* Don't widen an array-typed variable to a scalar via block-param
       inference.  When the variable already holds an array (set by a write
       site in the same iteration, before infer_block_params runs), widening
       it to the element scalar type collapses the outer array type to TY_POLY.
       Codegen emits a scoped shadow for the block param instead. */
    if (ty_is_array(lv->type) && !ty_is_array(pt))
      continue;
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
  if (n > 1) return TY_POLY_ARRAY;
  return n > 0 ? infer_type(c, a[0]) : TY_NIL;
}

static int infer_return_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  /* implicit return: the body's value */
  for (int s = 1; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    /* Specialized inherited-cls-new copies keep their fixed subclass return
       type (the shared body's bare `new` would otherwise infer the base). */
    if (sc->ret_specialized) continue;
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
    /* When the method returns a proc, record the proc's body return type so a
       caller's `m.call(...)` resolves its result type (factory pattern). */
    if (r == TY_PROC) {
      TyKind pr = TY_UNKNOWN;
      if (sc->body >= 0) {
        int bn = 0; const int *bb = nt_arr(nt, sc->body, "body", &bn);
        if (bn > 0) pr = proc_ret_of(c, bb[bn - 1]);
      }
      for (int id = 0; id < nt->count; id++) {
        const char *ty = nt_type(nt, id);
        if (ty && !strcmp(ty, "ReturnNode") && comp_scope_of(c, id) == sc) {
          int a = nt_ref(nt, id, "arguments"); int an = 0;
          const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
          if (an > 0) pr = ty_unify(pr == TY_UNKNOWN ? TY_UNKNOWN : pr, proc_ret_of(c, av[0]));
        }
      }
      if (pr != TY_UNKNOWN && sc->ret_proc_ret != (int)pr) { sc->ret_proc_ret = (int)pr; changed = 1; }
    }
  }
  return changed;
}

/* Collect CallNode names in the subtree rooted at `id`, stopping at nested
   DefNodes (which are separate method scopes). `out` / `n` / `cap` are
   the dynamic string array to append to. */
static void cr_collect_calls(const NodeTable *nt, int id,
                              char ***out, int *n, int *cap) {
  if (id < 0) return;
  const char *ty = nt_type(nt, id);
  if (!ty) return;
  if (!strcmp(ty, "DefNode")) return;          /* don't enter nested methods */
  /* Collect method name from CallNode, or operator name from op-assign nodes
     (e.g. `a += 1` → InstanceVariableOperatorWriteNode with binary_operator "+"). */
  const char *nm = NULL;
  if (!strcmp(ty, "CallNode")) {
    nm = nt_str(nt, id, "name");
    /* `method(:foo)` takes a reference to foo without calling it; the target
       must still be emitted, so treat the symbol arg as a called name. */
    if (nm && !strcmp(nm, "method")) {
      int margs = nt_ref(nt, id, "arguments");
      int man = 0; const int *mav = margs >= 0 ? nt_arr(nt, margs, "arguments", &man) : NULL;
      if (man >= 1) {
        const char *aty = nt_type(nt, mav[0]);
        const char *msym = NULL;
        if (aty && !strcmp(aty, "SymbolNode")) msym = nt_str(nt, mav[0], "value");
        else if (aty && !strcmp(aty, "StringNode")) { msym = nt_str(nt, mav[0], "content"); if (!msym) msym = nt_str(nt, mav[0], "unescaped"); }
        if (msym) {
          int found = 0;
          for (int i = 0; i < *n; i++) if (!strcmp((*out)[i], msym)) { found = 1; break; }
          if (!found) {
            if (*n >= *cap) { *cap = *cap ? *cap * 2 : 8; *out = realloc(*out, sizeof(char *) * (size_t)*cap); }
            (*out)[(*n)++] = strdup(msym);
          }
        }
      }
    }
  }
  else {
    size_t tl = strlen(ty);
    if (tl > 17 && (!strcmp(ty + tl - 17, "OperatorWriteNode")))
      nm = nt_str(nt, id, "binary_operator");
  }
  if (nm) {
    int found = 0;
    for (int i = 0; i < *n; i++) if (!strcmp((*out)[i], nm)) { found = 1; break; }
    if (!found) {
      if (*n >= *cap) { *cap = *cap ? *cap * 2 : 8; *out = realloc(*out, sizeof(char *) * (size_t)*cap); }
      (*out)[(*n)++] = strdup(nm);
    }
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(nt, id, i); if (ch >= 0) cr_collect_calls(nt, ch, out, n, cap); }
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) { int nn = 0; const int *ids = nt_arr_at(nt, id, i, &nn); for (int k = 0; k < nn; k++) if (ids[k] >= 0) cr_collect_calls(nt, ids[k], out, n, cap); }
}

/* Mark each method scope reachable via transitive call-graph BFS.
   Scope 0 (top level), every `initialize`, and implicitly-called methods
   are roots. Any method reachable from a root (directly or transitively)
   is marked live; others are dead-code-eliminated. */
static void compute_reachable(Compiler *c) {
  /* Build per-scope call sets (CallNode names, not entering nested DefNodes). */
  char ***scope_calls = calloc((size_t)c->nscopes, sizeof(char **));
  int   *sc_n        = calloc((size_t)c->nscopes, sizeof(int));
  int   *sc_cap      = calloc((size_t)c->nscopes, sizeof(int));
  for (int s = 0; s < c->nscopes; s++) {
    if (c->scopes[s].body >= 0)
      cr_collect_calls(c->nt, c->scopes[s].body, &scope_calls[s], &sc_n[s], &sc_cap[s]);
    /* Also scan parameter defaults (e.g. def foo(opt = bar)) — these emit calls
       within the method scope but live in the DefNode parameters subtree. */
    if (c->scopes[s].def_node >= 0) {
      int pn = nt_ref(c->nt, c->scopes[s].def_node, "parameters");
      if (pn >= 0)
        cr_collect_calls(c->nt, pn, &scope_calls[s], &sc_n[s], &sc_cap[s]);
    }
  }

  /* Names that may be invoked implicitly (no explicit CallNode): keep live. */
  static const char *const implicit[] = {
    "to_s", "inspect", "==", "<=>", "eql?", "hash", "each", "coerce",
    "to_str", "to_ary", "to_a", "to_i", "to_int", "to_h", "to_proc", "call", NULL };

  /* BFS queue (scope indices). */
  int *queue = malloc((size_t)c->nscopes * sizeof(int));
  int qhead = 0, qtail = 0;

  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    sc->reachable = 0;
    int is_root = (s == 0 || !sc->name || !strcmp(sc->name, "initialize"));
    if (!is_root)
      for (int i = 0; implicit[i]; i++) if (!strcmp(implicit[i], sc->name)) { is_root = 1; break; }
    if (is_root) { sc->reachable = 1; queue[qtail++] = s; }
  }

  /* "called_names" tracks every method name reached from any reachable scope.
     Used by alias and prep_to propagation (aliases have no scope of their own). */
  char **called_names = NULL; int cn_n = 0, cn_cap = 0;
  #define CN_ADD(NM) do { const char *_n=(NM); if(_n){ int _f=0; \
    for(int _i=0;_i<cn_n;_i++) if(!strcmp(called_names[_i],_n)){_f=1;break;} \
    if(!_f){if(cn_n>=cn_cap){cn_cap=cn_cap?cn_cap*2:32;called_names=realloc(called_names,sizeof(char*)*cn_cap);} \
    called_names[cn_n++]=strdup(_n);}} } while(0)

  /* Helper: mark a name reachable — all scopes with that name join the BFS. */
  #define MARK_NAME(NM) do { const char *_mn=(NM); if(_mn){ CN_ADD(_mn); \
    for(int _t=0;_t<c->nscopes;_t++) \
      if(!c->scopes[_t].reachable&&c->scopes[_t].name&&!strcmp(c->scopes[_t].name,_mn)) \
        { c->scopes[_t].reachable=1; queue[qtail++]=_t; } } } while(0)

  while (qhead < qtail) {
    int s = queue[qhead++];
    for (int ni = 0; ni < sc_n[s]; ni++) MARK_NAME(scope_calls[s][ni]);
  }

  /* Alias/prep_to propagation: when alias_new (or alias_old) is in called_names,
     make the counterpart reachable too (aliases have no scope of their own). */
  int changed = 1;
  while (changed) {
    changed = 0;
    for (int ci = 0; ci < c->nclasses; ci++) {
      ClassInfo *cls = &c->classes[ci];
      for (int i = 0; i < cls->naliases; i++) {
        const char *an = cls->alias_new[i], *ao = cls->alias_old[i];
        int an_live = 0, ao_live = 0;
        for (int j = 0; j < cn_n; j++) {
          if (an && !strcmp(called_names[j], an)) an_live = 1;
          if (ao && !strcmp(called_names[j], ao)) ao_live = 1;
        }
        /* also check reachable scope names (covers scope-backed aliases) */
        for (int s = 0; s < c->nscopes; s++) {
          if (c->scopes[s].reachable && c->scopes[s].name) {
            if (an && !strcmp(c->scopes[s].name, an)) an_live = 1;
            if (ao && !strcmp(c->scopes[s].name, ao)) ao_live = 1;
          }
        }
        if (an_live && !ao_live) {
          int prev_qtail = qtail;
          MARK_NAME(ao);
          if (qtail > prev_qtail) changed = 1;
          /* drain newly enqueued scopes */
          while (qhead < qtail) {
            int s = queue[qhead++];
            for (int ni = 0; ni < sc_n[s]; ni++) MARK_NAME(scope_calls[s][ni]);
          }
        }
        if (ao_live && !an_live) {
          int prev_qtail = qtail;
          MARK_NAME(an);
          if (qtail > prev_qtail) changed = 1;
          while (qhead < qtail) {
            int s = queue[qhead++];
            for (int ni = 0; ni < sc_n[s]; ni++) MARK_NAME(scope_calls[s][ni]);
          }
        }
      }
      for (int i = 0; i < cls->nprep_chain; i++) {
        const char *pf = cls->prep_from[i]; /* user-facing name, e.g. "hi" */
        const char *pt = cls->prep_to[i];   /* shadow name, e.g. "__prep_0_hi" */
        if (!pf || !pt) continue;
        /* When the user-facing name is called, the codegen wrapper calls the shadow
           implementation directly — so mark the shadow reachable too. */
        int pf_in_called = 0;
        for (int j = 0; j < cn_n; j++) if (!strcmp(called_names[j], pf)) { pf_in_called = 1; break; }
        if (!pf_in_called) {
          for (int s = 0; s < c->nscopes; s++)
            if (c->scopes[s].reachable && c->scopes[s].name && !strcmp(c->scopes[s].name, pf)) { pf_in_called = 1; break; }
        }
        if (pf_in_called) {
          int prev_qtail = qtail;
          MARK_NAME(pt);
          if (qtail > prev_qtail) { changed = 1;
            while (qhead < qtail) { int s=queue[qhead++]; for(int ni=0;ni<sc_n[s];ni++) MARK_NAME(scope_calls[s][ni]); }
          }
        }
      }
    }
  }

  for (int i = 0; i < cn_n; i++) free(called_names[i]);
  free(called_names);
  #undef CN_ADD
  #undef MARK_NAME

  /* Cleanup. */
  for (int s = 0; s < c->nscopes; s++) {
    for (int i = 0; i < sc_n[s]; i++) free(scope_calls[s][i]);
    free(scope_calls[s]);
  }
  free(scope_calls); free(sc_n); free(sc_cap); free(queue);
}

/* ---- proc capture detection (closures) ----
   A local read inside a proc body that isn't bound by the proc (param or a
   local the body itself writes) is a captured/free variable; its enclosing
   local must live in a heap cell so the closure and the enclosing scope share
   mutable storage. Mark those enclosing locals is_cell. */
typedef struct { const char **v; int n, cap; } ANameSet;
static int aname_has(ANameSet *s, const char *nm) {
  if (!nm) return 1;
  for (int i = 0; i < s->n; i++) if (!strcmp(s->v[i], nm)) return 1;
  return 0;
}
static void aname_add(ANameSet *s, const char *nm) {
  if (aname_has(s, nm)) return;
  if (s->n >= s->cap) { s->cap = s->cap ? s->cap * 2 : 8; s->v = realloc(s->v, sizeof(char *) * (size_t)s->cap); }
  s->v[s->n++] = nm;
}
static int a_nested_block(const char *ty) { return ty && (!strcmp(ty, "BlockNode") || !strcmp(ty, "LambdaNode")); }
static int a_is_local_node(const char *ty) {
  return ty && (!strcmp(ty, "LocalVariableReadNode") || !strcmp(ty, "LocalVariableWriteNode") ||
                !strcmp(ty, "LocalVariableTargetNode") || !strcmp(ty, "LocalVariableOperatorWriteNode") ||
                !strcmp(ty, "LocalVariableOrWriteNode") || !strcmp(ty, "LocalVariableAndWriteNode"));
}
static int a_is_write_node(const char *ty) {
  return ty && (!strcmp(ty, "LocalVariableWriteNode") || !strcmp(ty, "LocalVariableTargetNode") ||
                !strcmp(ty, "LocalVariableOperatorWriteNode") || !strcmp(ty, "LocalVariableOrWriteNode") ||
                !strcmp(ty, "LocalVariableAndWriteNode"));
}
/* Mark every node id in the subtree (crossing nested blocks: a node inside an
   inner block is still "inside a proc"). */
static void a_mark_subtree(Compiler *c, int id, char *inproc) {
  if (id < 0) return;
  inproc[id] = 1;
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(c->nt, id, i); if (ch >= 0) a_mark_subtree(c, ch, inproc); }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n); for (int k = 0; k < n; k++) if (ids[k] >= 0) a_mark_subtree(c, ids[k], inproc); }
}
/* Names used (read or written) directly in the proc body, not crossing nested
   blocks. */
static void a_collect_used(Compiler *c, int id, ANameSet *out) {
  if (id < 0) return;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return;
  if (a_is_local_node(ty)) aname_add(out, nt_str(c->nt, id, "name"));
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(c->nt, id, i); if (ch >= 0 && !a_nested_block(nt_type(c->nt, ch))) a_collect_used(c, ch, out); }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n); for (int k = 0; k < n; k++) if (ids[k] >= 0 && !a_nested_block(nt_type(c->nt, ids[k]))) a_collect_used(c, ids[k], out); }
}
static int a_proc_params_node(Compiler *c, int create) {
  const char *ty = nt_type(c->nt, create);
  if (ty && !strcmp(ty, "LambdaNode")) return nt_ref(c->nt, create, "parameters");
  int block = nt_ref(c->nt, create, "block");
  if (block < 0) return -1;
  int bp = nt_ref(c->nt, block, "parameters");
  return bp < 0 ? -1 : nt_ref(c->nt, bp, "parameters");
}
static int a_proc_body(Compiler *c, int create) {
  const char *ty = nt_type(c->nt, create);
  if (ty && !strcmp(ty, "LambdaNode")) return nt_ref(c->nt, create, "body");
  int block = nt_ref(c->nt, create, "block");
  return block >= 0 ? nt_ref(c->nt, block, "body") : -1;
}
/* A name used inside a proc is captured iff it belongs to the enclosing scope:
   it is an enclosing parameter, or it is assigned somewhere in the enclosing
   scope OUTSIDE any proc body. (A name assigned only inside the proc is a
   proc-local, not a capture -- Ruby's block-local rule.) Captured enclosing
   locals get a heap cell. */
static void mark_proc_captures(Compiler *c) {
  const NodeTable *nt = c->nt;
  char *inproc = (char *)calloc((size_t)nt->count, 1);
  if (!inproc) return;
  for (int id = 0; id < nt->count; id++)
    if (is_proc_create(c, id)) { int body = a_proc_body(c, id); if (body >= 0) a_mark_subtree(c, body, inproc); }

  for (int id = 0; id < nt->count; id++) {
    if (!is_proc_create(c, id)) continue;
    int body = a_proc_body(c, id);
    if (body < 0) continue;
    int encl = c->nscope[id];
    ANameSet params = {0}, used = {0};
    int pn = a_proc_params_node(c, id);
    if (pn >= 0) { int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn); for (int k = 0; k < rn; k++) aname_add(&params, nt_str(nt, reqs[k], "name")); }
    a_collect_used(c, body, &used);
    Scope *es = &c->scopes[encl];
    for (int u = 0; u < used.n; u++) {
      const char *nm = used.v[u];
      if (aname_has(&params, nm)) continue;          /* the proc's own param */
      LocalVar *lv = scope_local(es, nm);
      if (!lv) continue;                              /* not an enclosing local */
      int owned = lv->is_param;
      for (int w = 0; w < nt->count && !owned; w++) {
        if (c->nscope[w] != encl || inproc[w]) continue;
        if (!a_is_write_node(nt_type(nt, w))) continue;
        const char *wn = nt_str(nt, w, "name");
        if (wn && !strcmp(wn, nm)) owned = 1;
      }
      if (owned) lv->is_cell = 1;
    }
    free(params.v); free(used.v);
  }
  free(inproc);
}

/* ---- bigint loop-variable detection ---- */
/* Scan a while-loop body for `x = x * y` or `x *= y` patterns and collect
   the variable names in a heap-allocated array. Returns the count; caller
   must free the returned array. */
static void bigint_scan_body(const NodeTable *nt, int id, char ***names, int *n, int *cap) {
  if (id < 0) return;
  const char *ty = nt_type(nt, id);
  if (!ty) return;
  /* x *= y  (LocalVariableOperatorWriteNode with * or **) */
  if (!strcmp(ty, "LocalVariableOperatorWriteNode")) {
    const char *op = nt_str(nt, id, "binary_operator");
    if (op && (!strcmp(op, "*") || !strcmp(op, "**"))) {
      const char *nm = nt_str(nt, id, "name");
      if (nm) {
        for (int k = 0; k < *n; k++) if (!strcmp((*names)[k], nm)) goto skip_mul;
        if (*n >= *cap) { *cap = *cap * 2 + 4; *names = (char **)realloc(*names, (size_t)*cap * sizeof(char *)); }
        (*names)[(*n)++] = (char *)nm;
        skip_mul:;
      }
    }
  }
  /* x = x * y  (LocalVariableWriteNode where value is CallNode * with recv = x) */
  if (!strcmp(ty, "LocalVariableWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int val = nt_ref(nt, id, "value");
    if (nm && val >= 0 && !strcmp(nt_type(nt, val) ? nt_type(nt, val) : "", "CallNode")) {
      const char *op2 = nt_str(nt, val, "name");
      int recv2 = nt_ref(nt, val, "receiver");
      if (op2 && (!strcmp(op2, "*") || !strcmp(op2, "**")) && recv2 >= 0 &&
          !strcmp(nt_type(nt, recv2) ? nt_type(nt, recv2) : "", "LocalVariableReadNode") &&
          !strcmp(nt_str(nt, recv2, "name") ? nt_str(nt, recv2, "name") : "", nm)) {
        for (int k = 0; k < *n; k++) if (!strcmp((*names)[k], nm)) goto skip_lv;
        if (*n >= *cap) { *cap = *cap * 2 + 4; *names = (char **)realloc(*names, (size_t)*cap * sizeof(char *)); }
        (*names)[(*n)++] = (char *)nm;
        skip_lv:;
      }
    }
  }
  /* Recurse into body / stmts / subsequent */
  bigint_scan_body(nt, nt_ref(nt, id, "body"), names, n, cap);
  int sn = 0; const int *stmts2 = nt_arr(nt, id, "body", &sn);
  for (int k = 0; k < sn; k++) bigint_scan_body(nt, stmts2[k], names, n, cap);
  bigint_scan_body(nt, nt_ref(nt, id, "subsequent"), names, n, cap);
}

static void detect_bigint_loop_vars(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "WhileNode")) continue;
    int body = nt_ref(nt, id, "statements");
    if (body < 0) continue;
    char **cands = NULL; int ncands = 0, cap = 0;
    bigint_scan_body(nt, body, &cands, &ncands, &cap);
    /* Promote matching TY_INT locals to TY_BIGINT */
    for (int k = 0; k < ncands; k++) {
      Scope *s = comp_scope_of(c, id);
      LocalVar *lv = s ? scope_local(s, cands[k]) : NULL;
      if (lv && lv->type == TY_INT) lv->type = TY_BIGINT;
    }
    free(cands);
  }
}

/* After detect_bigint_loop_vars promotes some locals to TY_BIGINT, cascade
   the promotion to variables assigned from bigint-typed expressions. */
static void propagate_bigint_cascade(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 1;
  while (changed) {
    changed = 0;
    for (int id = 0; id < nt->count; id++) {
      const char *ty = nt_type(nt, id);
      if (!ty) continue;
      if (!strcmp(ty, "LocalVariableWriteNode")) {
        const char *nm = nt_str(nt, id, "name");
        Scope *s = comp_scope_of(c, id);
        LocalVar *lv = nm ? scope_local(s, nm) : NULL;
        if (!lv || lv->type != TY_INT) continue;
        TyKind vt = infer_type(c, nt_ref(nt, id, "value"));
        if (vt == TY_BIGINT) { lv->type = TY_BIGINT; changed = 1; }
      }
      else if (!strcmp(ty, "LocalVariableOperatorWriteNode")) {
        const char *nm = nt_str(nt, id, "name");
        Scope *s = comp_scope_of(c, id);
        LocalVar *lv = nm ? scope_local(s, nm) : NULL;
        if (!lv || lv->type != TY_INT) continue;
        TyKind vt = infer_type(c, nt_ref(nt, id, "value"));
        if (vt == TY_BIGINT) { lv->type = TY_BIGINT; changed = 1; }
      }
    }
  }
}

/* For nodes inside an instance_eval/exec block, the receiver's class id; -1
   elsewhere. Lets bare calls/ivar refs in the block resolve against the
   receiver's class during inference (codegen mirrors this via g_ie_class_id). */
static int *g_ie_node_class = NULL;

static void mark_ie_subtree(Compiler *c, int node, int cls) {
  if (node < 0) return;
  const char *ty = nt_type(c->nt, node);
  if (!ty) return;
  /* a nested def/class starts a fresh self; don't bleed the rebind into it */
  if (!strcmp(ty, "DefNode") || !strcmp(ty, "ClassNode") || !strcmp(ty, "ModuleNode")) return;
  g_ie_node_class[node] = cls;
  int nr = nt_num_refs(c->nt, node);
  for (int i = 0; i < nr; i++) mark_ie_subtree(c, nt_ref_at(c->nt, node, i), cls);
  int na = nt_num_arrs(c->nt, node);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, node, i, &n); for (int k = 0; k < n; k++) mark_ie_subtree(c, ids[k], cls); }
}

/* (Re)build the instance_eval/exec node→class map from current receiver types. */
static void build_ie_map(Compiler *c) {
  const NodeTable *nt = c->nt;
  if (!g_ie_node_class) g_ie_node_class = malloc(sizeof(int) * (size_t)nt->count);
  for (int i = 0; i < nt->count; i++) g_ie_node_class[i] = -1;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int recv = nt_ref(nt, id, "receiver");
    int blk = nt_ref(nt, id, "block");
    if (recv < 0 || blk < 0) continue;
    TyKind rt = infer_type(c, recv);
    if (!ty_is_object(rt)) continue;
    int cls = ty_object_class(rt);
    if (strcmp(nm, "instance_eval") && strcmp(nm, "instance_exec")) {
      /* not a direct instance_eval/exec: maybe a trampoline method on `cls`? */
      if (!comp_trampoline_kind(c, cls, nm, NULL)) continue;
    }
    int body = nt_ref(nt, blk, "body");
    if (body >= 0) mark_ie_subtree(c, body, cls);
  }
}

/* The receiver class for a node inside an instance_eval/exec block, or -1. */
static int ie_class_of(Compiler *c, int node) {
  (void)c;
  return (g_ie_node_class && node >= 0) ? g_ie_node_class[node] : -1;
}

void analyze_program(Compiler *c) {
  /* scope 0 = top level */
  Scope *top = comp_scope_new(c, NULL, -1);
  top->body = nt_ref(c->nt, c->nt->root_id, "statements");

  walk_scope(c, c->nt->root_id, 0, -1);
  register_structs(c);
  fix_struct_block_scopes(c);
  register_module_functions(c);
  register_locals(c);
  register_attrs(c);
  register_aliases(c);
  register_undefs(c);
  register_globals_consts(c);
  register_ffi_decls(c);

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
  register_includes(c);
  register_extends(c);
  register_prepends(c);
  specialize_inherited_cls_new(c);

  /* collect top-level `include <Mod>` calls so bare method calls can
     resolve to module_function methods in those modules. */
  {
    const NodeTable *nt = c->nt;
    int root_stmts = nt_ref(nt, nt->root_id, "statements");
    int sn = 0;
    const int *stmts = root_stmts >= 0 ? nt_arr(nt, root_stmts, "body", &sn) : NULL;
    for (int i = 0; i < sn; i++) {
      if (!nt_type(nt, stmts[i]) || strcmp(nt_type(nt, stmts[i]), "CallNode")) continue;
      if (!nt_str(nt, stmts[i], "name") || strcmp(nt_str(nt, stmts[i], "name"), "include")) continue;
      if (nt_ref(nt, stmts[i], "receiver") >= 0) continue;
      int anode = nt_ref(nt, stmts[i], "arguments");
      int an = 0;
      const int *args = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
      for (int j = 0; j < an; j++) {
        const char *aty = nt_type(nt, args[j]);
        const char *mname = NULL;
        if (aty && !strcmp(aty, "ConstantReadNode")) mname = nt_str(nt, args[j], "name");
        else if (aty && !strcmp(aty, "ConstantPathNode")) mname = nt_str(nt, args[j], "name");
        int ci = mname ? comp_class_index(c, mname) : -1;
        if (ci < 0) continue;
        c->toplevel_includes = realloc(c->toplevel_includes,
                                       sizeof(int) * (size_t)(c->ntoplevel_includes + 1));
        c->toplevel_includes[c->ntoplevel_includes++] = ci;
      }
    }
  }

  /* mark block-aware methods (contain yield or block_given?) -- these are
     inlined at every call site so block_given? reflects the actual site */
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (!ty) continue;
    if (!strcmp(ty, "YieldNode")) comp_scope_of(c, id)->yields = 1;
    else if (!strcmp(ty, "CallNode")) {
      int r = nt_ref(c->nt, id, "receiver");
      const char *rty = r >= 0 ? nt_type(c->nt, r) : NULL;
      int self_or_none = r < 0 || (rty && !strcmp(rty, "SelfNode"));
      const char *nm = nt_str(c->nt, id, "name");
      if (self_or_none && nm && !strcmp(nm, "block_given?")) comp_scope_of(c, id)->yields = 1;
    }
  }

  /* `&block` + block.call: a method whose block parameter never escapes
     (every read is a `.call` receiver or a `&block` forward) is inlined at
     its call sites exactly like a yielding method. The block-param slot is
     then virtual -- the literal block flows in like an implicit yield. */
  for (int mi = 0; mi < c->nscopes; mi++) {
    Scope *m = &c->scopes[mi];
    if (!m->blk_param) continue;
    /* instance_eval/exec trampolines are inlined at call sites by their own
       dedicated splice; don't treat the &block forward as a yield here. */
    if (m->class_id >= 0 && !m->is_cmethod && m->name &&
        comp_trampoline_kind(c, m->class_id, m->name, NULL)) continue;
    /* Anonymous `&`: nameless, so it can only be forwarded -- always safe
       to inline (there is no escaping read to worry about). */
    if (!m->blk_param[0]) { m->yields = 1; continue; }
    /* Mark nodes inside proc/lambda bodies nested within this method.
       A blk_param read there is a real capture-escape: the proc runs
       independently and needs blk to live in a heap cell. */
    char *inproc_m = (char *)calloc((size_t)c->nt->count, 1);
    if (inproc_m) {
      for (int id = 0; id < c->nt->count; id++) {
        if (!is_proc_create(c, id)) continue;
        if (comp_scope_of(c, id) != m) continue;
        int body = a_proc_body(c, id);
        if (body >= 0) a_mark_subtree(c, body, inproc_m);
      }
    }
    int escapes = 0, uses = 0;
    for (int id = 0; id < c->nt->count && !escapes; id++) {
      const char *ty = nt_type(c->nt, id);
      if (!ty || strcmp(ty, "LocalVariableReadNode")) continue;
      if (comp_scope_of(c, id) != m) continue;
      const char *nm = nt_str(c->nt, id, "name");
      if (!nm || strcmp(nm, m->blk_param)) continue;
      /* A read inside a nested proc body is a capture-escape: the proc
         holds a reference to blk independently of the call site. */
      if (inproc_m && inproc_m[id]) { escapes = 1; break; }
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
    free(inproc_m);
    if (!escapes && uses > 0) {
      /* Don't mark yields=1 if the method has an explicit return: emit_inlined_call
         would reject inlining anyway (scope_has_return), but the method would then
         be skipped in emission because yields=1 -- causing undefined references. */
      int has_ret = 0;
      for (int id2 = 0; id2 < c->nt->count && !has_ret; id2++) {
        const char *ty2 = nt_type(c->nt, id2);
        if (ty2 && !strcmp(ty2, "ReturnNode") && comp_scope_of(c, id2) == m) has_ret = 1;
      }
      if (!has_ret) m->yields = 1;
    }
  }

  /* intern every symbol literal so codegen can emit the id table */
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (ty && !strcmp(ty, "SymbolNode")) {
      const char *v = nt_str(c->nt, id, "value");
      if (v) comp_sym_intern(c, v);
    }
    /* __method__ / __callee__ yield the enclosing method's name as a symbol;
       intern it now so the id table is sized before the codegen prologue */
    else if (ty && !strcmp(ty, "CallNode") && nt_ref(c->nt, id, "receiver") < 0) {
      const char *nm = nt_str(c->nt, id, "name");
      if (nm && (!strcmp(nm, "__method__") || !strcmp(nm, "__callee__"))) {
        Scope *s = comp_scope_of(c, id);
        if (s && s->name && s->name[0]) comp_sym_intern(c, s->name);
      }
    }
  }
  /* Proc#parameters reports param kinds (:req/:opt) and names as symbols;
     intern them now so they land in the table before the codegen prologue. */
  for (int id = 0; id < c->nt->count; id++) {
    if (!is_proc_create(c, id)) continue;
    comp_sym_intern(c, "req");
    comp_sym_intern(c, "opt");
    int pn = a_proc_params_node(c, id);
    if (pn < 0) continue;
    int rn = 0; const int *reqs = nt_arr(c->nt, pn, "requireds", &rn);
    for (int k = 0; k < rn; k++) { const char *nm = nt_str(c->nt, reqs[k], "name"); if (nm) comp_sym_intern(c, nm); }
  }

  for (int iter = 0; iter < 128; iter++) {
    int ch = 0;
    build_ie_map(c);  /* refresh instance_exec receiver-class map each pass */
    ch |= infer_write_types(c);
    ch |= infer_param_types(c);
    ch |= propagate_prep_params(c);
    ch |= infer_string_params(c);
    ch |= infer_default_param_types(c);
    ch |= infer_block_params(c);
    ch |= infer_for_index(c);
    ch |= infer_ivar_types(c);
    ch |= infer_cvar_types(c);
    ch |= infer_inherited_ivars(c);
    ch |= infer_global_const_types(c);
    ch |= infer_multiwrite_const_types(c);
    ch |= infer_return_types(c);
    if (!ch) break;
  }

  /* Backstop: a parameter still unknown but with a `= nil` default is a
     nullable param -- represent it as poly so it can hold nil or a value.
     Also widen TY_SYMBOL/TY_BOOL params: those types have no nil sentinel
     and must be boxed into poly when the nil default is reachable. */
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    for (int i = 0; i < sc->nparams; i++) {
      if (sc->pdefault[i] < 0) continue;
      const char *dty = nt_type(c->nt, sc->pdefault[i]);
      if (!dty || strcmp(dty, "NilNode")) continue;
      LocalVar *p = scope_local(sc, sc->pnames[i]);
      if (!p) continue;
      if (p->type == TY_UNKNOWN || p->type == TY_SYMBOL || p->type == TY_BOOL)
        p->type = TY_POLY;
    }
  }

  /* Backstop: transplanted module scopes share the same def_node. If one
     copy has known param types (from call sites) but another copy lacks callers
     and has TY_UNKNOWN params, propagate the known types across. */
  for (int s1 = 0; s1 < c->nscopes; s1++) {
    Scope *sc1 = &c->scopes[s1];
    if (sc1->nparams == 0 || sc1->def_node < 0 || !sc1->name) continue;
    for (int pi = 0; pi < sc1->nparams; pi++) {
      if (!sc1->pnames[pi]) continue;
      LocalVar *p1 = scope_local(sc1, sc1->pnames[pi]);
      if (!p1 || p1->type != TY_UNKNOWN) continue;
      for (int s2 = 0; s2 < c->nscopes; s2++) {
        if (s2 == s1) continue;
        Scope *sc2 = &c->scopes[s2];
        if (sc2->def_node != sc1->def_node || sc2->nparams != sc1->nparams) continue;
        if (pi >= sc2->nparams || !sc2->pnames[pi]) continue;
        LocalVar *p2 = scope_local(sc2, sc2->pnames[pi]);
        if (!p2 || p2->type == TY_UNKNOWN) continue;
        p1->type = p2->type;
        break;
      }
    }
  }

  /* Backstop: an ivar assigned only an empty array literal (no element
     evidence from usage) is left UNKNOWN, which falls back to int and a
     scalar struct field. Default such a slot to an (empty) int array so the
     field is a pointer matching the emitted sp_IntArray_new(). */
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (!ty || strcmp(ty, "InstanceVariableWriteNode")) continue;
    int v = nt_ref(c->nt, id, "value");
    const char *vty = v >= 0 ? nt_type(c->nt, v) : NULL;
    if (!vty || strcmp(vty, "ArrayNode")) continue;
    int en = 0; nt_arr(c->nt, v, "elements", &en);
    if (en != 0) continue;
    Scope *s = comp_scope_of(c, id);
    int cls_id_bs = s->class_id;
    if (cls_id_bs < 0) cls_id_bs = comp_class_index(c, "Toplevel");
    if (cls_id_bs < 0) continue;
    ClassInfo *ci = &c->classes[cls_id_bs];
    int iv = comp_ivar_index(ci, nt_str(c->nt, id, "name"));
    if (iv >= 0 && ci->ivar_types[iv] == TY_UNKNOWN) ci->ivar_types[iv] = TY_INT_ARRAY;
  }
  /* Backstop: a local variable assigned only empty array literals with no
     push evidence stays TY_UNKNOWN. Default it to TY_POLY_ARRAY so array
     operations (map!, p, etc.) can dispatch. */
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (!ty || strcmp(ty, "LocalVariableWriteNode")) continue;
    int v = nt_ref(c->nt, id, "value");
    const char *vty = v >= 0 ? nt_type(c->nt, v) : NULL;
    if (!vty || strcmp(vty, "ArrayNode")) continue;
    int en = 0; nt_arr(c->nt, v, "elements", &en);
    if (en != 0) continue;
    const char *nm = nt_str(c->nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    LocalVar *lv = nm ? scope_local(s, nm) : NULL;
    /* Also reset any hash type that crept in via premature [] read
       promotion: a variable whose only write is an empty array literal
       is definitively an array, not a hash. */
    if (lv && (lv->type == TY_UNKNOWN || ty_is_hash(lv->type))) lv->type = TY_POLY_ARRAY;
  }
  /* A read-only ivar (referenced but never assigned a typed value) stays
     TY_UNKNOWN -> it has no C type. Such a slot always reads nil at runtime;
     give it a boxed-nil poly field so `.nil?`/`.inspect` behave (#712). */
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cl = &c->classes[ci];
    for (int iv = 0; iv < cl->nivars; iv++)
      if (cl->ivar_types[iv] == TY_UNKNOWN) cl->ivar_types[iv] = TY_POLY;
  }
  /* An attr_reader/attr_accessor ivar typed via a writer call (scalar type),
     but whose class has no initialize that writes it, starts nil on fresh
     instances. Only widen when there is NO write inside ANY initialize in
     the inheritance chain (the read-only case is already TY_POLY via the
     TY_UNKNOWN pass above). */
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cl = &c->classes[ci];
    if (cl->is_struct) continue; /* struct members are set by generated ctor */
    int init_mi = comp_method_in_chain(c, ci, "initialize", NULL);
    if (init_mi >= 0) continue;
    for (int ri = 0; ri < cl->nreaders; ri++) {
      const char *rname = cl->readers[ri];
      if (!rname) continue;
      char ivname[300]; snprintf(ivname, sizeof ivname, "@%s", rname);
      int iv = comp_ivar_index(cl, ivname);
      if (iv < 0) continue;
      TyKind t = cl->ivar_types[iv];
      if (t != TY_INT && t != TY_FLOAT && t != TY_STRING &&
          t != TY_SYMBOL && t != TY_BOOL) continue;
      cl->ivar_types[iv] = TY_POLY;
      /* Also patch the node-type cache for all InstanceVariableReadNode and
         InstanceVariableWriteNode nodes that reference this ivar, so codegen
         sees TY_POLY for both the struct field and the node type. */
      for (int nid = 0; nid < c->nt->count; nid++) {
        const char *nty = nt_type(c->nt, nid);
        if (!nty) continue;
        if (strcmp(nty, "InstanceVariableReadNode") &&
            strcmp(nty, "InstanceVariableWriteNode") &&
            strcmp(nty, "InstanceVariableOperatorWriteNode") &&
            strcmp(nty, "InstanceVariableOrWriteNode") &&
            strcmp(nty, "InstanceVariableAndWriteNode")) continue;
        /* only within methods of this class */
        Scope *s = comp_scope_of(c, nid);
        if (!s || s->class_id != ci) continue;
        const char *nm = nt_str(c->nt, nid, "name");
        if (nm && !strcmp(nm, ivname)) c->ntype[nid] = TY_POLY;
      }
    }
  }
  /* Post-backstop: re-run write type inference so multi-write locals whose
     RHS chains through a now-typed ivar (e.g. @h[bank][idx] where @h was
     just promoted from UNKNOWN to POLY) get their types resolved. */
  infer_write_types(c);
  /* recompute returns: a method returning such a param is now poly */
  for (int iter = 0; iter < 8; iter++) if (!infer_return_types(c)) break;

  /* Post-fixpoint body-usage inference: type any param still TY_UNKNOWN
     from how it is used inside the method body (hash subscript patterns,
     array-specific calls). Runs after the main fixpoint so caller-side
     types always win; the mini-loop below propagates the new types. */
  if (infer_hash_params(c) | infer_array_params(c) | infer_params_from_ivar_hash_ops(c)) {
    for (int iter = 0; iter < 16; iter++) {
      int ch = 0;
      ch |= infer_param_types(c);
      ch |= infer_return_types(c);
      /* Re-run write-type inference so locals whose types derive from
         function return types (e.g. `x = f([])` after `f`'s param was
         promoted from UNKNOWN to POLY_ARRAY) get updated. */
      ch |= infer_write_types(c);
      if (!ch) break;
    }
  }

  /* Post-fixpoint: unify param types across method override families.
     When an override widens a param to TY_POLY but the parent (or
     sibling) keeps it scalar, the generated C signatures disagree and
     virtual dispatch can't call both with the same arg temps. Walk all
     scope pairs that are overrides of the same instance method in a
     parent-child class pair and widen any differing slot to TY_POLY. */
  for (int s1 = 0; s1 < c->nscopes; s1++) {
    Scope *sc1 = &c->scopes[s1];
    if (sc1->class_id < 0 || !sc1->name || sc1->is_cmethod || sc1->nparams == 0) continue;
    /* initialize is never virtually dispatched (always via ClassName.new), so
       each override may have fully independent param types. */
    if (!strcmp(sc1->name, "initialize")) continue;
    for (int s2 = s1 + 1; s2 < c->nscopes; s2++) {
      Scope *sc2 = &c->scopes[s2];
      if (sc2->class_id < 0 || !sc2->name || sc2->is_cmethod || sc2->nparams == 0) continue;
      if (strcmp(sc1->name, sc2->name) != 0) continue;
      /* check ancestor relationship: one class must be an ancestor of the other */
      int c1 = sc1->class_id, c2 = sc2->class_id;
      int related = 0;
      for (int k = c1; k >= 0; k = c->classes[k].parent) if (k == c2) { related = 1; break; }
      if (!related)
        for (int k = c2; k >= 0; k = c->classes[k].parent) if (k == c1) { related = 1; break; }
      if (!related) continue;
      int np = sc1->nparams < sc2->nparams ? sc1->nparams : sc2->nparams;
      for (int k = 0; k < np; k++) {
        LocalVar *p1 = scope_local(sc1, sc1->pnames[k]);
        LocalVar *p2 = scope_local(sc2, sc2->pnames[k]);
        if (!p1 || !p2) continue;
        if (p1->type != p2->type && (p1->type == TY_POLY || p2->type == TY_POLY)) {
          p1->type = TY_POLY;
          p2->type = TY_POLY;
        }
      }
      /* Also unify return types: if one member returns poly and another void/nil,
         make both return poly so the dispatch statement-expression can capture
         a scalar result from any arm. */
      if (sc1->ret != sc2->ret && (sc1->ret == TY_POLY || sc2->ret == TY_POLY)) {
        sc1->ret = TY_POLY;
        sc2->ret = TY_POLY;
      }
    }
  }

  /* Promote loop-multiplication variables to bigint */
  detect_bigint_loop_vars(c);
  propagate_bigint_cascade(c);

  /* mark locals captured by escaping procs (they need heap cells) */
  mark_proc_captures(c);

  /* Reachability: an instance/free method is live only if its name is
     referenced somewhere -- as a call name, an alias target, or a symbol
     literal (covering send/method/define_method). Names never mentioned
     are dead code; skipping them avoids type-checking uninvoked methods
     (e.g. a never-called method with an uninferrable param). */
  compute_reachable(c);

  /* Lower self-recursive yield methods: methods that use `yield` AND call
     themselves recursively. Their implicit block is forwarded as a synthetic
     __yblk__ sp_Proc * parameter, so the method is emitted (yields=0) and
     each `yield` in its body calls sp_proc_call(__yblk__, ...). */
  for (int mi = 1; mi < c->nscopes; mi++) {
    Scope *m = &c->scopes[mi];
    if (!m->name || !m->reachable || m->blk_param) continue;
    if (!m->yields) continue;
    if (m->body < 0) continue;
    int has_yld = 0;
    for (int id = 0; id < c->nt->count && !has_yld; id++) {
      if (c->nscope[id] != mi) continue;
      const char *ty = nt_type(c->nt, id);
      if (ty && !strcmp(ty, "YieldNode")) has_yld = 1;
    }
    if (!has_yld) continue;
    int has_self_call = 0;
    for (int id = 0; id < c->nt->count && !has_self_call; id++) {
      if (c->nscope[id] != mi) continue;
      const char *ty = nt_type(c->nt, id);
      if (!ty || strcmp(ty, "CallNode")) continue;
      const char *nm = nt_str(c->nt, id, "name");
      if (!nm || strcmp(nm, m->name)) continue;
      int recv = nt_ref(c->nt, id, "receiver");
      const char *rty = recv >= 0 ? nt_type(c->nt, recv) : NULL;
      if (recv < 0 || (rty && !strcmp(rty, "SelfNode"))) has_self_call = 1;
    }
    if (!has_self_call) continue;
    m->is_lowered_yield = 1;
    m->yields = 0;
    m->ret = TY_INT;
    m->blk_param = strdup("__yblk__");
    LocalVar *yblk = scope_local_intern(m, "__yblk__");
    if (yblk) {
      yblk->type = TY_PROC;
      yblk->is_param = 1;
      yblk->is_cell = 1;
    }
  }

  /* Post-fixpoint: propagate include-copy param types back to the source
     scope so the final infer_type scan (which uses comp_scope_of, mapping
     body nodes to the ORIGINAL scope) sees the correctly-typed params.
     Without this, LocalVariableReadNodes inside the body get TY_UNKNOWN
     because the source scope's params were never updated (no direct calls
     go through it). */
  for (int ci = 0; ci < c->nscopes; ci++) {
    Scope *copy = &c->scopes[ci];
    if (!copy->name || !copy->is_transplanted_source || copy->nparams == 0) continue;
    /* This is a transplanted SOURCE: find copies (same body, different class_id,
       params registered and typed) and unify their param types back here. */
    for (int k = 0; k < c->nscopes; k++) {
      if (k == ci) continue;
      Scope *dst = &c->scopes[k];
      if (!dst->name || strcmp(dst->name, copy->name)) continue;
      if (dst->body != copy->body || dst->nparams != copy->nparams) continue;
      if (!dst->is_transplanted_source) {
        /* dst is a copy: unify its param types into the source */
        for (int p = 0; p < copy->nparams; p++) {
          if (!copy->pnames[p]) continue;
          LocalVar *slv = scope_local(copy, copy->pnames[p]);
          LocalVar *dlv = scope_local(dst,  dst->pnames[p]);
          if (!slv || !dlv || dlv->type == TY_UNKNOWN) continue;
          TyKind mg = ty_unify(slv->type, dlv->type);
          if (mg != slv->type) slv->type = mg;
        }
        if (dst->ret != TY_UNKNOWN && copy->ret == TY_UNKNOWN)
          copy->ret = dst->ret;
      }
    }
  }

  /* Backstop: a reachable method still has TY_UNKNOWN params if it was never
     called with typed arguments (BFS marked it live by name, but no typed call
     site bound its args).  Mark such methods unreachable so codegen skips them
     rather than exit(1)ing on the unknown type. */
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (!sc->reachable || sc->nparams == 0) continue;
    int has_unknown = 0;
    for (int i = 0; i < sc->nparams; i++) {
      LocalVar *p = sc->pnames[i] ? scope_local(sc, sc->pnames[i]) : NULL;
      if (p && p->type == TY_UNKNOWN) { has_unknown = 1; break; }
    }
    if (!has_unknown) continue;
    /* A method reached only via method(:sym) is invoked through the bound
       Method ABI, which passes mrb_int args -- default its untyped params to
       int rather than dropping the method (which would leave it undeclared). */
    int taken = 0;
    if (sc->name) {
      for (int id = 0; id < c->nt->count && !taken; id++) {
        const char *nty = nt_type(c->nt, id);
        if (!nty || strcmp(nty, "CallNode")) continue;
        const char *nm = nt_str(c->nt, id, "name");
        if (!nm || strcmp(nm, "method")) continue;
        const char *msym = method_sym_arg(c, id);
        if (msym && !strcmp(msym, sc->name)) taken = 1;
      }
    }
    if (taken) {
      for (int i = 0; i < sc->nparams; i++) {
        LocalVar *p = sc->pnames[i] ? scope_local(sc, sc->pnames[i]) : NULL;
        if (p && p->type == TY_UNKNOWN) p->type = TY_INT;
      }
      if (sc->ret == TY_UNKNOWN) sc->ret = TY_INT;
    }
    else sc->reachable = 0;
  }

  /* finalize: gc-root needs + full node type cache */
  for (int s = 0; s < c->nscopes; s++)
    for (int i = 0; i < c->scopes[s].nlocals; i++)
      c->scopes[s].locals[i].gc_root = (c->scopes[s].locals[i].type == TY_STRING);

  for (int id = 0; id < c->nt->count; id++)
    infer_type(c, id);


  /* Re-infer nodes inside instance_eval block bodies with the receiver's class
     context, so ivar reads get correct types in the final c->ntype cache.
     Call infer_type on each body statement: it recursively re-infers all
     sub-expressions (including ivar reads) and updates c->ntype. */
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty2 = nt_type(c->nt, id);
    if (!ty2 || strcmp(ty2, "CallNode")) continue;
    const char *nm2 = nt_str(c->nt, id, "name");
    if (!nm2) continue;
    int blk2 = nt_ref(c->nt, id, "block");
    int recv2 = nt_ref(c->nt, id, "receiver");
    if (blk2 < 0 || recv2 < 0) continue;
    TyKind rt2 = c->ntype[recv2];
    if (!ty_is_object(rt2)) continue;
    int is_ie2 = !strcmp(nm2, "instance_eval") || !strcmp(nm2, "instance_exec");
    if (is_ie2) {
      if (comp_method_in_chain(c, ty_object_class(rt2), nm2, NULL) >= 0) continue;
    }
    else if (!comp_trampoline_kind(c, ty_object_class(rt2), nm2, NULL)) continue;
    int bdy2 = nt_ref(c->nt, blk2, "body");
    if (bdy2 < 0) continue;
    int bn2 = 0; const int *bb2 = nt_arr(c->nt, bdy2, "body", &bn2);
    if (bn2 <= 0 || !bb2) continue;
    int saved2 = g_ie_class_id;
    g_ie_class_id = ty_object_class(rt2);
    for (int k2 = 0; k2 < bn2; k2++) infer_type(c, bb2[k2]);
    g_ie_class_id = saved2;
  }
}
