/* M2 code generator: the M1 scalar/control-flow subset plus user-defined
 * methods (required params, inferred param/return types, recursion, tail-
 * position implicit returns). Emits the same runtime ABI as the legacy
 * generator. Unsupported constructs abort loudly.
 */
#include "codegen.h"
#include "compiler.h"
#include "analyze.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- output buffer ---- */

typedef struct { char *p; size_t len, cap; } Buf;

static void buf_putn(Buf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + n + 1) nc *= 2;
    b->p = realloc(b->p, nc);
    b->cap = nc;
  }
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = '\0';
}
static void buf_puts(Buf *b, const char *s) { buf_putn(b, s, strlen(s)); }
static void buf_printf(Buf *b, const char *fmt, ...) {
  char tmp[512];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n < sizeof(tmp)) { buf_putn(b, tmp, (size_t)n); return; }
  char *big = malloc((size_t)n + 1);
  va_start(ap, fmt); vsnprintf(big, (size_t)n + 1, fmt, ap); va_end(ap);
  buf_putn(b, big, (size_t)n); free(big);
}
static void emit_indent(Buf *b, int n) { for (int i = 0; i < n; i++) buf_puts(b, "  "); }

/* Statement prelude: some expressions (array/hash literals) lower to
   temp-variable construction that must run before the statement that
   uses them. While a statement line is being built, g_pre collects those
   setup lines at g_indent; the statement wrapper flushes g_pre before the
   line. g_tmp hands out unique temp ids. */
static Buf *g_pre = NULL;
static int  g_indent = 0;
static int  g_tmp = 0;

/* ---- diagnostics ---- */

static void unsupported(Compiler *c, int id, const char *what) {
  const char *ty = nt_type(c->nt, id);
  fprintf(stderr, "spinelc: unsupported %s: node %d (%s)\n",
          what, id, ty ? ty : "?");
  exit(1);
}

/* ---- type -> C ---- */

static const char *c_type_name(TyKind t) {
  switch (t) {
    case TY_INT:         return "mrb_int";
    case TY_FLOAT:       return "mrb_float";
    case TY_BOOL:        return "mrb_bool";
    case TY_STRING:      return "const char *";
    case TY_SYMBOL:      return "sp_sym";
    case TY_RANGE:       return "sp_Range";
    case TY_INT_ARRAY:   return "sp_IntArray *";
    case TY_FLOAT_ARRAY: return "sp_FloatArray *";
    case TY_STR_ARRAY:   return "sp_StrArray *";
    case TY_STR_INT_HASH: return "sp_StrIntHash *";
    case TY_STR_STR_HASH: return "sp_StrStrHash *";
    case TY_INT_INT_HASH: return "sp_IntIntHash *";
    case TY_INT_STR_HASH: return "sp_IntStrHash *";
    default:             return NULL;
  }
}
static int is_scalar_ret(TyKind t) {
  return t == TY_INT || t == TY_FLOAT || t == TY_BOOL || t == TY_STRING ||
         t == TY_SYMBOL || t == TY_RANGE ||
         t == TY_INT_ARRAY || t == TY_FLOAT_ARRAY || t == TY_STR_ARRAY ||
         ty_is_hash(t) || ty_is_object(t);
}
static const char *default_value(TyKind t) {
  switch (t) {
    case TY_INT:    return "0";
    case TY_FLOAT:  return "0.0";
    case TY_BOOL:   return "0";
    case TY_STRING: return "(&(\"\\xff\")[1])";
    case TY_SYMBOL: return "((sp_sym)-1)";
    case TY_RANGE:  return "(sp_Range){0}";
    case TY_INT_ARRAY:
    case TY_FLOAT_ARRAY:
    case TY_STR_ARRAY: return "NULL";
    default:        return ty_is_hash(t) ? "NULL" : "0";
  }
}
/* Append the C type name for `t` to `b` (objects need the class name). */
static void emit_ctype(Compiler *c, TyKind t, Buf *b) {
  if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    buf_printf(b, "sp_%s", c->classes[cid].name);
  } else {
    const char *n = c_type_name(t);
    buf_puts(b, n ? n : "void");
  }
}

/* "Int" / "Str" / "Float" for the sp_<K>Array_* runtime family. */
static const char *array_kind(TyKind t) {
  switch (t) {
    case TY_INT_ARRAY:   return "Int";
    case TY_FLOAT_ARRAY: return "Float";
    case TY_STR_ARRAY:   return "Str";
    default:             return NULL;
  }
}

/* ---- C string literals ---- */

static void emit_c_escaped(Buf *b, const char *s) {
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    unsigned char ch = *p;
    if (ch == '\\' || ch == '"') buf_printf(b, "\\%c", ch);
    else if (ch == '\n') buf_puts(b, "\\n");
    else if (ch == '\t') buf_puts(b, "\\t");
    else if (ch == '\r') buf_puts(b, "\\r");
    else if (ch >= 0x20 && ch < 0x7f) buf_printf(b, "%c", ch);
    else buf_printf(b, "\\%03o", ch);
  }
}
static void emit_str_literal(Buf *b, const char *content) {
  if (!content || !*content) { buf_puts(b, "(&(\"\\xff\")[1])"); return; }
  buf_puts(b, "(&(\"\\xff\" \"");
  emit_c_escaped(b, content);
  buf_puts(b, "\")[1])");
}

/* ---- forward decls ---- */

static void emit_expr(Compiler *c, int id, Buf *b);
static void emit_stmt(Compiler *c, int id, Buf *b, int indent);
static void emit_stmts(Compiler *c, int id, Buf *b, int indent);
static void emit_stmts_tail(Compiler *c, int id, Buf *b, int indent);
static int  emit_array_mutate_stmt(Compiler *c, int id, Buf *b, int indent);
static int  emit_output_call(Compiler *c, int id, Buf *b, int indent);
static int  emit_iteration_stmt(Compiler *c, int id, Buf *b, int indent);
static void emit_index_op_write(Compiler *c, int id, Buf *b, int indent);
static void emit_super(Compiler *c, int id, Buf *b);
static void emit_args_filled(Compiler *c, int callee_idx, int argsNode, const char *lead, Buf *out);

/* Strip ParenthesesNode wrappers to reach the inner expression. */
static int unwrap_parens(Compiler *c, int id) {
  while (id >= 0) {
    const char *ty = nt_type(c->nt, id);
    if (!ty || strcmp(ty, "ParenthesesNode")) break;
    int body = nt_ref(c->nt, id, "body");
    int n = 0;
    const int *bd = body >= 0 ? nt_arr(c->nt, body, "body", &n) : NULL;
    if (n != 1) break;
    id = bd[0];
  }
  return id;
}

/* ---- calls ---- */

static const char *int_arith_fn(const char *op) {
  if (!strcmp(op, "+"))  return "sp_int_add";
  if (!strcmp(op, "-"))  return "sp_int_sub";
  if (!strcmp(op, "*"))  return "sp_int_mul";
  if (!strcmp(op, "/"))  return "sp_idiv";
  if (!strcmp(op, "%"))  return "sp_imod";
  if (!strcmp(op, "**")) return "sp_int_pow";
  return NULL;
}

static void emit_method_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int mi = comp_method_index(c, name);
  buf_printf(b, "sp_%s(", name);
  emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), "", b);
  buf_puts(b, ")");
}

/* map/select/reject/filter as an expression: build a result array via a
   loop emitted into the statement prelude; the expression value is the
   temp array. Returns 1 if handled. */
static int emit_collect_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name || recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = array_kind(rt);
  if (!k) return 0;

  int is_map = !strcmp(name, "map") || !strcmp(name, "collect");
  int is_sel = !strcmp(name, "select") || !strcmp(name, "filter");
  int is_rej = !strcmp(name, "reject");
  if (!is_map && !is_sel && !is_rej) return 0;

  TyKind restype = comp_ntype(c, id);
  const char *rk = array_kind(restype);
  if (!rk) return 0;

  const char *p0 = block_param_name(c, block, 0);
  int body = nt_ref(nt, block, "body");
  int bn = 0;
  const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;

  int trecv = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;

  /* eval receiver once (its own preludes must land before the decl line) */
  Buf rb; memset(&rb, 0, sizeof rb);
  emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent);
  emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = ", trecv);
  buf_puts(g_pre, rb.p ? rb.p : "");
  buf_puts(g_pre, ";\n");
  free(rb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk, tres, rk);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, k, trecv, ti);

  int bodyIndent = g_indent + 1;
  if (p0) {
    emit_indent(g_pre, bodyIndent);
    buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
  }
  /* body statements except the last */
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, bodyIndent);

  int saveIndent = g_indent;
  g_indent = bodyIndent;
  Buf vb; memset(&vb, 0, sizeof vb);
  emit_expr(c, bb[bn - 1], &vb);  /* value preludes flow to g_pre at bodyIndent */
  g_indent = saveIndent;

  if (is_map) {
    emit_indent(g_pre, bodyIndent);
    buf_printf(g_pre, "sp_%sArray_push(_t%d, ", rk, tres);
    buf_puts(g_pre, vb.p ? vb.p : ""); buf_puts(g_pre, ");\n");
  } else {
    emit_indent(g_pre, bodyIndent);
    buf_printf(g_pre, "if (%s(", is_rej ? "!" : "");
    buf_puts(g_pre, vb.p ? vb.p : ""); buf_puts(g_pre, ")) ");
    buf_printf(g_pre, "sp_%sArray_push(_t%d, lv_%s);\n", rk, tres, p0 ? p0 : "");
  }
  free(vb.p);
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "}\n");

  buf_printf(b, "_t%d", tres);
  return 1;
}

/* Emit the value for callee param `idx`: the provided arg node if any,
   else the param's default (a nil default becomes the type's default). */
static void emit_arg_or_default(Compiler *c, Scope *m, int idx, int provided, Buf *out) {
  if (provided >= 0) { emit_expr(c, provided, out); return; }
  LocalVar *p = scope_local(m, m->pnames[idx]);
  TyKind pt = p ? p->type : TY_INT;
  int dv = m->pdefault[idx];
  const char *dty = dv >= 0 ? nt_type(c->nt, dv) : NULL;
  if (dv < 0 || (dty && !strcmp(dty, "NilNode")))
    buf_puts(out, pt == TY_RANGE ? "(sp_Range){0}" : default_value(pt));
  else
    emit_expr(c, dv, out);
}

/* Emit a comma-separated argument list filling defaults for omitted
   optional params. `lead` is prepended before the first arg. */
static void emit_args_filled(Compiler *c, int callee_idx, int argsNode, const char *lead, Buf *out) {
  Scope *m = &c->scopes[callee_idx];
  int argc = 0;
  const int *argv = argsNode >= 0 ? nt_arr(c->nt, argsNode, "arguments", &argc) : NULL;
  for (int i = 0; i < m->nparams; i++) {
    buf_puts(out, i == 0 ? lead : ", ");
    emit_arg_or_default(c, m, i, i < argc ? argv[i] : -1, out);
  }
}

static int is_descendant(Compiler *c, int k, int anc) {
  for (int x = k; x >= 0; x = c->classes[x].parent) if (x == anc) return 1;
  return 0;
}

/* Number of distinct implementations of `name` across cid's subtree
   (cid + all descendants). >1 means a self/obj call needs runtime dispatch. */
static int dispatch_impl_count(Compiler *c, int cid, const char *name) {
  int impls[256], n = 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (!is_descendant(c, k, cid)) continue;
    int def = -1;
    if (comp_method_in_chain(c, k, name, &def) < 0) continue;
    int seen = 0;
    for (int j = 0; j < n; j++) if (impls[j] == def) seen = 1;
    if (!seen && n < 256) impls[n++] = def;
  }
  return n;
}

/* Emit a (possibly virtual) method call. `selfptr` is a reusable C
   expression yielding sp_<static>* (e.g. "self", "&lv_x", "&_t3"). Args
   are pre-evaluated into temps so they're emitted once. */
static void emit_dispatch(Compiler *c, int cid, const char *name,
                          const char *selfptr, int argsNode, Buf *b) {
  const NodeTable *nt = c->nt;
  int defcls = cid;
  int mi = comp_method_in_chain(c, cid, name, &defcls);
  Scope *m = mi >= 0 ? &c->scopes[mi] : NULL;
  TyKind ret = m ? m->ret : TY_UNKNOWN;

  int argc = 0;
  const int *argv = argsNode >= 0 ? nt_arr(nt, argsNode, "arguments", &argc) : NULL;
  int np = m ? m->nparams : argc;
  /* evaluate each param value (provided arg or default) into a temp so the
     virtual-dispatch cases reuse them without re-evaluating */
  int *atmp = np ? malloc(sizeof(int) * np) : NULL;
  for (int k = 0; k < np; k++) {
    atmp[k] = ++g_tmp;
    Buf ab; memset(&ab, 0, sizeof ab);
    emit_arg_or_default(c, m, k, k < argc ? argv[k] : -1, &ab);
    LocalVar *p = scope_local(m, m->pnames[k]);
    emit_indent(g_pre, g_indent);
    emit_ctype(c, p ? p->type : comp_ntype(c, k < argc ? argv[k] : -1), g_pre);
    buf_printf(g_pre, " _t%d = ", atmp[k]);
    buf_puts(g_pre, ab.p ? ab.p : ""); buf_puts(g_pre, ";\n");
    free(ab.p);
  }

  int virtual = dispatch_impl_count(c, cid, name) > 1 && is_scalar_ret(ret);

  if (!virtual) {
    buf_printf(b, "sp_%s_%s((sp_%s *)%s", c->classes[defcls].name, name, c->classes[defcls].name, selfptr);
    for (int k = 0; k < np; k++) buf_printf(b, ", _t%d", atmp[k]);
    buf_puts(b, ")");
    free(atmp);
    return;
  }

  /* runtime dispatch on cls_id (GCC statement-expression) */
  int rtmp = ++g_tmp;
  buf_puts(b, "({ ");
  emit_ctype(c, ret, b);
  buf_printf(b, " _t%d; switch ((%s)->cls_id) {", rtmp, selfptr);
  for (int k = 0; k < c->nclasses; k++) {
    if (!is_descendant(c, k, cid)) continue;
    int kd = -1;
    if (comp_method_in_chain(c, k, name, &kd) < 0) continue;
    buf_printf(b, " case %d: _t%d = sp_%s_%s((sp_%s *)%s", k, rtmp,
               c->classes[kd].name, name, c->classes[kd].name, selfptr);
    for (int a = 0; a < np; a++) buf_printf(b, ", _t%d", atmp[a]);
    buf_puts(b, "); break;");
  }
  buf_printf(b, " default: _t%d = sp_%s_%s((sp_%s *)%s", rtmp,
             c->classes[defcls].name, name, c->classes[defcls].name, selfptr);
  for (int a = 0; a < np; a++) buf_printf(b, ", _t%d", atmp[a]);
  buf_printf(b, "); break; } _t%d; })", rtmp);
  free(atmp);
}

static void emit_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  if (emit_collect_expr(c, id, b)) return;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  if (!name) unsupported(c, id, "call (no name)");

  if (recv < 0 && comp_method_index(c, name) >= 0) { emit_method_call(c, id, b); return; }

  /* implicit-self call inside an instance method */
  if (recv < 0) {
    Scope *self = comp_scope_of(c, id);
    if (self->class_id >= 0) {
      if (comp_reader_in_chain(c, self->class_id, name, NULL)) {
        buf_printf(b, "self->iv_%s", name);
        return;
      }
      int mi = comp_method_in_chain(c, self->class_id, name, NULL);
      if (mi >= 0) {
        emit_dispatch(c, self->class_id, name, "self", nt_ref(nt, id, "arguments"), b);
        return;
      }
    }
  }

  /* Class.new(args) -> sp_<Class>_new(args) */
  if (recv >= 0 && !strcmp(name, "new")) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ConstantReadNode")) {
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      if (ci >= 0) {
        buf_printf(b, "sp_%s_new(", c->classes[ci].name);
        int initm = comp_method_in_chain(c, ci, "initialize", NULL);
        if (initm >= 0) emit_args_filled(c, initm, nt_ref(nt, id, "arguments"), "", b);
        buf_puts(b, ")");
        return;
      }
    }
  }

  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  TyKind res = comp_ntype(c, id);

  if ((!strcmp(name, "-@") || !strcmp(name, "+@")) && recv >= 0 && argc == 0) {
    buf_puts(b, name[0] == '-' ? "(-" : "(+");
    emit_expr(c, recv, b); buf_puts(b, ")");
    return;
  }
  if (!strcmp(name, "!") && recv >= 0 && argc == 0) {
    buf_puts(b, "(!"); emit_expr(c, recv, b); buf_puts(b, ")");
    return;
  }

  if (recv >= 0 && argc == 1 && int_arith_fn(name)) {
    if (rt == TY_STRING && !strcmp(name, "+")) {
      buf_puts(b, "sp_str_concat(");
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
    if (rt == TY_STRING && !strcmp(name, "*")) {
      buf_puts(b, "sp_str_repeat(");
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
    if (res == TY_INT) {
      buf_printf(b, "%s(", int_arith_fn(name));
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
    if (res == TY_FLOAT && strcmp(name, "%") && strcmp(name, "**")) {
      buf_puts(b, "(");
      emit_expr(c, recv, b);
      buf_printf(b, " %s ", name);
      emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
    unsupported(c, id, "arithmetic");
  }

  /* integer bitwise operators */
  if (recv >= 0 && argc == 1 && rt == TY_INT &&
      (!strcmp(name, "&") || !strcmp(name, "|") || !strcmp(name, "^") ||
       !strcmp(name, "<<") || !strcmp(name, ">>"))) {
    buf_puts(b, "(");
    emit_expr(c, recv, b);
    buf_printf(b, " %s ", name);
    emit_expr(c, argv[0], b);
    buf_puts(b, ")");
    return;
  }

  if (recv >= 0 && argc == 1 &&
      (!strcmp(name, "<") || !strcmp(name, ">") ||
       !strcmp(name, "<=") || !strcmp(name, ">="))) {
    if (ty_is_numeric(rt)) {
      buf_puts(b, "(");
      emit_expr(c, recv, b);
      buf_printf(b, " %s ", name);
      emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
    if (rt == TY_STRING) {
      buf_puts(b, "(strcmp(");
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
      buf_printf(b, ") %s 0)", name);
      return;
    }
    unsupported(c, id, "comparison");
  }

  if (argc == 1 && (!strcmp(name, "==") || !strcmp(name, "!="))) {
    int eq = !strcmp(name, "==");
    if (rt == TY_STRING || a0 == TY_STRING) {
      buf_puts(b, eq ? "sp_str_eq(" : "(!sp_str_eq(");
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
      buf_puts(b, eq ? ")" : "))");
      return;
    }
    if (ty_is_numeric(rt) || rt == TY_BOOL || rt == TY_SYMBOL) {
      buf_puts(b, "(");
      emit_expr(c, recv, b);
      buf_printf(b, " %s ", eq ? "==" : "!=");
      emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
    unsupported(c, id, "equality");
  }

  /* object method call: sp_<DefClass>_<m>((sp_<DefClass>*)&recv, args) */
  if (recv >= 0 && ty_is_object(rt)) {
    int cid = ty_object_class(rt);
    /* attr reader -> field access (recv).iv_x */
    if (comp_reader_in_chain(c, cid, name, NULL)) {
      buf_puts(b, "("); emit_expr(c, recv, b); buf_printf(b, ").iv_%s", name);
      return;
    }
    int mi = comp_method_in_chain(c, cid, name, NULL);
    if (mi >= 0) {
      /* receiver pointer: &lvalue, or a temp for an rvalue receiver */
      char selfptr[64];
      const char *rty = nt_type(nt, recv);
      if (rty && (!strcmp(rty, "LocalVariableReadNode") || !strcmp(rty, "InstanceVariableReadNode"))) {
        Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
        char tmp[64];
        snprintf(tmp, sizeof tmp, "&%s", rb.p ? rb.p : "");
        snprintf(selfptr, sizeof selfptr, "%s", tmp);
        free(rb.p);
      } else {
        int t = ++g_tmp;
        emit_indent(g_pre, g_indent);
        emit_ctype(c, rt, g_pre);
        buf_printf(g_pre, " _t%d = ", t);
        Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
        buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
        snprintf(selfptr, sizeof selfptr, "&_t%d", t);
      }
      emit_dispatch(c, cid, name, selfptr, nt_ref(nt, id, "arguments"), b);
      return;
    }
  }

  /* range value methods (evaluate the range once into a temp) */
  if (recv >= 0 && rt == TY_RANGE) {
    static const char *const rmeths[] = {
      "to_a", "include?", "member?", "cover?", "sum", "min", "max",
      "first", "last", "size", "count", "begin", "end", NULL };
    int known = 0;
    for (int i = 0; rmeths[i]; i++) if (!strcmp(name, rmeths[i])) known = 1;
    if (known) {
      int t = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Range _t%d = ", t);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      if (!strcmp(name, "to_a"))
        buf_printf(b, "sp_IntArray_from_range(_t%d.first, _t%d.last - _t%d.excl)", t, t, t);
      else if (!strcmp(name, "include?") || !strcmp(name, "member?") || !strcmp(name, "cover?")) {
        buf_printf(b, "sp_range_include(&_t%d, ", t); emit_expr(c, argv[0], b); buf_puts(b, ")");
      } else if (!strcmp(name, "first") || !strcmp(name, "min") || !strcmp(name, "begin"))
        buf_printf(b, "(_t%d.first)", t);
      else if (!strcmp(name, "last") || !strcmp(name, "max"))
        buf_printf(b, "(_t%d.last - _t%d.excl)", t, t);
      else if (!strcmp(name, "end"))
        buf_printf(b, "(_t%d.last)", t);
      else if (!strcmp(name, "size") || !strcmp(name, "count"))
        buf_printf(b, "(_t%d.last - _t%d.excl - _t%d.first + 1)", t, t, t);
      else if (!strcmp(name, "sum"))
        buf_printf(b, "sp_IntArray_sum(sp_IntArray_from_range(_t%d.first, _t%d.last - _t%d.excl), 0)", t, t, t);
      return;
    }
  }

  /* hash value methods */
  if (recv >= 0 && ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    if (hn) {
      if (!strcmp(name, "[]") && argc == 1) {
        buf_printf(b, "sp_%sHash_get_opt(", hn);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "fetch") && argc == 1) {
        buf_printf(b, "sp_%sHash_get(", hn);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "length") || !strcmp(name, "size") || !strcmp(name, "count")) && argc == 0) {
        buf_printf(b, "sp_%sHash_length(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "empty?") && argc == 0) {
        buf_printf(b, "(sp_%sHash_length(", hn); emit_expr(c, recv, b); buf_puts(b, ") == 0)");
        return;
      }
      if ((!strcmp(name, "has_key?") || !strcmp(name, "key?") ||
           !strcmp(name, "include?") || !strcmp(name, "member?")) && argc == 1) {
        buf_printf(b, "sp_%sHash_has_key(", hn);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "keys") && argc == 0) {
        buf_printf(b, "sp_%sHash_keys(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "values") && argc == 0 && rt != TY_INT_INT_HASH) {
        buf_printf(b, "sp_%sHash_values(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "inspect") || !strcmp(name, "to_s")) && argc == 0) {
        buf_printf(b, "sp_%sHash_inspect(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
    }
  }

  /* array value methods */
  if (recv >= 0 && ty_is_array(rt)) {
    const char *k = array_kind(rt);
    if (k) {
      if (!strcmp(name, "[]") && argc == 1) {
        buf_printf(b, "sp_%sArray_get(", k);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "length") || !strcmp(name, "size")) && argc == 0) {
        buf_printf(b, "sp_%sArray_length(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "empty?") && argc == 0) {
        buf_printf(b, "(sp_%sArray_length(", k); emit_expr(c, recv, b); buf_puts(b, ") == 0)");
        return;
      }
      if (!strcmp(name, "sum") && argc == 0) {
        buf_printf(b, "sp_%sArray_sum(", k); emit_expr(c, recv, b); buf_puts(b, ", 0)");
        return;
      }
      if (!strcmp(name, "join") && rt == TY_STR_ARRAY && argc == 1) {
        buf_puts(b, "sp_StrArray_join("); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "inspect") || !strcmp(name, "to_s")) && argc == 0) {
        buf_printf(b, "sp_%sArray_inspect(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "first") && argc == 0) {
        buf_printf(b, "sp_%sArray_get(", k); emit_expr(c, recv, b); buf_puts(b, ", 0)");
        return;
      }
      if ((!strcmp(name, "min") || !strcmp(name, "max")) && argc == 0 && rt != TY_STR_ARRAY) {
        buf_printf(b, "sp_%sArray_%s(", k, name); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "include?") || !strcmp(name, "index")) && argc == 1 && rt != TY_FLOAT_ARRAY) {
        const char *fn = !strcmp(name, "include?") ? "include" : "index";
        buf_printf(b, "sp_%sArray_%s(", k, fn);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "sort") || !strcmp(name, "uniq")) && argc == 0 && rt == TY_INT_ARRAY) {
        buf_printf(b, "sp_IntArray_%s(", name); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "last") && argc == 0) {
        int t = ++g_tmp;
        Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "%s _t%d = ", c_type_name(rt), t);
        buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
        buf_printf(b, "sp_%sArray_get(_t%d, sp_%sArray_length(_t%d) - 1)", k, t, k, t);
        return;
      }
    }
  }

  /* symbol receiver methods */
  if (recv >= 0 && rt == TY_SYMBOL) {
    if (!strcmp(name, "to_s") || !strcmp(name, "id2name") || !strcmp(name, "name")) {
      buf_puts(b, "sp_sym_to_s("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (!strcmp(name, "inspect")) {
      buf_puts(b, "sp_str_concat(SPL(\":\"), sp_sym_to_s("); emit_expr(c, recv, b); buf_puts(b, "))");
      return;
    }
    if (!strcmp(name, "to_sym")) { emit_expr(c, recv, b); return; }
  }

  /* boolean receiver methods */
  if (recv >= 0 && rt == TY_BOOL) {
    if (!strcmp(name, "to_s") || !strcmp(name, "inspect")) {
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") ? SPL(\"true\") : SPL(\"false\"))");
      return;
    }
    if (!strcmp(name, "&") || !strcmp(name, "|") || !strcmp(name, "^")) {
      buf_puts(b, "("); emit_expr(c, recv, b); buf_printf(b, " %s ", name); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
  }

  /* scalar receiver methods: evaluate the receiver once into rs, then
     splice its text (so a literal/complex receiver isn't rebuilt). */
  if (recv >= 0 && (rt == TY_STRING || rt == TY_INT || rt == TY_FLOAT)) {
    Buf rs; memset(&rs, 0, sizeof rs);
    emit_expr(c, recv, &rs);
    const char *r = rs.p ? rs.p : "";
    int handled = 1;

    if (rt == TY_STRING) {
      if      (!strcmp(name, "length") || !strcmp(name, "size")) buf_printf(b, "sp_str_length(%s)", r);
      else if (!strcmp(name, "upcase"))     buf_printf(b, "sp_str_upcase(%s)", r);
      else if (!strcmp(name, "downcase"))   buf_printf(b, "sp_str_downcase(%s)", r);
      else if (!strcmp(name, "capitalize")) buf_printf(b, "sp_str_capitalize(%s)", r);
      else if (!strcmp(name, "reverse"))    buf_printf(b, "sp_str_reverse(%s)", r);
      else if (!strcmp(name, "strip"))      buf_printf(b, "sp_str_strip(%s)", r);
      else if (!strcmp(name, "lstrip"))     buf_printf(b, "sp_str_lstrip(%s)", r);
      else if (!strcmp(name, "rstrip"))     buf_printf(b, "sp_str_rstrip(%s)", r);
      else if (!strcmp(name, "chomp"))      buf_printf(b, "sp_str_chomp(%s)", r);
      else if (!strcmp(name, "chop"))       buf_printf(b, "sp_str_chop(%s)", r);
      else if (!strcmp(name, "to_s") || !strcmp(name, "to_str") || !strcmp(name, "dup")) buf_puts(b, r);
      else if (!strcmp(name, "inspect"))    buf_printf(b, "sp_str_inspect(%s)", r);
      else if (!strcmp(name, "empty?"))     buf_printf(b, "(sp_str_length(%s) == 0)", r);
      else if (!strcmp(name, "include?") && argc == 1) {
        buf_printf(b, "sp_str_include(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      } else if (!strcmp(name, "start_with?") && argc == 1) {
        buf_printf(b, "sp_str_start_with(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      } else if (!strcmp(name, "end_with?") && argc == 1) {
        buf_printf(b, "sp_str_end_with(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      } else if (!strcmp(name, "index") && argc == 1) {
        buf_printf(b, "sp_str_index(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      } else if (!strcmp(name, "[]") && argc == 1) {
        buf_printf(b, "sp_str_char_at_or_nil(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      } else if (!strcmp(name, "split") && argc == 1) {
        buf_printf(b, "sp_str_split_drop_trailing(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      } else if (!strcmp(name, "lines") && argc == 0) buf_printf(b, "sp_str_lines(%s)", r);
      else if (!strcmp(name, "bytes") && argc == 0)   buf_printf(b, "sp_str_bytes(%s)", r);
      else if (!strcmp(name, "to_i") && argc == 0)    buf_printf(b, "sp_str_to_i_strict(%s)", r);
      else if (!strcmp(name, "to_f") && argc == 0)    buf_printf(b, "atof(%s)", r);
      else if (!strcmp(name, "gsub") && argc == 2) {
        buf_printf(b, "sp_str_gsub(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      } else if (!strcmp(name, "sub") && argc == 2) {
        buf_printf(b, "sp_str_sub(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      } else if (!strcmp(name, "tr") && argc == 2) {
        buf_printf(b, "sp_str_tr(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      } else if (!strcmp(name, "center") && argc == 1) {
        buf_printf(b, "sp_str_center(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      } else if (!strcmp(name, "ljust") && argc == 1) {
        buf_printf(b, "sp_str_ljust(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      } else if (!strcmp(name, "rjust") && argc == 1) {
        buf_printf(b, "sp_str_rjust(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      } else handled = 0;
    } else if (rt == TY_INT) {
      if      (!strcmp(name, "to_s") && argc == 0) buf_printf(b, "sp_int_to_s(%s)", r);
      else if (!strcmp(name, "inspect")) buf_printf(b, "sp_int_to_s(%s)", r);
      else if (!strcmp(name, "to_f"))   buf_printf(b, "((mrb_float)(%s))", r);
      else if (!strcmp(name, "to_i") || !strcmp(name, "to_int") || !strcmp(name, "floor") ||
               !strcmp(name, "ceil") || !strcmp(name, "round")) buf_printf(b, "(%s)", r);
      else if (!strcmp(name, "abs"))    buf_printf(b, "((%s) < 0 ? -(%s) : (%s))", r, r, r);
      else if (!strcmp(name, "chr"))    buf_printf(b, "sp_int_chr(%s)", r);
      else if (!strcmp(name, "even?"))  buf_printf(b, "((%s) %% 2 == 0)", r);
      else if (!strcmp(name, "odd?"))   buf_printf(b, "((%s) %% 2 != 0)", r);
      else if (!strcmp(name, "zero?"))  buf_printf(b, "((%s) == 0)", r);
      else if (!strcmp(name, "positive?")) buf_printf(b, "((%s) > 0)", r);
      else if (!strcmp(name, "negative?")) buf_printf(b, "((%s) < 0)", r);
      else if (!strcmp(name, "gcd") && argc == 1) { buf_printf(b, "sp_gcd(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "lcm") && argc == 1) { buf_printf(b, "sp_lcm(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "clamp") && argc == 2) { buf_printf(b, "sp_int_clamp(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "digits") && argc == 0) buf_printf(b, "sp_int_digits(%s, 10)", r);
      else if (!strcmp(name, "to_s") && argc == 1) { buf_printf(b, "sp_int_to_s_base(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else handled = 0;
    } else { /* TY_FLOAT */
      /* round/ceil/floor/truncate(n>0) -> Float to n decimals; else Integer */
      int ndig = 0;
      if ((!strcmp(name, "floor") || !strcmp(name, "ceil") ||
           !strcmp(name, "round") || !strcmp(name, "truncate")) && argc == 1) {
        const char *aty = nt_type(c->nt, argv[0]);
        if (aty && !strcmp(aty, "IntegerNode")) ndig = (int)nt_int(c->nt, argv[0], "value", 0);
      }
      const char *cfn = !strcmp(name, "floor") ? "floor" : !strcmp(name, "ceil") ? "ceil"
                      : !strcmp(name, "truncate") ? "trunc" : "round";
      if ((!strcmp(name, "floor") || !strcmp(name, "ceil") ||
           !strcmp(name, "round") || !strcmp(name, "truncate"))) {
        if (ndig > 0)
          buf_printf(b, "({ double _f = pow(10, %d); %s((%s) * _f) / _f; })", ndig, cfn, r);
        else
          buf_printf(b, "((mrb_int)%s(%s))", cfn, r);
      }
      else if (!strcmp(name, "to_i"))  buf_printf(b, "((mrb_int)(%s))", r);
      else if (!strcmp(name, "to_f"))  buf_printf(b, "(%s)", r);
      else if (!strcmp(name, "to_s") || !strcmp(name, "inspect"))  buf_printf(b, "sp_float_to_s(%s)", r);
      else if (!strcmp(name, "abs"))   buf_printf(b, "((%s) < 0 ? -(%s) : (%s))", r, r, r);
      else if (!strcmp(name, "zero?")) buf_printf(b, "((%s) == 0.0)", r);
      else handled = 0;
    }
    free(rs.p);
    if (handled) return;
  }

  unsupported(c, id, "call");
}

/* Array-mutating calls emitted as statements: a[i]=v, a.push(v), a<<v.
   Returns 1 if handled. */
static int emit_array_mutate_stmt(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name || recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);

  if (ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    if (hn && !strcmp(name, "[]=") && argc == 2) {
      emit_indent(b, indent);
      buf_printf(b, "sp_%sHash_set(", hn);
      emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_expr(c, argv[0], b); buf_puts(b, ", ");
      emit_expr(c, argv[1], b); buf_puts(b, ");\n");
      return 1;
    }
    return 0;
  }

  if (!ty_is_array(rt)) return 0;
  const char *k = array_kind(rt);
  if (!k) return 0;

  if (!strcmp(name, "[]=") && argc == 2) {
    emit_indent(b, indent);
    buf_printf(b, "sp_%sArray_set(", k);
    emit_expr(c, recv, b); buf_puts(b, ", ");
    emit_expr(c, argv[0], b); buf_puts(b, ", ");
    emit_expr(c, argv[1], b); buf_puts(b, ");\n");
    return 1;
  }
  if ((!strcmp(name, "push") || !strcmp(name, "<<")) && argc == 1) {
    emit_indent(b, indent);
    buf_printf(b, "sp_%sArray_push(", k);
    emit_expr(c, recv, b); buf_puts(b, ", ");
    emit_expr(c, argv[0], b); buf_puts(b, ");\n");
    return 1;
  }
  return 0;
}

/* h[k] op= v  /  a[i] op= v  (IndexOperatorWriteNode). Receiver and key
   are evaluated once into temps. */
static void emit_index_op_write(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  const char *op = nt_str(nt, id, "binary_operator");
  int args = nt_ref(nt, id, "arguments");
  int v = nt_ref(nt, id, "value");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  if (argc != 1 || !op) unsupported(c, id, "index operator assignment");
  TyKind rt = comp_ntype(c, recv);

  int ta = ++g_tmp, tb = ++g_tmp;

  if (ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    TyKind vt = ty_hash_val(rt);
    if (!hn) unsupported(c, id, "index operator assignment (hash)");
    emit_indent(b, indent);
    buf_printf(b, "{ %s _t%d = ", c_type_name(rt), ta); emit_expr(c, recv, b);
    buf_printf(b, "; %s _t%d = ", c_type_name(ty_hash_key(rt)), tb); emit_expr(c, argv[0], b);
    buf_puts(b, "; ");
    buf_printf(b, "sp_%sHash_set(_t%d, _t%d, ", hn, ta, tb);
    if (vt == TY_STRING && !strcmp(op, "+")) {
      buf_printf(b, "sp_str_concat(sp_%sHash_get(_t%d, _t%d), ", hn, ta, tb);
      emit_expr(c, v, b); buf_puts(b, ")");
    } else {
      buf_printf(b, "sp_%sHash_get(_t%d, _t%d) %s ", hn, ta, tb, op);
      buf_puts(b, "("); emit_expr(c, v, b); buf_puts(b, ")");
    }
    buf_puts(b, "); }\n");
    return;
  }

  if (ty_is_array(rt)) {
    const char *k = array_kind(rt);
    if (!k) unsupported(c, id, "index operator assignment (array)");
    emit_indent(b, indent);
    buf_printf(b, "{ %s _t%d = ", c_type_name(rt), ta); emit_expr(c, recv, b);
    buf_printf(b, "; mrb_int _t%d = ", tb); emit_expr(c, argv[0], b);
    buf_puts(b, "; ");
    buf_printf(b, "sp_%sArray_set(_t%d, _t%d, sp_%sArray_get(_t%d, _t%d) %s ", k, ta, tb, k, ta, tb, op);
    buf_puts(b, "("); emit_expr(c, v, b); buf_puts(b, ")); }\n");
    return;
  }
  unsupported(c, id, "index operator assignment");
}

/* Block iteration lowered to an inline C for-loop. Handles n.times,
   array.each, range.each, n.upto/downto. Returns 1 if handled. */
static int emit_iteration_stmt(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name || recv < 0) return 0;
  int body = nt_ref(nt, block, "body");
  const char *p0 = block_param_name(c, block, 0);
  TyKind rt = comp_ntype(c, recv);

  /* n.times { |i| ... } */
  if (!strcmp(name, "times") && rt == TY_INT) {
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb);
    emit_expr(c, recv, &rb);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < ", t, t);
    buf_puts(b, rb.p); buf_printf(b, "; _t%d++) {\n", t);
    if (p0) { emit_indent(b, indent + 1); buf_printf(b, "lv_%s = _t%d;\n", p0, t); }
    emit_stmts(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    free(rb.p);
    return 1;
  }

  /* hash.each / each_pair { |k, v| ... } */
  if ((!strcmp(name, "each") || !strcmp(name, "each_pair")) && ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    if (!hn) return 0;
    const char *p1 = block_param_name(c, block, 1);
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb);
    emit_expr(c, recv, &rb);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < ", t, t);
    buf_puts(b, rb.p); buf_printf(b, "->len; _t%d++) {\n", t);
    if (p0) {
      emit_indent(b, indent + 1);
      buf_printf(b, "lv_%s = ", p0); buf_puts(b, rb.p); buf_printf(b, "->order[_t%d];\n", t);
    }
    if (p1) {
      emit_indent(b, indent + 1);
      buf_printf(b, "lv_%s = sp_%sHash_get(", p1, hn);
      buf_puts(b, rb.p); buf_puts(b, ", "); buf_puts(b, rb.p); buf_printf(b, "->order[_t%d]);\n", t);
    }
    emit_stmts(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    free(rb.p);
    return 1;
  }

  /* array.each { |x| ... } */
  if (!strcmp(name, "each") && ty_is_array(rt)) {
    const char *k = array_kind(rt);
    if (!k) return 0;
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb);
    emit_expr(c, recv, &rb);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(", t, t, k);
    buf_puts(b, rb.p); buf_printf(b, "); _t%d++) {\n", t);
    if (p0) {
      emit_indent(b, indent + 1);
      buf_printf(b, "lv_%s = sp_%sArray_get(", p0, k);
      buf_puts(b, rb.p); buf_printf(b, ", _t%d);\n", t);
    }
    emit_stmts(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    free(rb.p);
    return 1;
  }

  /* (a..b).each { |i| ... } -- any range-typed receiver */
  if (!strcmp(name, "each") && rt == TY_RANGE && p0) {
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent);
    buf_printf(b, "sp_Range _t%d = ", t); buf_puts(b, rb.p ? rb.p : ""); buf_puts(b, ";\n");
    free(rb.p);
    emit_indent(b, indent);
    buf_printf(b, "for (lv_%s = _t%d.first; lv_%s <= _t%d.last - _t%d.excl; lv_%s++) {\n",
               p0, t, p0, t, t, p0);
    emit_stmts(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* n.upto(m) / n.downto(m) { |i| ... } */
  if ((!strcmp(name, "upto") || !strcmp(name, "downto")) && rt == TY_INT && p0) {
    int up = !strcmp(name, "upto");
    int args = nt_ref(nt, id, "arguments");
    int argc = 0;
    const int *argv = NULL;
    if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
    if (argc != 1) return 0;
    Buf lo; memset(&lo, 0, sizeof lo); emit_expr(c, recv, &lo);
    Buf hi; memset(&hi, 0, sizeof hi); emit_expr(c, argv[0], &hi);
    emit_indent(b, indent);
    buf_printf(b, "for (lv_%s = ", p0); buf_puts(b, lo.p);
    buf_printf(b, "; lv_%s %s ", p0, up ? "<=" : ">="); buf_puts(b, hi.p);
    buf_printf(b, "; lv_%s%s) {\n", p0, up ? "++" : "--");
    emit_stmts(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    free(lo.p); free(hi.p);
    return 1;
  }

  return 0;
}

/* ---- interpolation ---- */

static void emit_interp(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *parts = nt_arr(nt, id, "parts", &n);
  Buf fmt; memset(&fmt, 0, sizeof fmt);
  Buf argbuf; memset(&argbuf, 0, sizeof argbuf);
  int nargs = 0;

  for (int k = 0; k < n; k++) {
    int pid = parts[k];
    const char *pty = nt_type(nt, pid);
    if (pty && !strcmp(pty, "StringNode")) {
      const char *content = nt_str(nt, pid, "content");
      for (const char *p = content ? content : ""; *p; p++) {
        if (*p == '%') buf_puts(&fmt, "%%");
        else buf_printf(&fmt, "%c", *p);
      }
    } else if (pty && !strcmp(pty, "EmbeddedStatementsNode")) {
      int s = nt_ref(nt, pid, "statements");
      int bn = 0;
      const int *body = s >= 0 ? nt_arr(nt, s, "body", &bn) : NULL;
      int expr = bn > 0 ? body[bn - 1] : -1;
      TyKind t = comp_ntype(c, expr);
      buf_puts(&argbuf, ", ");
      if (t == TY_INT) {
        buf_puts(&fmt, "%lld"); buf_puts(&argbuf, "(long long)");
        emit_expr(c, expr, &argbuf);
      } else if (t == TY_STRING) {
        buf_puts(&fmt, "%s"); emit_expr(c, expr, &argbuf);
      } else if (t == TY_FLOAT) {
        buf_puts(&fmt, "%s"); buf_puts(&argbuf, "sp_float_to_s(");
        emit_expr(c, expr, &argbuf); buf_puts(&argbuf, ")");
      } else if (t == TY_BOOL) {
        buf_puts(&fmt, "%s"); buf_puts(&argbuf, "(");
        emit_expr(c, expr, &argbuf); buf_puts(&argbuf, " ? \"true\" : \"false\")");
      } else if (t == TY_SYMBOL) {
        buf_puts(&fmt, "%s"); buf_puts(&argbuf, "sp_sym_to_s(");
        emit_expr(c, expr, &argbuf); buf_puts(&argbuf, ")");
      } else {
        free(fmt.p); free(argbuf.p);
        unsupported(c, pid, "interpolation value");
      }
      nargs++;
    } else {
      free(fmt.p); free(argbuf.p);
      unsupported(c, pid, "interpolation part");
    }
  }

  if (nargs == 0) {
    buf_puts(b, "(&(\"\\xff\" \"");
    for (const char *p = fmt.p ? fmt.p : ""; *p; p++) {
      if (p[0] == '%' && p[1] == '%') { buf_puts(b, "%"); p++; }
      else buf_printf(b, "%c", *p);
    }
    buf_puts(b, "\")[1])");
  } else {
    buf_puts(b, "sp_sprintf(\"");
    buf_puts(b, fmt.p ? fmt.p : "");
    buf_puts(b, "\"");
    buf_puts(b, argbuf.p ? argbuf.p : "");
    buf_puts(b, ")");
  }
  free(fmt.p); free(argbuf.p);
}

/* ---- expression ---- */

static void emit_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) unsupported(c, id, "expression (no type)");

  if (!strcmp(ty, "IntegerNode")) { buf_printf(b, "%lldLL", nt_int(nt, id, "value", 0)); return; }
  if (!strcmp(ty, "FloatNode")) { const char *v = nt_content(nt, id); buf_puts(b, v ? v : "0.0"); return; }
  if (!strcmp(ty, "StringNode")) { emit_str_literal(b, nt_str(nt, id, "content")); return; }
  if (!strcmp(ty, "InterpolatedStringNode")) { emit_interp(c, id, b); return; }
  if (!strcmp(ty, "TrueNode"))  { buf_puts(b, "1"); return; }
  if (!strcmp(ty, "FalseNode")) { buf_puts(b, "0"); return; }
  if (!strcmp(ty, "NilNode"))   { buf_puts(b, "0"); return; }  /* default in numeric/bool context */
  if (!strcmp(ty, "SymbolNode")) {
    int sid = comp_sym_intern(c, nt_str(nt, id, "value"));
    buf_printf(b, "((sp_sym)%d)", sid);
    return;
  }
  if (!strcmp(ty, "RangeNode")) {
    int left = nt_ref(nt, id, "left");
    int right = nt_ref(nt, id, "right");
    int excl = (int)(nt_int(nt, id, "flags", 0) & 4) ? 1 : 0;
    buf_puts(b, "sp_range_new(");
    if (left >= 0) emit_expr(c, left, b); else buf_puts(b, "0");
    buf_puts(b, ", ");
    if (right >= 0) emit_expr(c, right, b); else buf_puts(b, "0");
    buf_printf(b, ", %d)", excl);
    return;
  }
  if (!strcmp(ty, "LocalVariableReadNode")) { buf_printf(b, "lv_%s", nt_str(nt, id, "name")); return; }
  if (!strcmp(ty, "InstanceVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");  /* "@x" */
    buf_printf(b, "self->iv_%s", nm + 1);
    return;
  }
  if (!strcmp(ty, "GlobalVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    if (nm && comp_gvar(c, nm + 1)) { buf_printf(b, "gv_%s", nm + 1); return; }
    unsupported(c, id, "global variable read");
  }
  if (!strcmp(ty, "ConstantReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    if (nm && comp_const(c, nm)) { buf_printf(b, "cst_%s", nm); return; }
    unsupported(c, id, "constant read");
  }
  if (!strcmp(ty, "ParenthesesNode")) {
    int body = nt_ref(nt, id, "body");
    int n = 0;
    const int *bd = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    if (n != 1) unsupported(c, id, "parenthesized group");
    buf_puts(b, "("); emit_expr(c, bd[0], b); buf_puts(b, ")");
    return;
  }
  if (!strcmp(ty, "ArrayNode")) {
    int n = 0;
    const int *els = nt_arr(nt, id, "elements", &n);
    TyKind at = comp_ntype(c, id);
    /* an empty `[]` literal carries no element type of its own; it is
       emitted via the target's type in emit_assign. If we reach here for
       an empty literal, fall back to an int array. */
    const char *k = array_kind(at);
    if (n == 0 && !k) { buf_puts(b, "sp_IntArray_new()"); return; }
    if (!k) unsupported(c, id, "array literal (element type)");
    int t = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", k, t, k);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
    for (int j = 0; j < n; j++) {
      Buf el; memset(&el, 0, sizeof el);
      emit_expr(c, els[j], &el);   /* element preludes flow to g_pre first */
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_%sArray_push(_t%d, ", k, t);
      buf_puts(g_pre, el.p ? el.p : "");
      buf_puts(g_pre, ");\n");
      free(el.p);
    }
    buf_printf(b, "_t%d", t);
    return;
  }
  if (!strcmp(ty, "HashNode")) {
    TyKind ht = comp_ntype(c, id);
    const char *hn = ty_hash_cname(ht);
    if (!hn) unsupported(c, id, "hash literal (key/value type)");
    int n = 0;
    const int *els = nt_arr(nt, id, "elements", &n);
    int t = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_%sHash *_t%d = sp_%sHash_new();\n", hn, t, hn);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
    for (int j = 0; j < n; j++) {
      int key = nt_ref(nt, els[j], "key");
      int val = nt_ref(nt, els[j], "value");
      Buf kb; memset(&kb, 0, sizeof kb); emit_expr(c, key, &kb);
      Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, val, &vb);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_%sHash_set(_t%d, ", hn, t);
      buf_puts(g_pre, kb.p ? kb.p : ""); buf_puts(g_pre, ", ");
      buf_puts(g_pre, vb.p ? vb.p : ""); buf_puts(g_pre, ");\n");
      free(kb.p); free(vb.p);
    }
    buf_printf(b, "_t%d", t);
    return;
  }
  if (!strcmp(ty, "CallNode")) { emit_call(c, id, b); return; }
  if (!strcmp(ty, "SuperNode") || !strcmp(ty, "ForwardingSuperNode")) { emit_super(c, id, b); return; }
  if (!strcmp(ty, "AndNode") || !strcmp(ty, "OrNode")) {
    int left = nt_ref(nt, id, "left"), right = nt_ref(nt, id, "right");
    if (comp_ntype(c, left) == TY_BOOL && comp_ntype(c, right) == TY_BOOL) {
      buf_puts(b, "(");
      emit_expr(c, left, b);
      buf_puts(b, !strcmp(ty, "AndNode") ? " && " : " || ");
      emit_expr(c, right, b);
      buf_puts(b, ")");
      return;
    }
    unsupported(c, id, "&&/|| (non-bool operands)");
  }

  unsupported(c, id, "expression");
}

/* ---- output statements (puts/print/p) ---- */

static void emit_puts_one(Compiler *c, int arg, Buf *b, int indent) {
  TyKind t = comp_ntype(c, arg);
  emit_indent(b, indent);
  if (t == TY_INT) {
    buf_puts(b, "printf(\"%lld\\n\", (long long)"); emit_expr(c, arg, b); buf_puts(b, ");\n");
  } else if (t == TY_FLOAT) {
    buf_puts(b, "{ const char *_fs = sp_float_to_s("); emit_expr(c, arg, b);
    buf_puts(b, "); fputs(_fs, stdout); putchar('\\n'); }\n");
  } else if (t == TY_STRING) {
    buf_puts(b, "{ const char *_ps = (const char *)("); emit_expr(c, arg, b);
    buf_puts(b, "); if (_ps) { fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); } else putchar('\\n'); }\n");
  } else if (t == TY_BOOL) {
    buf_puts(b, "puts(("); emit_expr(c, arg, b); buf_puts(b, ") ? \"true\" : \"false\");\n");
  } else if (t == TY_SYMBOL) {
    buf_puts(b, "puts(sp_sym_to_s("); emit_expr(c, arg, b); buf_puts(b, "));\n");
  } else if (ty_is_array(t) && array_kind(t)) {
    /* puts [a,b,c] prints each element on its own line (empty array: blank) */
    const char *k = array_kind(t);
    Buf ab; memset(&ab, 0, sizeof ab); emit_expr(c, arg, &ab);
    const char *a = ab.p ? ab.p : "";
    int ti = ++g_tmp;
    buf_printf(b, "if (sp_%sArray_length(%s) == 0) putchar('\\n');\n", k, a);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(%s); _t%d++) ", ti, ti, k, a, ti);
    if (t == TY_INT_ARRAY)
      buf_printf(b, "printf(\"%%lld\\n\", (long long)sp_IntArray_get(%s, _t%d));\n", a, ti);
    else if (t == TY_FLOAT_ARRAY)
      buf_printf(b, "{ const char *_fs = sp_float_to_s(sp_FloatArray_get(%s, _t%d)); fputs(_fs, stdout); putchar('\\n'); }\n", a, ti);
    else /* str */
      buf_printf(b, "{ const char *_ps = sp_StrArray_get(%s, _t%d); if (_ps) { fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); } else putchar('\\n'); }\n", a, ti);
    free(ab.p);
  } else if (ty_is_object(t) && comp_method_in_class(c, ty_object_class(t), "to_s") >= 0) {
    int cid = ty_object_class(t);
    buf_puts(b, "{ const char *_ps = (const char *)(");
    buf_printf(b, "sp_%s_to_s(", c->classes[cid].name);
    const char *rty = nt_type(c->nt, arg);
    if (rty && (!strcmp(rty, "LocalVariableReadNode") || !strcmp(rty, "InstanceVariableReadNode"))) {
      buf_puts(b, "&"); emit_expr(c, arg, b);
    } else {
      int tt = ++g_tmp;
      emit_indent(g_pre, g_indent); emit_ctype(c, t, g_pre);
      buf_printf(g_pre, " _t%d = ", tt);
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, arg, &rb);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      buf_printf(b, "&_t%d", tt);
    }
    buf_puts(b, ")); if (_ps) { fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); } else putchar('\\n'); }\n");
  } else {
    unsupported(c, arg, "puts argument");
  }
}
static void emit_print_one(Compiler *c, int arg, Buf *b, int indent) {
  TyKind t = comp_ntype(c, arg);
  emit_indent(b, indent);
  if (t == TY_INT) {
    buf_puts(b, "printf(\"%lld\", (long long)"); emit_expr(c, arg, b); buf_puts(b, ");\n");
  } else if (t == TY_FLOAT) {
    buf_puts(b, "fputs(sp_float_to_s("); emit_expr(c, arg, b); buf_puts(b, "), stdout);\n");
  } else if (t == TY_STRING) {
    buf_puts(b, "{ const char *_s = ("); emit_expr(c, arg, b);
    buf_puts(b, "); if (_s) fputs(_s, stdout); }\n");
  } else if (t == TY_BOOL) {
    buf_puts(b, "fputs(("); emit_expr(c, arg, b); buf_puts(b, ") ? \"true\" : \"false\", stdout);\n");
  } else {
    unsupported(c, arg, "print argument");
  }
}
static void emit_p_one(Compiler *c, int arg, Buf *b, int indent) {
  TyKind t = comp_ntype(c, arg);
  emit_indent(b, indent);
  if (t == TY_INT) {
    buf_puts(b, "printf(\"%lld\\n\", (long long)"); emit_expr(c, arg, b); buf_puts(b, ");\n");
  } else if (t == TY_FLOAT) {
    buf_puts(b, "{ const char *_fs = sp_float_to_s("); emit_expr(c, arg, b);
    buf_puts(b, "); fputs(_fs, stdout); putchar('\\n'); }\n");
  } else if (t == TY_STRING) {
    buf_puts(b, "fputs(sp_str_inspect("); emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  } else if (t == TY_BOOL) {
    buf_puts(b, "puts(("); emit_expr(c, arg, b); buf_puts(b, ") ? \"true\" : \"false\");\n");
  } else if (t == TY_SYMBOL) {
    buf_puts(b, "fputs(sp_str_concat(SPL(\":\"), sp_sym_to_s(");
    emit_expr(c, arg, b);
    buf_puts(b, ")), stdout); putchar('\\n');\n");
  } else if (ty_is_array(t) && array_kind(t)) {
    buf_printf(b, "fputs(sp_%sArray_inspect(", array_kind(t));
    emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  } else if (ty_is_hash(t) && ty_hash_cname(t)) {
    buf_printf(b, "fputs(sp_%sHash_inspect(", ty_hash_cname(t));
    emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  } else {
    unsupported(c, arg, "p argument");
  }
}

static int emit_output_call(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name || recv >= 0) return 0;
  if (comp_method_index(c, name) >= 0) return 0; /* user method shadows builtin */
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);

  if (!strcmp(name, "puts")) {
    if (argc == 0) { emit_indent(b, indent); buf_puts(b, "putchar('\\n');\n"); return 1; }
    for (int k = 0; k < argc; k++) emit_puts_one(c, argv[k], b, indent);
    return 1;
  }
  if (!strcmp(name, "print")) { for (int k = 0; k < argc; k++) emit_print_one(c, argv[k], b, indent); return 1; }
  if (!strcmp(name, "p"))     { for (int k = 0; k < argc; k++) emit_p_one(c, argv[k], b, indent); return 1; }
  return 0;
}

/* ---- assignment ---- */

static void emit_assign(Compiler *c, int id, Buf *b, int indent) {
  const char *nm = nt_str(c->nt, id, "name");
  int v = nt_ref(c->nt, id, "value");
  LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
  emit_indent(b, indent);
  buf_printf(b, "lv_%s = ", nm);
  /* `x = nil` -> the variable's type-appropriate default */
  const char *vty = nt_type(c->nt, v);
  int vn = 0;
  int is_empty_array = vty && !strcmp(vty, "ArrayNode") && (nt_arr(c->nt, v, "elements", &vn), vn == 0);
  if (vty && !strcmp(vty, "NilNode") && lv) {
    if (lv->type == TY_RANGE) buf_puts(b, "(sp_Range){0}");
    else buf_puts(b, default_value(lv->type));
  } else if (is_empty_array && lv && array_kind(lv->type)) {
    /* `a = []` -> a new array of the variable's resolved element type */
    buf_printf(b, "sp_%sArray_new()", array_kind(lv->type));
  } else {
    emit_expr(c, v, b);
  }
  buf_puts(b, ";\n");
}

static void emit_op_assign(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  const char *op = nt_str(nt, id, "binary_operator");
  int v = nt_ref(nt, id, "value");
  LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
  TyKind t = lv ? lv->type : TY_UNKNOWN;
  emit_indent(b, indent);

  if (t == TY_STRING && !strcmp(op, "+")) {
    buf_printf(b, "lv_%s = sp_str_concat(lv_%s, ", nm, nm);
    emit_expr(c, v, b); buf_puts(b, ");\n");
    return;
  }
  if (t == TY_INT && (!strcmp(op, "+") || !strcmp(op, "-") || !strcmp(op, "*"))) {
    buf_printf(b, "lv_%s %s= ", nm, op); emit_expr(c, v, b); buf_puts(b, ";\n");
    return;
  }
  if (t == TY_INT) {
    const char *fn = int_arith_fn(op);
    if (fn) { buf_printf(b, "lv_%s = %s(lv_%s, ", nm, fn, nm); emit_expr(c, v, b); buf_puts(b, ");\n"); return; }
  }
  if (t == TY_FLOAT && (!strcmp(op, "+") || !strcmp(op, "-") || !strcmp(op, "*") || !strcmp(op, "/"))) {
    buf_printf(b, "lv_%s %s= ", nm, op); emit_expr(c, v, b); buf_puts(b, ";\n");
    return;
  }
  unsupported(c, id, "operator assignment");
}

/* ---- control flow ---- */

static void emit_cond(Compiler *c, int id, Buf *b) {
  if (comp_ntype(c, id) != TY_BOOL) unsupported(c, id, "condition (non-bool)");
  emit_expr(c, id, b);
}

static void emit_if(Compiler *c, int id, Buf *b, int indent, int is_unless, int tail) {
  const NodeTable *nt = c->nt;
  int pred = nt_ref(nt, id, "predicate");
  int then_b = nt_ref(nt, id, "statements");
  int sub = nt_ref(nt, id, "subsequent");

  emit_indent(b, indent);
  buf_puts(b, "if (");
  if (is_unless) buf_puts(b, "!(");
  emit_cond(c, pred, b);
  if (is_unless) buf_puts(b, ")");
  buf_puts(b, ") {\n");
  if (tail) emit_stmts_tail(c, then_b, b, indent + 1);
  else      emit_stmts(c, then_b, b, indent + 1);
  emit_indent(b, indent);
  buf_puts(b, "}");

  if (sub >= 0) {
    const char *sty = nt_type(nt, sub);
    if (sty && !strcmp(sty, "ElseNode")) {
      buf_puts(b, "\n");
      emit_indent(b, indent);
      buf_puts(b, "else {\n");
      int s = nt_ref(nt, sub, "statements");
      if (tail) emit_stmts_tail(c, s, b, indent + 1);
      else      emit_stmts(c, s, b, indent + 1);
      emit_indent(b, indent); buf_puts(b, "}\n");
    } else if (sty && !strcmp(sty, "IfNode")) {
      buf_puts(b, "\n");
      emit_indent(b, indent);
      buf_puts(b, "else {\n");
      emit_if(c, sub, b, indent + 1, 0, tail);
      emit_indent(b, indent); buf_puts(b, "}\n");
    } else {
      buf_puts(b, "\n");
    }
  } else {
    buf_puts(b, "\n");
  }
}

/* case/when -> an if / else-if chain. Statement form. */
static void emit_case(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  int pred = nt_ref(nt, id, "predicate");
  int nw = 0;
  const int *whens = nt_arr(nt, id, "conditions", &nw);
  int else_clause = nt_ref(nt, id, "else_clause");

  int t = -1;
  TyKind pt = TY_UNKNOWN;
  if (pred >= 0) {
    pt = comp_ntype(c, pred);
    t = ++g_tmp;
    emit_indent(b, indent);
    emit_ctype(c, pt, b);
    buf_printf(b, " _t%d = ", t);
    emit_expr(c, pred, b);
    buf_puts(b, ";\n");
  }

  for (int w = 0; w < nw; w++) {
    int wn = whens[w];
    int wc = 0;
    const int *conds = nt_arr(nt, wn, "conditions", &wc);
    emit_indent(b, indent);
    buf_puts(b, w == 0 ? "if (" : "else if (");
    for (int j = 0; j < wc; j++) {
      if (j) buf_puts(b, " || ");
      if (pred >= 0) {
        if (pt == TY_STRING) {
          buf_printf(b, "sp_str_eq(_t%d, ", t); emit_expr(c, conds[j], b); buf_puts(b, ")");
        } else {
          buf_printf(b, "(_t%d == ", t); emit_expr(c, conds[j], b); buf_puts(b, ")");
        }
      } else {
        buf_puts(b, "("); emit_expr(c, conds[j], b); buf_puts(b, ")");
      }
    }
    buf_puts(b, ") {\n");
    emit_stmts(c, nt_ref(nt, wn, "statements"), b, indent + 1);
    emit_indent(b, indent);
    buf_puts(b, "}\n");
  }

  if (else_clause >= 0) {
    emit_indent(b, indent);
    buf_puts(b, "else {\n");
    emit_stmts(c, nt_ref(nt, else_clause, "statements"), b, indent + 1);
    emit_indent(b, indent);
    buf_puts(b, "}\n");
  }
}

static void emit_while(Compiler *c, int id, Buf *b, int indent, int is_until) {
  const NodeTable *nt = c->nt;
  int pred = nt_ref(nt, id, "predicate");
  int body = nt_ref(nt, id, "statements");
  emit_indent(b, indent);
  buf_puts(b, "while (");
  if (is_until) buf_puts(b, "!(");
  emit_cond(c, pred, b);
  if (is_until) buf_puts(b, ")");
  buf_puts(b, ") {\n");
  emit_stmts(c, body, b, indent + 1);
  emit_indent(b, indent);
  buf_puts(b, "}\n");
}

static void emit_return(Compiler *c, int id, Buf *b, int indent) {
  int args = nt_ref(c->nt, id, "arguments");
  int n = 0;
  const int *a = args >= 0 ? nt_arr(c->nt, args, "arguments", &n) : NULL;
  emit_indent(b, indent);
  if (n > 0) { buf_puts(b, "return "); emit_expr(c, a[0], b); buf_puts(b, ";\n"); }
  else buf_puts(b, "return;\n");
}

static void emit_stmt_inner(Compiler *c, int id, Buf *b, int indent);
static void emit_stmt_tail_inner(Compiler *c, int id, Buf *b, int indent);

/* Wrap a line-emitting statement so any expression preludes are flushed
   before the line itself. */
static void emit_with_prelude(Compiler *c, int id, Buf *b, int indent,
                              void (*inner)(Compiler *, int, Buf *, int)) {
  Buf *savePre = g_pre;
  int saveIndent = g_indent;
  Buf pre;  memset(&pre, 0, sizeof pre);
  Buf line; memset(&line, 0, sizeof line);
  g_pre = &pre;
  g_indent = indent;
  inner(c, id, &line, indent);
  g_pre = savePre;
  g_indent = saveIndent;
  if (pre.p)  buf_puts(b, pre.p);
  if (line.p) buf_puts(b, line.p);
  free(pre.p);
  free(line.p);
}

static void emit_stmt(Compiler *c, int id, Buf *b, int indent) {
  emit_with_prelude(c, id, b, indent, emit_stmt_inner);
}
static void emit_stmt_tail(Compiler *c, int id, Buf *b, int indent) {
  emit_with_prelude(c, id, b, indent, emit_stmt_tail_inner);
}

static void emit_stmt_inner(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) unsupported(c, id, "statement (no type)");

  if (!strcmp(ty, "CallNode")) {
    if (emit_output_call(c, id, b, indent)) return;
    if (emit_iteration_stmt(c, id, b, indent)) return;
    /* attr writer: obj.x = v */
    {
      const char *nm = nt_str(nt, id, "name");
      int recv = nt_ref(nt, id, "receiver");
      size_t ln = nm ? strlen(nm) : 0;
      if (nm && recv >= 0 && ln >= 2 && nm[ln - 1] == '=') {
        TyKind rt = comp_ntype(c, recv);
        if (ty_is_object(rt)) {
          char base[256];
          if (ln - 1 < sizeof base) {
            memcpy(base, nm, ln - 1); base[ln - 1] = '\0';
            if (comp_writer_in_chain(c, ty_object_class(rt), base, NULL)) {
              int args = nt_ref(nt, id, "arguments");
              int an = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
              if (an >= 1) {
                emit_indent(b, indent);
                buf_puts(b, "("); emit_expr(c, recv, b); buf_printf(b, ").iv_%s = ", base);
                emit_expr(c, argv[0], b); buf_puts(b, ";\n");
                return;
              }
            }
          }
        }
      }
    }
    if (emit_array_mutate_stmt(c, id, b, indent)) return;
    emit_indent(b, indent);
    emit_expr(c, id, b);
    buf_puts(b, ";\n");
    return;
  }
  if (!strcmp(ty, "LocalVariableWriteNode")) { emit_assign(c, id, b, indent); return; }
  if (!strcmp(ty, "LocalVariableOperatorWriteNode")) { emit_op_assign(c, id, b, indent); return; }
  if (!strcmp(ty, "InstanceVariableWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    emit_indent(b, indent);
    buf_printf(b, "self->iv_%s = ", nm + 1);
    const char *vty = nt_type(nt, v);
    int sc = comp_scope_of(c, id)->class_id;
    TyKind ivt = TY_INT;
    if (sc >= 0) { int iv = comp_ivar_index(&c->classes[sc], nm); if (iv >= 0) ivt = c->classes[sc].ivar_types[iv]; }
    if (vty && !strcmp(vty, "NilNode")) {
      if (ivt == TY_RANGE) buf_puts(b, "(sp_Range){0}");
      else buf_puts(b, default_value(ivt));
    } else {
      emit_expr(c, v, b);
    }
    buf_puts(b, ";\n");
    return;
  }
  if (!strcmp(ty, "InstanceVariableOperatorWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    const char *op = nt_str(nt, id, "binary_operator");
    int sc = comp_scope_of(c, id)->class_id;
    TyKind vt = TY_UNKNOWN;
    if (sc >= 0) { int iv = comp_ivar_index(&c->classes[sc], nm); if (iv >= 0) vt = c->classes[sc].ivar_types[iv]; }
    emit_indent(b, indent);
    if (vt == TY_STRING && op && !strcmp(op, "+")) {
      buf_printf(b, "self->iv_%s = sp_str_concat(self->iv_%s, ", nm + 1, nm + 1);
      emit_expr(c, nt_ref(nt, id, "value"), b); buf_puts(b, ");\n");
    } else {
      buf_printf(b, "self->iv_%s %s= ", nm + 1, op ? op : "+");
      emit_expr(c, nt_ref(nt, id, "value"), b); buf_puts(b, ";\n");
    }
    return;
  }
  if (!strcmp(ty, "GlobalVariableWriteNode") || !strcmp(ty, "ConstantWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int isg = ty[0] == 'G';
    const char *pfx = isg ? "gv" : "cst";
    const char *key = isg ? nm + 1 : nm;
    LocalVar *lv = isg ? comp_gvar(c, key) : comp_const(c, key);
    if (!lv) { /* not registered (non-ident name or class const) -> ignore */ return; }
    int v = nt_ref(nt, id, "value");
    emit_indent(b, indent);
    buf_printf(b, "%s_%s = ", pfx, key);
    const char *vty = nt_type(nt, v);
    if (vty && !strcmp(vty, "NilNode")) buf_puts(b, lv->type == TY_RANGE ? "(sp_Range){0}" : default_value(lv->type));
    else emit_expr(c, v, b);
    buf_puts(b, ";\n");
    return;
  }
  if (!strcmp(ty, "GlobalVariableOperatorWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? comp_gvar(c, nm + 1) : NULL;
    if (!lv) return;
    const char *op = nt_str(nt, id, "binary_operator");
    int v = nt_ref(nt, id, "value");
    emit_indent(b, indent);
    if (lv->type == TY_STRING && op && !strcmp(op, "+")) {
      buf_printf(b, "gv_%s = sp_str_concat(gv_%s, ", nm + 1, nm + 1);
      emit_expr(c, v, b); buf_puts(b, ");\n");
    } else {
      buf_printf(b, "gv_%s %s= ", nm + 1, op ? op : "+");
      emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    return;
  }
  if (!strcmp(ty, "MultiWriteNode")) {
    int ln = 0;
    const int *lefts = nt_arr(nt, id, "lefts", &ln);
    int value = nt_ref(nt, id, "value");
    const char *vty = nt_type(nt, value);
    int en = 0;
    const int *els = (vty && !strcmp(vty, "ArrayNode")) ? nt_arr(nt, value, "elements", &en) : NULL;
    if (!els || en < ln) unsupported(c, id, "multiple assignment");
    /* evaluate all RHS values into temps first (so `a, b = b, a` swaps) */
    int base = g_tmp + 1;
    for (int i = 0; i < ln; i++) {
      int t = ++g_tmp;
      Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, els[i], &vb);
      emit_indent(b, indent);
      emit_ctype(c, comp_ntype(c, els[i]), b);
      buf_printf(b, " _t%d = ", t);
      buf_puts(b, vb.p ? vb.p : ""); buf_puts(b, ";\n"); free(vb.p);
    }
    for (int i = 0; i < ln; i++) {
      const char *lty = nt_type(nt, lefts[i]);
      if (lty && !strcmp(lty, "LocalVariableTargetNode")) {
        emit_indent(b, indent);
        buf_printf(b, "lv_%s = _t%d;\n", nt_str(nt, lefts[i], "name"), base + i);
      }
    }
    return;
  }
  if (!strcmp(ty, "ClassNode") || !strcmp(ty, "ModuleNode")) { return; } /* methods emitted separately */
  if (!strcmp(ty, "SuperNode") || !strcmp(ty, "ForwardingSuperNode")) {
    emit_indent(b, indent); emit_super(c, id, b); buf_puts(b, ";\n"); return;
  }
  if (!strcmp(ty, "IndexOperatorWriteNode")) { emit_index_op_write(c, id, b, indent); return; }
  if (!strcmp(ty, "IfNode"))     { emit_if(c, id, b, indent, 0, 0); return; }
  if (!strcmp(ty, "UnlessNode")) { emit_if(c, id, b, indent, 1, 0); return; }
  if (!strcmp(ty, "WhileNode"))  { emit_while(c, id, b, indent, 0); return; }
  if (!strcmp(ty, "UntilNode"))  { emit_while(c, id, b, indent, 1); return; }
  if (!strcmp(ty, "CaseNode"))   { emit_case(c, id, b, indent); return; }
  if (!strcmp(ty, "ReturnNode")) { emit_return(c, id, b, indent); return; }
  if (!strcmp(ty, "DefNode"))    { return; } /* emitted separately */

  unsupported(c, id, "statement");
}

/* Tail position: the value of this statement is the method's return value. */
static void emit_stmt_tail_inner(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) unsupported(c, id, "tail statement (no type)");

  if (!strcmp(ty, "IfNode"))     { emit_if(c, id, b, indent, 0, 1); return; }
  if (!strcmp(ty, "UnlessNode")) { emit_if(c, id, b, indent, 1, 1); return; }
  if (!strcmp(ty, "ReturnNode")) { emit_return(c, id, b, indent); return; }

  /* statements that don't produce a usable tail value: emit normally;
     the trailing default return covers the method's value. */
  if (!strcmp(ty, "LocalVariableWriteNode") ||
      !strcmp(ty, "LocalVariableOperatorWriteNode") ||
      !strcmp(ty, "WhileNode") || !strcmp(ty, "UntilNode") ||
      (!strcmp(ty, "CallNode") && nt_ref(nt, id, "receiver") < 0 &&
       emit_output_call(c, id, b, indent))) {
    if (strcmp(ty, "CallNode") != 0) emit_stmt(c, id, b, indent);
    return;
  }

  /* a value expression: return it */
  emit_indent(b, indent);
  buf_puts(b, "return ");
  emit_expr(c, id, b);
  buf_puts(b, ";\n");
}

static void emit_stmts(Compiler *c, int id, Buf *b, int indent) {
  if (id < 0) return;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (ty && !strcmp(ty, "StatementsNode")) {
    int n = 0;
    const int *body = nt_arr(nt, id, "body", &n);
    for (int k = 0; k < n; k++) emit_stmt(c, body[k], b, indent);
  } else {
    emit_stmt(c, id, b, indent);
  }
}

static void emit_stmts_tail(Compiler *c, int id, Buf *b, int indent) {
  if (id < 0) return;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (ty && !strcmp(ty, "StatementsNode")) {
    int n = 0;
    const int *body = nt_arr(nt, id, "body", &n);
    for (int k = 0; k < n; k++) {
      if (k == n - 1) emit_stmt_tail(c, body[k], b, indent);
      else emit_stmt(c, body[k], b, indent);
    }
  } else {
    emit_stmt_tail(c, id, b, indent);
  }
}

/* ---- declarations ---- */

/* Heap-managed types need a GC root for their local slot. */
static int needs_root(TyKind t) { return t == TY_STRING || ty_is_array(t) || ty_is_hash(t); }

static void declare_local(Compiler *c, Buf *b, LocalVar *lv) {
  switch (lv->type) {
    case TY_INT:    buf_printf(b, "    mrb_int lv_%s = 0;\n", lv->name); break;
    case TY_FLOAT:  buf_printf(b, "    mrb_float lv_%s = 0.0;\n", lv->name); break;
    case TY_BOOL:   buf_printf(b, "    mrb_bool lv_%s = 0;\n", lv->name); break;
    case TY_STRING:
      buf_printf(b, "    const char * lv_%s = (&(\"\\xff\")[1]);\n", lv->name);
      buf_printf(b, "    SP_GC_ROOT(lv_%s);\n", lv->name);
      break;
    case TY_SYMBOL:
      buf_printf(b, "    sp_sym lv_%s = ((sp_sym)-1);\n", lv->name);
      break;
    case TY_RANGE:
      buf_printf(b, "    sp_Range lv_%s = {0};\n", lv->name);
      break;
    case TY_INT_ARRAY:
    case TY_FLOAT_ARRAY:
    case TY_STR_ARRAY:
      buf_printf(b, "    %s lv_%s = NULL;\n", c_type_name(lv->type), lv->name);
      buf_printf(b, "    SP_GC_ROOT(lv_%s);\n", lv->name);
      break;
    default:
      if (ty_is_hash(lv->type)) {
        buf_printf(b, "    %s lv_%s = NULL;\n", c_type_name(lv->type), lv->name);
        buf_printf(b, "    SP_GC_ROOT(lv_%s);\n", lv->name);
        break;
      }
      if (ty_is_object(lv->type)) {
        buf_puts(b, "    ");
        emit_ctype(c, lv->type, b);
        buf_printf(b, " lv_%s = {0};\n", lv->name);
        break;
      }
      fprintf(stderr, "spinelc: local '%s' has unsupported type %s\n",
              lv->name, ty_name(lv->type));
      exit(1);
  }
}

/* Declare a scope's locals. Params are already C function parameters, so
   they only need a GC root; body locals get a full declaration. */
static void emit_scope_decls(Compiler *c, Scope *s, Buf *b) {
  for (int i = 0; i < s->nlocals; i++) {
    LocalVar *lv = &s->locals[i];
    if (lv->is_param) {
      if (needs_root(lv->type)) buf_printf(b, "    SP_GC_ROOT(lv_%s);\n", lv->name);
    } else {
      declare_local(c, b, lv);
    }
  }
}

/* ---- methods ---- */

static int method_is_void(Scope *s) {
  /* initialize is always void (mutates *self); else by return type */
  if (s->class_id >= 0 && s->name && !strcmp(s->name, "initialize")) return 1;
  return !is_scalar_ret(s->ret);
}

/* The mangled C name: sp_<name> for free functions, sp_<Class>_<name>
   for instance methods. */
static void emit_method_cname(Compiler *c, Scope *s, Buf *b) {
  if (s->class_id >= 0)
    buf_printf(b, "sp_%s_%s", c->classes[s->class_id].name, s->name);
  else
    buf_printf(b, "sp_%s", s->name);
}

static void emit_method_signature(Compiler *c, Scope *s, Buf *b) {
  if (method_is_void(s)) buf_puts(b, "static void ");
  else { buf_puts(b, "static "); emit_ctype(c, s->ret, b); buf_puts(b, " "); }
  emit_method_cname(c, s, b);
  buf_puts(b, "(");
  int wrote = 0;
  if (s->class_id >= 0) {
    /* self: by pointer (reference semantics for ivar mutation) */
    buf_printf(b, "sp_%s *self", c->classes[s->class_id].name);
    wrote = 1;
  }
  for (int i = 0; i < s->nparams; i++) {
    if (wrote++) buf_puts(b, ", ");
    LocalVar *p = scope_local(s, s->pnames[i]);
    TyKind pt = p ? p->type : TY_UNKNOWN;
    if (!is_scalar_ret(pt)) {
      fprintf(stderr, "spinelc: method '%s' param '%s' has unsupported type %s\n",
              s->name, s->pnames[i], ty_name(pt));
      exit(1);
    }
    emit_ctype(c, pt, b);
    buf_printf(b, " lv_%s", s->pnames[i]);
  }
  if (!wrote) buf_puts(b, "void");
  buf_puts(b, ")");
}

static void emit_method(Compiler *c, Scope *s, Buf *b) {
  emit_method_signature(c, s, b);
  buf_puts(b, " {\n");
  buf_puts(b, "    SP_GC_SAVE();\n");
  emit_scope_decls(c, s, b);
  if (method_is_void(s)) {
    emit_stmts(c, s->body, b, 1);
  } else {
    emit_stmts_tail(c, s->body, b, 1);
    buf_puts(b, "  return ");
    if (ty_is_object(s->ret)) { emit_ctype(c, s->ret, b); buf_puts(b, "){0}"); /* unreachable default */ buf_puts(b, ";\n"); }
    else buf_printf(b, "%s;\n", default_value(s->ret));
  }
  buf_puts(b, "}\n");
}

/* Emit the struct + the constructor (sp_<Class>_new) for one class. */
static void emit_class_struct(Compiler *c, ClassInfo *ci, Buf *b) {
  buf_printf(b, "typedef struct sp_%s_s sp_%s;\n", ci->name, ci->name);
  buf_printf(b, "struct sp_%s_s {\n", ci->name);
  buf_puts(b, "  mrb_int cls_id;\n");  /* runtime class tag for virtual dispatch */
  for (int i = 0; i < ci->nivars; i++) {
    TyKind t = ci->ivar_types[i];
    if (!is_scalar_ret(t) && t != TY_UNKNOWN) { /* ok */ }
    buf_puts(b, "  ");
    emit_ctype(c, t == TY_UNKNOWN ? TY_INT : t, b);
    /* ivar name includes '@'; strip it for the field */
    buf_printf(b, " iv_%s;\n", ci->ivars[i] + 1);
  }
  buf_puts(b, "};\n");
}

static void emit_class_new(Compiler *c, ClassInfo *ci, Buf *b) {
  int cid = comp_class_index(c, ci->name);
  int initcls = cid;
  int init = comp_method_in_chain(c, cid, "initialize", &initcls);
  buf_printf(b, "static sp_%s sp_%s_new(", ci->name, ci->name);
  if (init >= 0 && c->scopes[init].nparams > 0) {
    Scope *s = &c->scopes[init];
    for (int i = 0; i < s->nparams; i++) {
      if (i) buf_puts(b, ", ");
      LocalVar *p = scope_local(s, s->pnames[i]);
      emit_ctype(c, p ? p->type : TY_INT, b);
      buf_printf(b, " lv_%s", s->pnames[i]);
    }
  } else {
    buf_puts(b, "void");
  }
  buf_printf(b, ") {\n  sp_%s self = {0};\n", ci->name);
  buf_printf(b, "  self.cls_id = %d;\n", cid);
  if (init >= 0) {
    /* an inherited initialize takes the parent's self type */
    buf_printf(b, "  sp_%s_initialize(", c->classes[initcls].name);
    if (initcls != cid) buf_printf(b, "(sp_%s *)", c->classes[initcls].name);
    buf_puts(b, "&self");
    Scope *s = &c->scopes[init];
    for (int i = 0; i < s->nparams; i++) buf_printf(b, ", lv_%s", s->pnames[i]);
    buf_puts(b, ");\n");
  }
  buf_puts(b, "  return self;\n}\n");
}

/* super(args) / super -> call the parent's same-named method. */
static void emit_super(Compiler *c, int id, Buf *b) {
  Scope *s = comp_scope_of(c, id);
  if (s->class_id < 0 || !s->name) { unsupported(c, id, "super (not in a method)"); }
  int p = c->classes[s->class_id].parent;
  int defcls = -1;
  int mi = p >= 0 ? comp_method_in_chain(c, p, s->name, &defcls) : -1;
  if (mi < 0) unsupported(c, id, "super (no parent method)");
  buf_printf(b, "sp_%s_%s((sp_%s *)self", c->classes[defcls].name, s->name, c->classes[defcls].name);
  /* explicit args, or forward the current method's params for bare super */
  const char *ty = nt_type(c->nt, id);
  if (ty && !strcmp(ty, "ForwardingSuperNode")) {
    for (int i = 0; i < s->nparams; i++) buf_printf(b, ", lv_%s", s->pnames[i]);
  } else {
    int args = nt_ref(c->nt, id, "arguments");
    int argc = 0; const int *argv = NULL;
    if (args >= 0) argv = nt_arr(c->nt, args, "arguments", &argc);
    for (int k = 0; k < argc; k++) { buf_puts(b, ", "); emit_expr(c, argv[k], b); }
  }
  buf_puts(b, ")");
}

/* ---- top level ---- */

char *codegen_program(const NodeTable *nt) {
  Compiler *c = comp_new(nt);
  analyze_program(c);

  Buf b; memset(&b, 0, sizeof b);
  buf_puts(&b, "/* Generated by Spinel AOT compiler */\n");
  buf_puts(&b, "#include \"sp_runtime.h\"\n");
  if (c->nsymbols > 0) {
    buf_printf(&b, "static const char *const sp_sym_names[%d] = {", c->nsymbols);
    for (int i = 0; i < c->nsymbols; i++) {
      if (i) buf_puts(&b, ", ");
      emit_str_literal(&b, c->symbols[i]);
    }
    buf_puts(&b, "};\n");
    buf_printf(&b, "static const char *sp_sym_to_s(sp_sym id){if(id>=0&&id<%d)return sp_sym_names[id];return \"\";}\n\n", c->nsymbols);
  } else {
    buf_puts(&b, "static const char *sp_sym_to_s(sp_sym id){(void)id;return \"\";}\n\n");
  }
  buf_puts(&b, "static const char *sp_class_to_s(sp_Class c){(void)c;return \"\";}\n\n\n");

  /* class structs */
  for (int i = 0; i < c->nclasses; i++) emit_class_struct(c, &c->classes[i], &b);
  if (c->nclasses > 0) buf_puts(&b, "\n");

  /* method prototypes (scope 0 is top-level) */
  for (int s = 1; s < c->nscopes; s++) { emit_method_signature(c, &c->scopes[s], &b); buf_puts(&b, ";\n"); }
  /* constructor prototypes + definitions (after method protos: new calls initialize) */
  for (int i = 0; i < c->nclasses; i++) {
    buf_printf(&b, "static sp_%s sp_%s_new();\n", c->classes[i].name, c->classes[i].name);
  }
  if (c->nscopes > 1 || c->nclasses > 0) buf_puts(&b, "\n");
  for (int i = 0; i < c->nclasses; i++) emit_class_new(c, &c->classes[i], &b);
  for (int s = 1; s < c->nscopes; s++) emit_method(c, &c->scopes[s], &b);

  /* global variables and top-level constants (file-scope statics) */
  for (int i = 0; i < c->ngvars; i++) {
    LocalVar *lv = &c->gvars[i];
    if (!is_scalar_ret(lv->type)) continue;
    buf_puts(&b, "static ");
    emit_ctype(c, lv->type, &b);
    buf_printf(&b, " gv_%s = %s;\n", lv->name,
               lv->type == TY_RANGE ? "{0}" : default_value(lv->type));
  }
  for (int i = 0; i < c->nconsts; i++) {
    LocalVar *lv = &c->consts[i];
    if (!is_scalar_ret(lv->type)) continue;
    buf_puts(&b, "static ");
    emit_ctype(c, lv->type, &b);
    buf_printf(&b, " cst_%s = %s;\n", lv->name,
               lv->type == TY_RANGE ? "{0}" : default_value(lv->type));
  }
  if (c->ngvars || c->nconsts) buf_puts(&b, "\n");

  buf_puts(&b, "int main(int argc,char**argv){\n");
  buf_puts(&b, "    SP_GC_SAVE();\n");
  emit_scope_decls(c, &c->scopes[0], &b);
  buf_puts(&b, "\n");
  emit_stmts(c, c->scopes[0].body, &b, 1);
  buf_puts(&b, "  return 0;\n}\n");

  comp_free(c);
  return b.p;
}
