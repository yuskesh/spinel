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

/* The value type of `yield` / a `<&block-param>.call` inside method mi: the
   block-body value type at a (any) call site of mi. Polymorphic, resolved from
   the first matching caller -- matches how the rewrite inlines per call site. */
static TyKind yield_value_type(Compiler *c, int mi) {
  const NodeTable *nt = c->nt;
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
      if (rmi < 0) { Scope *cs = comp_scope_of(c, cid); if (cs->class_id >= 0) rmi = comp_method_in_chain(c, cs->class_id, cn, NULL); }
    } else {
      TyKind crt = infer_type(c, crecv);
      if (ty_is_object(crt)) rmi = comp_method_in_chain(c, ty_object_class(crt), cn, NULL);
    }
    if (rmi != mi) continue;
    int bb = nt_ref(nt, blk, "body");
    int bn = 0; const int *bd = bb >= 0 ? nt_arr(nt, bb, "body", &bn) : NULL;
    if (bn == 0) return TY_NIL;
    TyKind bt = infer_type(c, bd[bn - 1]);
    return bt == TY_VOID ? TY_NIL : bt;  /* a void last-expr's block value is nil */
  }
  return TY_UNKNOWN;
}

static TyKind method_call_ret(Compiler *c, int mi, int call_id) {
  int last = scope_body_last(c, mi);
  int is_yield = last >= 0 && nt_type(c->nt, last) && !strcmp(nt_type(c->nt, last), "YieldNode");
  if (is_yield || is_blk_param_call(c, last, mi)) {
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
static int is_proc_literal(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty || strcmp(ty, "CallNode")) return 0;
  if (nt_ref(nt, id, "block") < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 && name && (!strcmp(name, "proc") || !strcmp(name, "lambda"))) return 1;
  if (recv >= 0 && name && !strcmp(name, "new") && nt_type(nt, recv) &&
      !strcmp(nt_type(nt, recv), "ConstantReadNode") && nt_str(nt, recv, "name") &&
      !strcmp(nt_str(nt, recv, "name"), "Proc")) return 1;
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

  /* an empty array literal used directly as a receiver (`[].flatten`) has no
     usage to fold an element type from; treat it as an empty poly array so
     array methods dispatch instead of falling through to unresolved. */
  if (rt == TY_UNKNOWN && recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ArrayNode")) {
      int en = 0; nt_arr(nt, recv, "elements", &en);
      if (en == 0) {
        /* first/last/min/max/pop/shift of an empty array is nil; carry it as
           a nullable int */
        if ((!strcmp(name, "first") || !strcmp(name, "last") ||
             !strcmp(name, "min") || !strcmp(name, "max") ||
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

  /* proc {} / lambda {} / Proc.new {} -> a first-class Proc value */
  if (is_proc_literal(c, id)) return TY_PROC;

  /* <proc>.call(args) / .() / [] -> the proc's recorded body return type */
  if (recv >= 0 && rt == TY_PROC &&
      (!strcmp(name, "call") || !strcmp(name, "()") || !strcmp(name, "[]")))
    return proc_call_ret(c, recv);

  /* Proc introspection */
  if (recv >= 0 && rt == TY_PROC && argc == 0) {
    if (!strcmp(name, "arity")) return TY_INT;
    if (!strcmp(name, "lambda?")) return TY_BOOL;
    if (!strcmp(name, "parameters")) return TY_POLY_ARRAY;
  }

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

  /* X.class.name / .to_s -> the class-name string (X.class is already that) */
  if (recv >= 0 && argc == 0 && (!strcmp(name, "name") || !strcmp(name, "to_s")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "class"))
    return TY_STRING;

  /* SomeClass.name / .to_s / .inspect -> the class-name string */
  if (recv >= 0 && argc == 0 &&
      (!strcmp(name, "name") || !strcmp(name, "to_s") || !strcmp(name, "inspect")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && comp_class_index(c, nt_str(nt, recv, "name")) >= 0)
    return TY_STRING;

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
    if (rty && !strcmp(rty, "ConstantReadNode")) {
      const char *cn = nt_str(nt, recv, "name");
      int ci = comp_class_index(c, cn);
      if (ci >= 0) return ty_object(ci);
      if (cn && !strcmp(cn, "Array") && argc == 2) return ty_array_of(infer_type(c, argv[1]));
      if (cn && !strcmp(cn, "String")) return TY_STRING;
      if (cn && !strcmp(cn, "StringIO")) return TY_STRINGIO;
      if (cn && !strcmp(cn, "StringScanner")) return TY_STRINGSCANNER;
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
         !strcmp(name, "mktime") || !strcmp(name, "utc") || !strcmp(name, "gm")))
      return TY_TIME;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Process") &&
        (!strcmp(name, "pid") || !strcmp(name, "ppid")))
      return TY_INT;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Integer") &&
        !strcmp(name, "sqrt"))
      return TY_INT;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "JSON") &&
        (!strcmp(name, "generate") || !strcmp(name, "dump")))
      return TY_STRING;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Dir") &&
        (!strcmp(name, "exist?") || !strcmp(name, "exists?")))
      return TY_BOOL;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Dir") &&
        !strcmp(name, "pwd"))
      return TY_STRING;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "File")) {
      if (!strcmp(name, "basename") || !strcmp(name, "dirname") || !strcmp(name, "extname") ||
          !strcmp(name, "read") || !strcmp(name, "expand_path"))
        return TY_STRING;
      if (!strcmp(name, "exist?") || !strcmp(name, "exists?"))
        return TY_BOOL;
      if (!strcmp(name, "write") || !strcmp(name, "delete"))
        return TY_INT;
      if (!strcmp(name, "mtime"))
        return TY_TIME;
    }
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
  }

  /* obj.method(...) -> the method's return type (walks the superclass chain) */
  if (recv >= 0 && ty_is_object(rt)) {
    int cid = ty_object_class(rt);
    ClassInfo *cls = &c->classes[cid];
    if (!strcmp(name, "is_a?") || !strcmp(name, "kind_of?") || !strcmp(name, "instance_of?") ||
        !strcmp(name, "respond_to?") || !strcmp(name, "==") || !strcmp(name, "!=") ||
        !strcmp(name, "nil?") || !strcmp(name, "equal?") || !strcmp(name, "frozen?")) return TY_BOOL;
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
    if (mi >= 0) return method_call_ret(c, mi, id);
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
      if (mi >= 0) return method_call_ret(c, mi, id);
    }
  }

  /* user-defined free-function call (no receiver) */
  if (recv < 0) {
    int mi = comp_method_index(c, name);
    if (mi >= 0) return method_call_ret(c, mi, id);
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
      if (!strcmp(name, "max_by") || !strcmp(name, "min_by") ||
          !strcmp(name, "find") || !strcmp(name, "detect"))
        return ty_array_elem(rt);  /* returns an element */
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
    /* index returns nil on a miss -> poly (int-or-nil) */
    if (!strcmp(name, "index") && (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY)) return TY_POLY;
    if (!strcmp(name, "length") || !strcmp(name, "size") ||
        !strcmp(name, "count") || !strcmp(name, "index")) return TY_INT;
    if (!strcmp(name, "sum"))                         return ty_array_elem(rt);
    if ((!strcmp(name, "first") || !strcmp(name, "last")) && argc == 1) return rt;  /* first(n)/last(n) -> subarray */
    if (!strcmp(name, "first") || !strcmp(name, "last") ||
        !strcmp(name, "min") || !strcmp(name, "max") ||
        !strcmp(name, "pop") || !strcmp(name, "shift")) return ty_array_elem(rt);
    if (!strcmp(name, "minmax")) return rt;  /* [min, max], same element kind */
    if (!strcmp(name, "join"))                        return TY_STRING;
    if (!strcmp(name, "pack") && argc == 1)           return TY_STRING;
    if (!strcmp(name, "inspect") || !strcmp(name, "to_s")) return TY_STRING;
    if (!strcmp(name, "empty?") || !strcmp(name, "include?")) return TY_BOOL;
    if ((!strcmp(name, "all?") || !strcmp(name, "any?") ||
         !strcmp(name, "none?") || !strcmp(name, "one?")) && argc == 0) return TY_BOOL;
    if (!strcmp(name, "push") || !strcmp(name, "<<") || !strcmp(name, "append") ||
        !strcmp(name, "reverse") || !strcmp(name, "sort") || !strcmp(name, "uniq") ||
        !strcmp(name, "to_a") || !strcmp(name, "dup") || !strcmp(name, "clone") ||
        !strcmp(name, "compact") || !strcmp(name, "flatten") || !strcmp(name, "clear") ||
        !strcmp(name, "reverse!") || !strcmp(name, "sort!") || !strcmp(name, "shuffle!") ||
        !strcmp(name, "rotate!") || !strcmp(name, "insert") || !strcmp(name, "freeze") ||
        (!strcmp(name, "fill") && argc == 1) ||
        !strcmp(name, "values_at")) return rt;
    if (!strcmp(name, "frozen?")) return TY_BOOL;
    if ((!strcmp(name, "delete_at") || !strcmp(name, "delete")) && argc == 1)
      return ty_array_elem(rt);
    if (!strcmp(name, "shift") && argc == 0) return ty_array_elem(rt);
    if (!strcmp(name, "[]="))                         return ty_array_elem(rt);
    if ((!strcmp(name, "assoc") || !strcmp(name, "rassoc")) && rt == TY_POLY_ARRAY)
      return TY_POLY_ARRAY;  /* the matching sub-array, or nil (NULL ptr) */
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
      if (!strcmp(name, "to_i") || !strcmp(name, "length") || !strcmp(name, "size")) return TY_INT;
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
    if (!strcmp(name, "delete")) return ty_hash_val(rt);
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
    }
    if (!strcmp(name, "merge") || !strcmp(name, "dup") || !strcmp(name, "clone") ||
        !strcmp(name, "replace")) return rt;
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
        !strcmp(name, "chop") || !strcmp(name, "chr") || !strcmp(name, "clamp") ||
        !strcmp(name, "squeeze") || !strcmp(name, "tr") || !strcmp(name, "tr_s") ||
        !strcmp(name, "delete")) return TY_STRING;
    if (!strcmp(name, "[]") || !strcmp(name, "slice") || !strcmp(name, "byteslice") ||
        !strcmp(name, "force_encoding") || !strcmp(name, "b") || !strcmp(name, "encode")) return TY_STRING;
    if (!strcmp(name, "index") && argc == 1) {
      const char *aty = nt_type(nt, argv[0]);
      if (aty && !strcmp(aty, "RegularExpressionNode")) return TY_POLY;  /* nil on no match */
    }
    if (!strcmp(name, "index") || !strcmp(name, "to_i") || !strcmp(name, "count") ||
        !strcmp(name, "oct") || !strcmp(name, "ord") || !strcmp(name, "casecmp")) return TY_INT;
    if (!strcmp(name, "scrub") || !strcmp(name, "crypt")) return TY_STRING;
    if (!strcmp(name, "rindex")) return TY_INT;
    if (!strcmp(name, "partition") || !strcmp(name, "rpartition")) return TY_STR_ARRAY;
    if (!strcmp(name, "casecmp?")) return TY_BOOL;
    if (!strcmp(name, "to_f"))  return TY_FLOAT;
    if (!strcmp(name, "split") || !strcmp(name, "lines") || !strcmp(name, "scan")) return TY_STR_ARRAY;
    if (!strcmp(name, "upto") && argc == 1) return TY_STR_ARRAY;  /* blockless: materialized sequence */
    if (!strcmp(name, "each_char") || !strcmp(name, "each_line") || !strcmp(name, "each_byte")) return TY_STRING;
    if (!strcmp(name, "bytes")) return TY_INT_ARRAY;
    if (!strcmp(name, "gsub") || !strcmp(name, "sub") || !strcmp(name, "tr") ||
        !strcmp(name, "center") || !strcmp(name, "ljust") || !strcmp(name, "rjust"))
      return TY_STRING;
    if (!strcmp(name, "*")) return TY_STRING;
    /* in-place append / concat reassign the receiver and evaluate to it */
    if ((!strcmp(name, "<<") || !strcmp(name, "concat") || !strcmp(name, "prepend")) && argc == 1)
      return TY_STRING;
  }
  /* integer receiver methods */
  if (recv >= 0 && rt == TY_INT) {
    if (!strcmp(name, "ceil") || !strcmp(name, "floor") ||
        !strcmp(name, "round") || !strcmp(name, "truncate")) return TY_INT;  /* no precision arg -> self */
    if (!strcmp(name, "divmod") && argc == 1) return TY_INT_ARRAY;  /* [quotient, remainder] */
    if ((!strcmp(name, "allbits?") || !strcmp(name, "anybits?") || !strcmp(name, "nobits?")) && argc == 1) return TY_BOOL;
    if ((!strcmp(name, "ceildiv") || !strcmp(name, "pow")) && argc >= 1) return TY_INT;
    if ((!strcmp(name, "pred") || !strcmp(name, "succ") || !strcmp(name, "next")) && argc == 0) return TY_INT;
    if (!strcmp(name, "nonzero?") && argc == 0) return TY_INT;  /* self or nil (nullable int) */
    /* times/upto/downto/step with a block return the receiver (self) */
    if ((!strcmp(name, "times") || !strcmp(name, "upto") || !strcmp(name, "downto") ||
         !strcmp(name, "step")) && nt_ref(nt, id, "block") >= 0) return TY_INT;
    if (!strcmp(name, "chr")) return TY_STRING;
    if (!strcmp(name, "[]") && argc == 1) return TY_INT;  /* bit access */
    if (!strcmp(name, "gcd") || !strcmp(name, "lcm") || !strcmp(name, "clamp")) return TY_INT;
    if (!strcmp(name, "digits")) return TY_INT_ARRAY;
    if (!strcmp(name, "to_s") && argc == 1) return TY_STRING;
  }
  /* float receiver methods */
  if (recv >= 0 && rt == TY_FLOAT) {
    if (!strcmp(name, "divmod") && argc == 1) return TY_POLY_ARRAY;  /* [Integer, Float] */
    if (!strcmp(name, "infinite?")) return TY_INT;   /* nil / 1 / -1 (nullable int) */
    if (!strcmp(name, "nan?") || !strcmp(name, "finite?") ||
        !strcmp(name, "positive?") || !strcmp(name, "negative?")) return TY_BOOL;
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

  if ((!strcmp(name, "-@") || !strcmp(name, "+@")) && recv >= 0 && argc == 0)
    return ty_is_numeric(rt) ? rt : TY_UNKNOWN;
  if (!strcmp(name, "!")) return TY_BOOL;
  if (!strcmp(name, "respond_to?") && recv >= 0) return TY_BOOL;
  if ((!strcmp(name, "method_defined?") || !strcmp(name, "const_defined?")) && recv >= 0) return TY_BOOL;
  if (!strcmp(name, "nil?") && recv >= 0 && argc == 0) return TY_BOOL;
  if (!strcmp(name, "object_id") && recv >= 0 && argc == 0) return TY_INT;
  if (!strcmp(name, "between?") && argc == 2 && (rt == TY_STRING || ty_is_numeric(rt))) return TY_BOOL;
  if ((!strcmp(name, "match?") || !strcmp(name, "!~")) && recv >= 0) return TY_BOOL;
  if (!strcmp(name, "=~") && recv >= 0 && argc == 1) {
    const char *rrt = nt_type(nt, recv), *art = nt_type(nt, argv[0]);
    if ((rrt && !strcmp(rrt, "RegularExpressionNode")) ||
        (art && !strcmp(art, "RegularExpressionNode"))) return TY_POLY;
  }
  /* /re/.source -> String, /re/.options -> Integer (compile-time constants) */
  if (recv >= 0 && argc == 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "RegularExpressionNode")) {
    if (!strcmp(name, "source")) return TY_STRING;
    if (!strcmp(name, "options")) return TY_INT;
  }

  if (recv >= 0 && argc == 1 && is_arith_op(name)) {
    if (rt == TY_STRING) {
      if (!strcmp(name, "%")) return TY_STRING;  /* sprintf (array or single value) */
      if (!strcmp(name, "+") || !strcmp(name, "*")) return TY_STRING;
      return TY_UNKNOWN;
    }
    /* array + array (same kind) -> a concatenated array of that kind */
    if (!strcmp(name, "+") && ty_is_array(rt) && a0 == rt) return rt;
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

  if (!strcmp(ty, "LocalVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    LocalVar *lv = nm ? scope_local(s, nm) : NULL;
    return lv ? lv->type : TY_UNKNOWN;
  }
  if (!strcmp(ty, "GlobalVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    /* predefined punctuation globals: $/ defaults to "\n"; $! / $; / $, read nil */
    if (nm && !strcmp(nm, "$/")) return TY_STRING;
    if (nm && (!strcmp(nm, "$!") || !strcmp(nm, "$;") || !strcmp(nm, "$,"))) return TY_NIL;
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
  if (!strcmp(ty, "ClassVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    if (s->class_id < 0) return TY_UNKNOWN;
    int idx = nm ? comp_cvar_index(&c->classes[s->class_id], nm) : -1;
    return idx >= 0 ? c->classes[s->class_id].cvar_types[idx] : TY_UNKNOWN;
  }
  if (!strcmp(ty, "ClassVariableWriteNode"))
    return infer_type(c, nt_ref(nt, id, "value"));
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
  if (!strcmp(ty, "HashNode") || !strcmp(ty, "KeywordHashNode")) {
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
  if (!strcmp(ty, "YieldNode"))
    return yield_value_type(c, (int)(comp_scope_of(c, id) - c->scopes));
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

  if (ty && (!strcmp(ty, "ClassNode") || !strcmp(ty, "ModuleNode"))) {
    int cp = nt_ref(c->nt, id, "constant_path");
    const char *cname = cp >= 0 ? nt_str(c->nt, cp, "name") : NULL;
    if (cname && comp_class_index(c, cname) < 0) {
      comp_class_new(c, cname, id);
      child_class = c->nclasses - 1;
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
      int accessor = !strcmp(nm, "attr_accessor") ||
                     !strcmp(nm, "attribute") || !strcmp(nm, "attributes");
      int reader = !strcmp(nm, "attr_reader") || accessor;
      int writer = !strcmp(nm, "attr_writer") || accessor;
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
      if (nm && is_c_ident(nm) && comp_class_index(c, nm) < 0) {
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
/* Register each class variable (@@x) in its owning class and infer its type
   from the write sites' RHS. */
static int infer_cvar_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
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
  return changed;
}

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
    /* Record the proc's body return type so `<var>.call(...)` knows the
       result type (e.g. `sq = proc { |x| x*x }; sq.call(5)` -> int). Only
       a direct proc literal RHS carries it; escape-through-return/param is
       a later slice. */
    if (lv->type == TY_PROC && !strcmp(ty, "LocalVariableWriteNode")) {
      int vnode = nt_ref(nt, id, "value");
      TyKind pr = vnode >= 0 ? proc_ret_of(c, vnode) : TY_UNKNOWN;
      if (pr != TY_UNKNOWN && (TyKind)lv->proc_ret != pr) { lv->proc_ret = (int)pr; changed = 1; }
    }
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
      if (s->class_id < 0) continue;
      ClassInfo *ci = &c->classes[s->class_id];
      int iv = inm ? comp_ivar_index(ci, inm) : -1;
      if (iv < 0) continue;
      slot = &ci->ivar_types[iv];
    }
    else continue;

    TyKind before = *slot;
    if (is_push || kt == TY_INT) {
      /* array (a known string's `<<` is append, not push -- don't pollute) */
      if (vt == TY_UNKNOWN) continue;
      if (*slot != TY_UNKNOWN && !ty_is_array(*slot)) continue;
      *slot = ty_unify(*slot, ty_array_of(vt));
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
    if (*slot != before) changed = 1;
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
    /* a proc passed as an argument carries its body return type to the param,
       so `f.call(...)` inside the method knows its result type */
    if (merged == TY_PROC) {
      TyKind pr = proc_ret_of(c, argv[k]);
      if (pr != TY_UNKNOWN && p->proc_ret != (int)pr) { p->proc_ret = (int)pr; changed = 1; }
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
      if (dt == TY_NIL) continue;  /* nil default doesn't pin the type */
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
    else if (rt == TY_STRING && (!strcmp(name, "each_char") || !strcmp(name, "each_line") || !strcmp(name, "upto")))
      pt = TY_STRING;
    else if (rt == TY_STRING && !strcmp(name, "each_byte"))
      pt = TY_INT;
    else if ((!strcmp(name, "each") || !strcmp(name, "map") || !strcmp(name, "collect") ||
              !strcmp(name, "select") || !strcmp(name, "reject") || !strcmp(name, "filter") ||
              !strcmp(name, "find") || !strcmp(name, "detect") || !strcmp(name, "each_with_index") ||
              !strcmp(name, "sort_by") || !strcmp(name, "find_all") || !strcmp(name, "count")) && rt == TY_RANGE)
      pt = TY_INT;
    else if ((!strcmp(name, "each") || !strcmp(name, "map") || !strcmp(name, "collect") ||
              !strcmp(name, "select") || !strcmp(name, "reject") || !strcmp(name, "filter") ||
              !strcmp(name, "find") || !strcmp(name, "detect") ||
              !strcmp(name, "max_by") || !strcmp(name, "min_by") || !strcmp(name, "sort_by") ||
              !strcmp(name, "take_while") || !strcmp(name, "drop_while") ||
              !strcmp(name, "reverse_each") || !strcmp(name, "each_entry") ||
              !strcmp(name, "each_with_index")) &&
             ty_is_array(rt))
      pt = ty_array_elem(rt);

    /* array.each_cons(n) { |a, b, ...| } -- a single param binds the n-element
       sub-array; multiple params destructure consecutive elements */
    if (!strcmp(name, "each_cons") && ty_is_array(rt)) {
      Scope *es = comp_scope_of(c, block);
      int np = 0; while (block_param_name(c, block, np)) np++;
      for (int pj = 0; pj < np; pj++) {
        const char *pn = block_param_name(c, block, pj);
        LocalVar *lp = scope_local_intern(es, pn); lp->is_block_param = 1;
        TyKind want = (np == 1) ? rt : ty_array_elem(rt);
        TyKind m = ty_unify(lp->type, want);
        if (m != lp->type) { lp->type = m; changed = 1; }
      }
      continue;
    }

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

    /* hash.each / each_pair { |k, v| } binds two params */
    if ((!strcmp(name, "each") || !strcmp(name, "each_pair") || !strcmp(name, "map") ||
         !strcmp(name, "collect") || !strcmp(name, "flat_map") || !strcmp(name, "select") ||
         !strcmp(name, "filter") || !strcmp(name, "reject") || !strcmp(name, "find") ||
         !strcmp(name, "detect") || !strcmp(name, "sort_by") || !strcmp(name, "min_by") ||
         !strcmp(name, "max_by") || !strcmp(name, "count") || !strcmp(name, "sum") ||
         !strcmp(name, "any?") || !strcmp(name, "all?") || !strcmp(name, "none?") ||
         !strcmp(name, "each_with_index") || !strcmp(name, "each_with_object")) && ty_is_hash(rt)) {
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
    ch |= infer_write_types(c);
    ch |= infer_param_types(c);
    ch |= infer_string_params(c);
    ch |= infer_default_param_types(c);
    ch |= infer_block_params(c);
    ch |= infer_for_index(c);
    ch |= infer_ivar_types(c);
    ch |= infer_cvar_types(c);
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
    if (s->class_id < 0) continue;
    ClassInfo *ci = &c->classes[s->class_id];
    int iv = comp_ivar_index(ci, nt_str(c->nt, id, "name"));
    if (iv >= 0 && ci->ivar_types[iv] == TY_UNKNOWN) ci->ivar_types[iv] = TY_INT_ARRAY;
  }
  /* A read-only ivar (referenced but never assigned a typed value) stays
     TY_UNKNOWN -> it has no C type. Such a slot always reads nil at runtime;
     give it a boxed-nil poly field so `.nil?`/`.inspect` behave (#712). */
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cl = &c->classes[ci];
    for (int iv = 0; iv < cl->nivars; iv++)
      if (cl->ivar_types[iv] == TY_UNKNOWN) cl->ivar_types[iv] = TY_POLY;
  }
  /* recompute returns: a method returning such a param is now poly */
  for (int iter = 0; iter < 8; iter++) if (!infer_return_types(c)) break;

  /* mark locals captured by escaping procs (they need heap cells) */
  mark_proc_captures(c);

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
