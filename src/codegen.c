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

/* Inlining a yielding method: method-local names are renamed (to avoid
   clashing with the call site's locals), and yield emits the active
   block's body. g_block_id is the current BlockNode for yield (-1 if
   none). The rename map holds only the inlined method's locals. */
#define MAX_RENAME 128
static char g_ren_from[MAX_RENAME][96];
static char g_ren_to[MAX_RENAME][112];
static int  g_nren = 0;
static int  g_block_id = -1;
/* When a yielding method is inlined, g_yield_block_fallback holds the block
   that was active in the CALLER's context so nested `yield`s inside the
   passed block can chain back to the outermost caller's block. */
static int  g_yield_block_fallback = -1;
/* Name of the `&block` parameter of the method currently being inlined, so
   `<blk>.call(args)` inside it expands the active block like `yield args`. */
static const char *g_block_param_name = NULL;
/* The C expression for `self` (a pointer). Overridden while inlining an
   instance method at a call site (where there is no real `self` param). */
static const char *g_self = "self";
/* When emitting class/module body statements, the class index (-1 outside). */
static int g_class_body_id = -1;
/* While emitting a rescue handler: the C var names holding the caught
   exception's class/message, so a bare `raise` can re-raise. */
static const char *g_rescue_cls = NULL, *g_rescue_msg = NULL;
/* When set, tail positions assign to this var instead of `return`ing
   (used to give a begin/rescue a value). */
static const char *g_result_var = NULL;
/* When g_result_var is set, whether that result slot is poly (so a scalar
   tail value must be boxed into it). */
static int g_result_poly = 0;
/* Return type of the method currently being emitted, so a tail/return value
   can be boxed when the method returns poly but the value is concrete. */
static TyKind g_ret_type = TY_UNKNOWN;

/* Ensure context stack for deferred `return` inside begin..ensure.
   When `return` appears in the body of a begin..ensure block, the return
   is deferred until after the ensure clause runs.  Each ensure clause
   pushes a context on this stack; emit_return uses the top to emit a
   deferred goto instead of a bare C `return`. */
#define MAX_ENSURE_DEPTH 32
typedef struct { int lid; int has_retval; } EnsureCtx;
static EnsureCtx g_ensure_stack[MAX_ENSURE_DEPTH];
static int       g_ensure_depth = 0;

/* First-class Proc support: each `proc {}` / `lambda {}` / `->{}` literal
   lowers to a standalone `static mrb_int _proc_N(void *cap, mrb_int *args)`
   function (the ABI sp_proc_call expects). Definitions accumulate in g_procs
   and prototypes in g_proc_protos during the main emission pass, then are
   flushed ahead of the method/main bodies that reference them. */
static Buf g_procs;
static Buf g_proc_protos;
static int g_proc_counter = 0;

/* Static regex-literal table: each distinct (source, flags) pair compiles once
   to an sp_re_pat_<i> global initialized in sp_re_init(). */
static char **g_re_src; static int *g_re_flg; static int g_re_count, g_re_cap;
/* Map Prism regex flag bits (IGNORE_CASE=4, EXTENDED=8, MULTI_LINE=16) to the
   engine's RE_FLAG_* (IGNORECASE=1, MULTILINE=2, DOTALL=4, EXTENDED=8); Ruby's
   /m means dot-matches-newline -> MULTILINE|DOTALL = 6. */
static int re_engine_flags(int pf) {
  int f = 0;
  if (pf & 4) f |= 1;
  if (pf & 8) f |= 8;
  if (pf & 16) f |= 6;
  return f;
}
/* True if a regex source contains a capturing group: an unescaped '(' that
   isn't the start of a non-capturing/extension group '(?...'. scan returns
   nested arrays for capturing patterns, which the str_array path can't model. */
static int re_has_captures(const char *src) {
  if (!src) return 0;
  for (const char *p = src; *p; p++) {
    if (*p == '\\') { if (p[1]) p++; continue; }
    if (*p == '(' && p[1] != '?') return 1;
  }
  return 0;
}

/* Find or add a RegularExpressionNode literal; returns its table index, or
   -1 if the node isn't a static regex literal. */
static int re_lit_index(Compiler *c, int nid) {
  if (nid < 0) return -1;
  const char *ty = nt_type(c->nt, nid);
  if (!ty) return -1;
  /* a constant bound to a regex literal (PATTERN = /re/[.freeze], possibly
     namespaced) resolves to that literal's precompiled pattern */
  if (!strcmp(ty, "ConstantReadNode") || !strcmp(ty, "ConstantPathNode")) {
    const char *nm = nt_str(c->nt, nid, "name");
    if (!nm) return -1;
    for (int k = 0; k < c->nt->count; k++) {
      const char *kt = nt_type(c->nt, k);
      if (!kt || (strcmp(kt, "ConstantWriteNode") && strcmp(kt, "ConstantPathWriteNode"))) continue;
      const char *kn = nt_str(c->nt, k, "name");
      if (!kn || strcmp(kn, nm)) continue;
      int v = nt_ref(c->nt, k, "value");
      if (v >= 0 && nt_type(c->nt, v) && !strcmp(nt_type(c->nt, v), "CallNode") &&
          nt_str(c->nt, v, "name") && !strcmp(nt_str(c->nt, v, "name"), "freeze"))
        v = nt_ref(c->nt, v, "receiver");
      if (v >= 0 && nt_type(c->nt, v) && !strcmp(nt_type(c->nt, v), "RegularExpressionNode"))
        return re_lit_index(c, v);
    }
    return -1;
  }
  /* a local variable of type TY_REGEX: look up its write node */
  if (!strcmp(ty, "LocalVariableReadNode") && comp_ntype(c, nid) == TY_REGEX) {
    const char *nm = nt_str(c->nt, nid, "name");
    if (!nm) return -1;
    for (int k = 0; k < c->nt->count; k++) {
      const char *kt = nt_type(c->nt, k);
      if (!kt || strcmp(kt, "LocalVariableWriteNode")) continue;
      const char *kn = nt_str(c->nt, k, "name");
      if (!kn || strcmp(kn, nm)) continue;
      int v = nt_ref(c->nt, k, "value");
      if (v >= 0 && nt_type(c->nt, v) && !strcmp(nt_type(c->nt, v), "RegularExpressionNode"))
        return re_lit_index(c, v);
    }
    return -1;
  }
  if (strcmp(ty, "RegularExpressionNode")) return -1;
  const char *src = nt_str(c->nt, nid, "unescaped");
  if (!src) return -1;
  int flg = re_engine_flags((int)nt_int(c->nt, nid, "flags", 0));
  for (int i = 0; i < g_re_count; i++)
    if (g_re_flg[i] == flg && !strcmp(g_re_src[i], src)) return i;
  if (g_re_count >= g_re_cap) {
    g_re_cap = g_re_cap ? g_re_cap * 2 : 8;
    g_re_src = realloc(g_re_src, sizeof(char *) * (size_t)g_re_cap);
    g_re_flg = realloc(g_re_flg, sizeof(int) * (size_t)g_re_cap);
  }
  g_re_src[g_re_count] = (char *)src;
  g_re_flg[g_re_count] = flg;
  return g_re_count++;
}
/* The unescaped source of a regex literal or a constant bound to one (for
   capture detection). Returns NULL when nid is not a resolvable regex. */
static const char *re_lit_src(Compiler *c, int nid) {
  if (nid < 0) return NULL;
  const char *ty = nt_type(c->nt, nid);
  if (!ty) return NULL;
  if (!strcmp(ty, "RegularExpressionNode")) return nt_str(c->nt, nid, "unescaped");
  if (!strcmp(ty, "ConstantReadNode") || !strcmp(ty, "ConstantPathNode")) {
    const char *nm = nt_str(c->nt, nid, "name");
    if (!nm) return NULL;
    for (int k = 0; k < c->nt->count; k++) {
      const char *kt = nt_type(c->nt, k);
      if (!kt || (strcmp(kt, "ConstantWriteNode") && strcmp(kt, "ConstantPathWriteNode"))) continue;
      const char *kn = nt_str(c->nt, k, "name");
      if (!kn || strcmp(kn, nm)) continue;
      int v = nt_ref(c->nt, k, "value");
      if (v >= 0 && nt_type(c->nt, v) && !strcmp(nt_type(c->nt, v), "CallNode") &&
          nt_str(c->nt, v, "name") && !strcmp(nt_str(c->nt, v, "name"), "freeze"))
        v = nt_ref(c->nt, v, "receiver");
      if (v >= 0 && nt_type(c->nt, v) && !strcmp(nt_type(c->nt, v), "RegularExpressionNode"))
        return nt_str(c->nt, v, "unescaped");
    }
  }
  return NULL;
}

static void emit_interp(Compiler *c, int id, Buf *b);  /* forward */

/* Emit a regex pattern expression to `b`, handling both static literals and
   interpolated patterns. For interpolated patterns, setup is emitted to
   g_pre and a temp mrb_regexp_pattern* variable name is written to `b`.
   Returns 1 if handled, 0 if nid is not a recognizable regex. */
static int emit_regex_pat_to_buf(Compiler *c, int nid, Buf *b) {
  int ri = re_lit_index(c, nid);
  if (ri >= 0) { buf_printf(b, "sp_re_pat_%d", ri); return 1; }
  const char *ty = nt_type(c->nt, nid);
  if (ty && !strcmp(ty, "InterpolatedRegularExpressionNode")) {
    int flg = re_engine_flags((int)nt_int(c->nt, nid, "flags", 0));
    int ts = ++g_tmp, tp = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "const char *_t%d = ", ts);
    emit_interp(c, nid, g_pre);
    buf_puts(g_pre, ";\n");
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "mrb_regexp_pattern *_t%d = re_compile(_t%d, (int64_t)strlen(_t%d), %d);\n", tp, ts, ts, flg);
    buf_printf(b, "_t%d", tp);
    return 1;
  }
  return 0;
}

/* A set of local names (borrowed pointers into the node table). */
typedef struct { const char **v; int n, cap; } NameSet;
static int nameset_has(NameSet *s, const char *nm) {
  if (!nm) return 0;
  for (int i = 0; i < s->n; i++) if (!strcmp(s->v[i], nm)) return 1;
  return 0;
}
static void nameset_add(NameSet *s, const char *nm) {
  if (!nm || nameset_has(s, nm)) return;
  if (s->n >= s->cap) { s->cap = s->cap ? s->cap * 2 : 8; s->v = realloc(s->v, sizeof(char *) * (size_t)s->cap); }
  s->v[s->n++] = nm;
}
/* While emitting a capturing proc's body: the cap struct's C type name and the
   set of captured names, so a read/write of a captured var routes to the cell
   held in `_cap` instead of a (non-existent) local. NULL outside such a body. */
static const char *g_cap_struct = NULL;
static NameSet *g_cap_names = NULL;
/* set when the program registers an at_exit hook; main()'s tail then runs them
   in reverse registration order. */
static int g_needs_at_exit = 0;

static const char *rename_local(const char *nm);

/* Emit the C lvalue for local `name` in the current emission context: a
   captured var inside a proc body -> the cell in _cap; a cell local in its
   enclosing scope -> `(*_cell_x)`; otherwise the plain `lv_x`. Reads and
   writes share this (a cell deref is a valid lvalue). */
static void emit_local_ref(Compiler *c, int scope_node, const char *name, Buf *b) {
  if (g_cap_struct && g_cap_names && nameset_has(g_cap_names, name)) {
    buf_printf(b, "(*((%s *)_cap)->%s)", g_cap_struct, name);
    return;
  }
  LocalVar *lv = scope_node >= 0 ? scope_local(comp_scope_of(c, scope_node), name) : NULL;
  if (lv && lv->is_cell) { buf_printf(b, "(*_cell_%s)", name); return; }
  buf_printf(b, "lv_%s", rename_local(name));
}

/* Emit the lead of a tail value: `return ` or `<result> = `. */
static void emit_tail_lead(Buf *b) {
  if (g_result_var) buf_printf(b, "%s = ", g_result_var);
  else buf_puts(b, "return ");
}

static const char *rename_local(const char *nm) {
  for (int i = 0; i < g_nren; i++)
    if (strcmp(g_ren_from[i], nm) == 0) return g_ren_to[i];
  return nm;
}

/* ---- diagnostics ---- */

static void unsupported(Compiler *c, int id, const char *what) {
  const char *ty = nt_type(c->nt, id);
  const char *mname = ty && !strcmp(ty, "CallNode") ? nt_str(c->nt, id, "name") : NULL;
  if (mname)
    fprintf(stderr, "spinelc: unsupported %s: node %d (%s `%s`)\n",
            what, id, ty, mname);
  else
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
    case TY_TIME:        return "sp_Time";
    case TY_STRINGIO:    return "sp_StringIO *";
    case TY_STRINGSCANNER: return "sp_StringScanner *";
    case TY_MATCHDATA:   return "sp_MatchData *";
    case TY_REGEX:       return "mrb_regexp_pattern *";
    case TY_EXCEPTION:   return "sp_Exception *";
    case TY_INT_ARRAY:   return "sp_IntArray *";
    case TY_FLOAT_ARRAY: return "sp_FloatArray *";
    case TY_STR_ARRAY:   return "sp_StrArray *";
    case TY_STR_INT_HASH: return "sp_StrIntHash *";
    case TY_STR_STR_HASH: return "sp_StrStrHash *";
    case TY_INT_INT_HASH: return "sp_IntIntHash *";
    case TY_INT_STR_HASH: return "sp_IntStrHash *";
    case TY_SYM_POLY_HASH:  return "sp_SymPolyHash *";
    case TY_STR_POLY_HASH:  return "sp_StrPolyHash *";
    case TY_POLY_POLY_HASH: return "sp_PolyPolyHash *";
    case TY_POLY:         return "sp_RbVal";
    case TY_POLY_ARRAY:   return "sp_PolyArray *";
    case TY_PROC:         return "sp_Proc *";
    default:             return NULL;
  }
}
static int is_scalar_ret(TyKind t) {
  return t == TY_INT || t == TY_FLOAT || t == TY_BOOL || t == TY_STRING ||
         t == TY_SYMBOL || t == TY_RANGE || t == TY_TIME || t == TY_STRINGIO || t == TY_STRINGSCANNER || t == TY_MATCHDATA || t == TY_REGEX || t == TY_EXCEPTION ||
         t == TY_INT_ARRAY || t == TY_FLOAT_ARRAY || t == TY_STR_ARRAY ||
         t == TY_POLY || t == TY_POLY_ARRAY || t == TY_PROC ||
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
    case TY_TIME:   return "(sp_Time){0}";
    case TY_STRINGIO: return "NULL";
    case TY_STRINGSCANNER: return "NULL";
    case TY_MATCHDATA:  return "NULL";
    case TY_REGEX:      return "NULL";
    case TY_EXCEPTION: return "NULL";
    case TY_INT_ARRAY:
    case TY_FLOAT_ARRAY:
    case TY_STR_ARRAY:
    case TY_POLY_ARRAY: return "NULL";
    case TY_PROC:    return "NULL";
    case TY_POLY:    return "sp_box_nil()";
    default:        return (ty_is_hash(t) || ty_is_object(t)) ? "NULL" : "0";
  }
}
/* Append the C type name for `t` to `b` (objects need the class name). */
static void emit_ctype(Compiler *c, TyKind t, Buf *b) {
  if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    buf_printf(b, "sp_%s *", c->classes[cid].name);  /* objects are heap pointers (reference semantics) */
  }
  else {
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

static void emit_c_escaped_n(Buf *b, const char *s, size_t len) {
  for (size_t i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (ch == '\\' || ch == '"') buf_printf(b, "\\%c", ch);
    else if (ch == '\n') buf_puts(b, "\\n");
    else if (ch == '\t') buf_puts(b, "\\t");
    else if (ch == '\r') buf_puts(b, "\\r");
    else if (ch >= 0x20 && ch < 0x7f) buf_printf(b, "%c", ch);
    else buf_printf(b, "\\%03o", ch);
  }
}

static void emit_c_escaped(Buf *b, const char *s) {
  if (s) emit_c_escaped_n(b, s, strlen(s));
}

/* Emit a Ruby string literal. len is the true byte count (may exceed strlen
   when the string contains embedded NUL bytes). */
static void emit_str_literal_n(Buf *b, const char *content, size_t len) {
  if (!content || len == 0) { buf_puts(b, "(&(\"\\xff\")[1])"); return; }
  /* NUL-containing strings: use sp_str_from_bytes with explicit byte count. */
  if (len > strlen(content)) {
    buf_puts(b, "sp_str_from_bytes(\"");
    emit_c_escaped_n(b, content, len);
    buf_printf(b, "\", %zu)", len);
    return;
  }
  buf_puts(b, "(&(\"\\xff\" \"");
  emit_c_escaped_n(b, content, len);
  buf_puts(b, "\")[1])");
}

static void emit_str_literal(Buf *b, const char *content) {
  if (!content) { buf_puts(b, "(&(\"\\xff\")[1])"); return; }
  emit_str_literal_n(b, content, strlen(content));
}

/* ---- forward decls ---- */

static void emit_method_cname(Compiler *c, Scope *s, Buf *b);
static void emit_expr(Compiler *c, int id, Buf *b);
static void emit_stmt(Compiler *c, int id, Buf *b, int indent);
static void emit_stmts(Compiler *c, int id, Buf *b, int indent);
static void emit_stmts_tail(Compiler *c, int id, Buf *b, int indent);
static void emit_op_assign(Compiler *c, int id, Buf *b, int indent);
static void emit_begin(Compiler *c, int id, Buf *b, int indent, const char *resultvar);
static int  emit_array_mutate_stmt(Compiler *c, int id, Buf *b, int indent);
static int  emit_output_call(Compiler *c, int id, Buf *b, int indent);
static int  emit_iteration_stmt(Compiler *c, int id, Buf *b, int indent);
static int  emit_inline_call(Compiler *c, int id, Buf *b, int indent);
static int  emit_inline_expr(Compiler *c, int id, Buf *b);
static void emit_cond(Compiler *c, int id, Buf *b);
static int  needs_root(TyKind t);
static int  method_is_void(Scope *s);
static void emit_index_op_write(Compiler *c, int id, Buf *b, int indent);
static void emit_index_and_or_write(Compiler *c, int id, Buf *b, int indent, int is_or);
static void emit_boxed(Compiler *c, int node, Buf *b);
static void emit_hash_key(Compiler *c, int key, TyKind kt, Buf *b) {
  TyKind actual = comp_ntype(c, key);
  if (actual == TY_POLY && kt != TY_POLY) {
    buf_puts(b, "(");
    emit_expr(c, key, b);
    buf_puts(b, kt == TY_STRING ? ").v.s" : ").v.i");  /* int/sym share v.i */
    return;
  }
  if (kt == TY_POLY && actual != TY_POLY) {
    /* PolyPolyHash key: box the typed value into sp_RbVal */
    emit_boxed(c, key, b);
    return;
  }
  emit_expr(c, key, b);
}
static void emit_super(Compiler *c, int id, Buf *b);
static int  emit_super_inline(Compiler *c, int id, Buf *b, int indent, int as_expr);
static void emit_args_filled(Compiler *c, int callee_idx, int argsNode, const char *lead, Buf *out);
static void emit_boxed(Compiler *c, int node, Buf *b);
/* Emit a hash key, unboxing a poly value to the typed-hash's key type. */
static void emit_hash_key(Compiler *c, int key, TyKind kt, Buf *b);
static void emit_boxed_text(Compiler *c, TyKind t, const char *expr, Buf *b);
static void emit_unbox_text(Compiler *c, TyKind t, const char *expr, Buf *b);
static void emit_proc_literal(Compiler *c, int create, Buf *b);
static int proc_slot_is_direct(TyKind t);
static int proc_slot_is_ptr(TyKind t);
static void emit_case_expr(Compiler *c, int id, Buf *b);

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

/* Mangle a Ruby method name into a C identifier: `?`->_p, `!`->_bang,
   `=`->_set, anything else non-identifier -> `_`. Returns a static buffer
   (one live result at a time -- fine since each use is consumed inline). */
static const char *mc(const char *name) {
  static char buf[256];
  int j = 0;
  for (const char *p = name; *p && j < (int)sizeof buf - 8; p++) {
    char ch = *p;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '_') { buf[j++] = ch; continue; }
    /* operator characters map to distinct tokens so that, e.g., `&` and `|`
       (or `<<` and `>>`) don't mangle to the same C identifier */
    const char *tok;
    switch (ch) {
      case '?': tok = "_p";     break;
      case '!': tok = "_bang";  break;
      case '=': tok = "_set";   break;
      case '+': tok = "_plus";  break;
      case '-': tok = "_minus"; break;
      case '*': tok = "_star";  break;
      case '/': tok = "_slash"; break;
      case '%': tok = "_pct";   break;
      case '<': tok = "_lt";    break;
      case '>': tok = "_gt";    break;
      case '&': tok = "_amp";   break;
      case '|': tok = "_bar";   break;
      case '^': tok = "_caret"; break;
      case '~': tok = "_tilde"; break;
      case '@': tok = "_at";    break;
      case '[': tok = "_lb";    break;
      case ']': tok = "_rb";    break;
      default:  tok = "_";      break;
    }
    size_t tl = strlen(tok);
    memcpy(buf + j, tok, tl); j += (int)tl;
  }
  buf[j] = '\0';
  return buf;
}

/* A class method scope is shadowed (and must not be emitted) when a later
   scope redefines the same (class, name, is_cmethod) -- a reopened class
   where the last definition wins, matching comp_method_in_class. */
static int scope_is_shadowed(Compiler *c, int s) {
  Scope *sc = &c->scopes[s];
  if (sc->class_id < 0 || !sc->name) return 0;
  for (int k = s + 1; k < c->nscopes; k++) {
    Scope *o = &c->scopes[k];
    if (o->class_id == sc->class_id && o->is_cmethod == sc->is_cmethod &&
        o->name && !strcmp(o->name, sc->name)) return 1;
  }
  return 0;
}

/* Value node for keyword `name` inside a KeywordHashNode, or -1. */
static int struct_kwarg_value(Compiler *c, int kwh, const char *name) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *els = nt_arr(nt, kwh, "elements", &n);
  for (int i = 0; i < n; i++) {
    if (!nt_type(nt, els[i]) || strcmp(nt_type(nt, els[i]), "AssocNode")) continue;
    int key = nt_ref(nt, els[i], "key");
    if (key >= 0 && nt_type(nt, key) && !strcmp(nt_type(nt, key), "SymbolNode")) {
      const char *kn = nt_str(nt, key, "value");
      if (kn && !strcmp(kn, name)) return nt_ref(nt, els[i], "value");
    }
  }
  return -1;
}

/* Value-equality family: operands in the same nonzero family compare by value;
   different nonzero families are never == (Ruby does no cross-type coercion,
   except int/float which share family 1). 0 = not a simple comparable type. */
static int eq_family(TyKind t) {
  if (ty_is_numeric(t)) return 1;
  if (t == TY_STRING) return 2;
  if (t == TY_BOOL) return 3;
  if (t == TY_SYMBOL) return 4;
  if (t == TY_RANGE) return 5;
  return 0;
}

/* Compile-time `is_a?` for a concrete builtin receiver type: 1 yes, 0 no,
   -1 not determinable here. `exact` is instance_of? (no ancestor match). */
static int ty_matches_class(TyKind t, const char *cn, int exact) {
  const char *self_cls = NULL;
  if (t == TY_STRING) self_cls = "String";
  else if (t == TY_INT) self_cls = "Integer";
  else if (t == TY_FLOAT) self_cls = "Float";
  else if (t == TY_SYMBOL) self_cls = "Symbol";
  else if (t == TY_RANGE) self_cls = "Range";
  else if (ty_is_array(t)) self_cls = "Array";
  else if (ty_is_hash(t)) self_cls = "Hash";
  if (!self_cls) return -1;
  if (!strcmp(cn, self_cls)) return 1;
  if (exact) return 0;
  if (!strcmp(cn, "Object") || !strcmp(cn, "BasicObject") || !strcmp(cn, "Kernel")) return 1;
  if (!strcmp(cn, "Comparable") && (t == TY_STRING || t == TY_INT || t == TY_FLOAT || t == TY_SYMBOL)) return 1;
  if (!strcmp(cn, "Numeric") && (t == TY_INT || t == TY_FLOAT)) return 1;
  if (!strcmp(cn, "Enumerable") && (ty_is_array(t) || ty_is_hash(t) || t == TY_RANGE)) return 1;
  return 0;
}

static void emit_method_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int mi = comp_method_index(c, name);
  buf_printf(b, "sp_%s(", mc(name));
  emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), "", b);
  buf_puts(b, ")");
}

/* hash.map / collect { |k, v| ... } as an expression -> an array of the block
   values, built via a loop over the hash entries in the statement prelude. */
static int emit_hash_collect_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (strcmp(name, "map") && strcmp(name, "collect")) return 0;  /* map only for now */
  TyKind rt = comp_ntype(c, recv);
  const char *hn = ty_hash_cname(rt);
  if (!hn) return 0;
  TyKind restype = comp_ntype(c, id);
  int res_poly = (restype == TY_POLY_ARRAY);
  const char *rk = res_poly ? "Poly" : array_kind(restype);
  if (!rk) return 0;
  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  const char *p1 = block_param_name(c, block, 1); if (p1) p1 = rename_local(p1);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  int trecv = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", trecv); emit_expr(c, recv, g_pre); buf_puts(g_pre, ";\n");
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk, tres, rk);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
  if (p0) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = _t%d->order[_t%d];\n", p0, trecv, ti); }
  if (p1) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sHash_get(_t%d, _t%d->order[_t%d]);\n", p1, hn, trecv, trecv, ti); }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
  int save = g_indent; g_indent++;
  Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = save;
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_%sArray_push(_t%d, ", rk, tres);
  if (res_poly && comp_ntype(c, bb[bn - 1]) != TY_POLY) { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, comp_ntype(c, bb[bn - 1]), vb.p ? vb.p : "", &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p); }
  else buf_puts(g_pre, vb.p ? vb.p : "");
  buf_puts(g_pre, ");\n"); free(vb.p);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tres);
  return 1;
}

/* hash.transform_keys { |k| nk } / transform_values { |v| nv }: rebuild the
   hash applying the block to every key (or value), keeping the other half.
   Returns 1 if handled. */
static int emit_transform_hash_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || (strcmp(name, "transform_keys") && strcmp(name, "transform_values"))) return 0;
  int keys = !strcmp(name, "transform_keys");
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  const char *shn = ty_hash_cname(rt);
  if (!shn) return 0;
  TyKind dt = comp_ntype(c, id);
  const char *dhn = ty_hash_cname(dt);
  if (!dhn) return 0;
  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  TyKind skt = ty_hash_key(rt), svt = ty_hash_val(rt);
  TyKind dvt = ty_hash_val(dt);
  int ts = ++g_tmp, td = ++g_tmp, ti = ++g_tmp, tk = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);  /* recv preludes flush to g_pre first */
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", ts); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
  emit_indent(g_pre, g_indent); emit_ctype(c, dt, g_pre); buf_printf(g_pre, " _t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);\n", td, dhn, td);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, ts, ti);
  emit_indent(g_pre, g_indent + 1); emit_ctype(c, skt, g_pre); buf_printf(g_pre, " _t%d = _t%d->order[_t%d];\n", tk, ts, ti);
  if (p0) {
    emit_indent(g_pre, g_indent + 1);
    if (keys) buf_printf(g_pre, "lv_%s = _t%d;\n", p0, tk);
    else buf_printf(g_pre, "lv_%s = sp_%sHash_get(_t%d, _t%d);\n", p0, shn, ts, tk);
  }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
  int save = g_indent; g_indent++;
  Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = save;
  TyKind bret = comp_ntype(c, bb[bn - 1]);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_%sHash_set(_t%d, ", dhn, td);
  if (keys) {
    /* new key = block result; value carried over (box if dst value is poly) */
    buf_puts(g_pre, vb.p ? vb.p : "0"); buf_puts(g_pre, ", ");
    if (dvt == TY_POLY && svt != TY_POLY) { Buf bx; memset(&bx, 0, sizeof bx); char g[64]; snprintf(g, sizeof g, "sp_%sHash_get(_t%d, _t%d)", shn, ts, tk); emit_boxed_text(c, svt, g, &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p); }
    else buf_printf(g_pre, "sp_%sHash_get(_t%d, _t%d)", shn, ts, tk);
  }
  else {
    /* key carried over; new value = block result (box if dst value is poly) */
    buf_printf(g_pre, "_t%d, ", tk);
    if (dvt == TY_POLY && bret != TY_POLY) { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, bret, vb.p ? vb.p : "", &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p); }
    else buf_puts(g_pre, vb.p ? vb.p : "0");
  }
  buf_puts(g_pre, ");\n"); free(vb.p);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", td);
  return 1;
}

/* (lo..hi).bsearch { |x| cond } in find-minimum mode: binary search for the
   smallest member where the block is truthy, or nil (the SP_INT_NIL sentinel)
   when none qualifies. Loop in the statement prelude; value is the result.
   Returns 1 if handled. */
static int emit_bsearch_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || strcmp(name, "bsearch")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || comp_ntype(c, recv) != TY_RANGE) return 0;
  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  int tr = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp, tres = ++g_tmp, tmid = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_Range _t%d = ", tr); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = _t%d.first;\n", tlo, tr);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = _t%d.last - _t%d.excl;\n", thi, tr, tr);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = SP_INT_NIL;\n", tres);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "while (_t%d <= _t%d) {\n", tlo, thi);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "mrb_int _t%d = _t%d + (_t%d - _t%d) / 2;\n", tmid, tlo, thi, tlo);
  if (p0) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = _t%d;\n", p0, tmid); }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
  int save = g_indent; g_indent++;
  Buf cb; memset(&cb, 0, sizeof cb); emit_expr(c, bb[bn - 1], &cb); g_indent = save;
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "if (%s) { _t%d = _t%d; _t%d = _t%d - 1; }\n", cb.p ? cb.p : "0", tres, tmid, thi, tmid); free(cb.p);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d = _t%d + 1; }\n", tlo, tmid);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tres);
  return 1;
}

/* array.max_by / min_by { |x| key } -> the element with the largest/smallest
   (int/float) key. Loop in the statement prelude; value is the best element. */
static int emit_minmax_by_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int is_max = !strcmp(name, "max_by"), is_min = !strcmp(name, "min_by");
  if (!is_max && !is_min) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind et = ty_array_elem(rt);
  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  TyKind bvt = infer_type(c, bb[bn - 1]);
  if (bvt != TY_INT && bvt != TY_FLOAT && bvt != TY_POLY) return 0;
  int trecv = ++g_tmp, tbest = ++g_tmp, tbv = ++g_tmp, tf = ++g_tmp, ti = ++g_tmp, tcur = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);  /* recv value; its own preludes flow to g_pre */
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", trecv); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
  emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tbest, et == TY_RANGE ? "(sp_Range){0}" : default_value(et));
  emit_indent(g_pre, g_indent); emit_ctype(c, bvt, g_pre); buf_printf(g_pre, " _t%d = %s; int _t%d = 1;\n", tbv, default_value(bvt), tf);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, k, trecv, ti);
  if (p0) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti); }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
  Scope *mbsc = p0 ? comp_scope_of(c, block) : NULL;
  LocalVar *mlv0 = (mbsc && p0) ? scope_local(mbsc, p0) : NULL;
  TyKind mpt0 = mlv0 ? mlv0->type : TY_UNKNOWN;
  if (mlv0) mlv0->type = et;
  int save = g_indent; g_indent++;
  Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = save;
  if (mlv0) mlv0->type = mpt0;
  emit_indent(g_pre, g_indent + 1); emit_ctype(c, bvt, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tcur, vb.p ? vb.p : default_value(bvt)); free(vb.p);
  emit_indent(g_pre, g_indent + 1);
  if (bvt == TY_POLY)
    buf_printf(g_pre, "if (_t%d || sp_poly_%s(_t%d, _t%d)) { _t%d = lv_%s; _t%d = _t%d; _t%d = 0; }\n",
               tf, is_max ? "gt" : "lt", tcur, tbv, tbest, p0 ? p0 : "", tbv, tcur, tf);
  else
    buf_printf(g_pre, "if (_t%d || _t%d %s _t%d) { _t%d = lv_%s; _t%d = _t%d; _t%d = 0; }\n",
               tf, tcur, is_max ? ">" : "<", tbv, tbest, p0 ? p0 : "", tbv, tcur, tf);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tbest);
  return 1;
}

/* "str".gsub(/re/) { |m| repl } / sub as an expression: iterate the matches
   of a regex literal, binding the block param to each matched substring and
   appending its return value as the replacement. sub replaces only the first
   match. Anchored patterns (^/$) are matched per-remainder, so this targets
   the unanchored block forms. Returns 1 if handled. */
static int emit_gsub_block_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || (strcmp(name, "gsub") && strcmp(name, "sub"))) return 0;
  int once = !strcmp(name, "sub");
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || comp_ntype(c, recv) != TY_STRING) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (argc != 1) return 0;
  int reidx = re_lit_index(c, argv[0]);
  if (reidx < 0) return 0;
  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  int ts = ++g_tmp, tpos = ++g_tmp, tslen = ++g_tmp, tout = ++g_tmp,
      tm = ++g_tmp, tms = ++g_tmp, tme = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "const char *_t%d = ", ts); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = 0;\n", tpos);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = (mrb_int)strlen(_t%d);\n", tslen, ts);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_String *_t%d = sp_String_new(\"\"); SP_GC_ROOT(_t%d);\n", tout, tout);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "while (_t%d <= _t%d) {\n", tpos, tslen);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "mrb_int _t%d = sp_re_match(sp_re_pat_%d, _t%d + _t%d);\n", tm, reidx, ts, tpos);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "if (_t%d < 0) { sp_String_append(_t%d, _t%d + _t%d); break; }\n", tm, tout, ts, tpos);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "mrb_int _t%d = sp_re_caps[0];\n", tms);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "mrb_int _t%d = sp_re_caps[1];\n", tme);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_String_append(_t%d, sp_str_substr(_t%d + _t%d, 0, _t%d));\n", tout, ts, tpos, tms);
  if (p0) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_str_substr(_t%d + _t%d, _t%d, _t%d - _t%d);\n", p0, ts, tpos, tms, tme, tms); }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
  int save = g_indent; g_indent++;
  Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = save;
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_String_append(_t%d, %s);\n", tout, vb.p ? vb.p : "\"\""); free(vb.p);
  if (once) {
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_String_append(_t%d, _t%d + _t%d + _t%d); break;\n", tout, ts, tpos, tme);
  }
  else {
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "if (_t%d == _t%d) { if (_t%d + _t%d < _t%d) sp_String_append(_t%d, sp_str_substr(_t%d + _t%d, _t%d, 1)); _t%d += _t%d + 1; }\n",
               tme, tms, tpos, tme, tslen, tout, ts, tpos, tme, tpos, tme);
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d += _t%d; }\n", tpos, tme);
  }
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d->data", tout);
  return 1;
}

/* array.sum([init]) { |x| f(x) } as an expression: sum the block's result
   over every element. Returns 1 if handled. */
static int emit_sum_block_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || strcmp(name, "sum")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  TyKind acct = comp_ntype(c, bb[bn - 1]);
  if (acct != TY_INT && acct != TY_FLOAT) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int ta = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp;
  buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
  buf_printf(b, "; mrb_int _t%d = sp_%sArray_length(_t%d); ", tn, k, ta);
  emit_ctype(c, acct, b); buf_printf(b, " _t%d = ", tacc);
  if (argc == 1) {
    TyKind init_t = comp_ntype(c, argv[0]);
    if (acct == TY_FLOAT && init_t == TY_INT) {
      buf_puts(b, "(mrb_float)("); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (acct == TY_FLOAT && init_t == TY_POLY) {
      buf_puts(b, "sp_poly_to_f("); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (acct == TY_INT && init_t == TY_POLY) {
      buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else {
      emit_expr(c, argv[0], b);
    }
  }
  else {
    buf_puts(b, acct == TY_FLOAT ? "0.0" : "0");
  }
  buf_printf(b, "; for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) { ", ti, ti, tn, ti);
  if (p0) buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d); ", p0, k, ta, ti);
  buf_printf(b, "_t%d = ", tacc);
  if (acct == TY_INT) { buf_printf(b, "sp_int_add(_t%d, ", tacc); emit_expr(c, bb[bn - 1], b); buf_puts(b, ")"); }
  else { buf_printf(b, "_t%d + ", tacc); emit_expr(c, bb[bn - 1], b); }
  buf_printf(b, "; } _t%d; })", tacc);
  return 1;
}

/* int_array.slice_when { |a, b| cond }[.to_a].inspect  or
   int_array.chunk { |x| key }[.to_a].inspect  ->  inspect string.
   Emits setup to g_pre and the result variable to b. Returns 1 if handled. */
static int emit_slice_when_chunk_inspect_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || strcmp(name, "inspect")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  /* allow .to_a wrapper */
  if (nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "to_a"))
    recv = nt_ref(nt, recv, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || strcmp(nt_type(nt, recv), "CallNode")) return 0;
  const char *m = nt_str(nt, recv, "name");
  if (!m) return 0;
  int is_sw = !strcmp(m, "slice_when");
  int is_ck = !strcmp(m, "chunk");
  if (!is_sw && !is_ck) return 0;
  int block = nt_ref(nt, recv, "block");
  if (block < 0) return 0;
  int pr = nt_ref(nt, recv, "receiver");
  if (pr < 0 || comp_ntype(c, pr) != TY_INT_ARRAY) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  const char *p0n = block_param_name(c, block, 0);
  if (!p0n) return 0;
  const char *p0 = rename_local(p0n);

  if (is_sw) {
    /* slice_when { |a, b| cond } */
    const char *p1n = block_param_name(c, block, 1);
    if (!p1n) return 0;
    const char *p1 = rename_local(p1n);
    int ta = ++g_tmp, tout = ++g_tmp, tcur = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, pr, &rb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_IntArray *_t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PtrArray *_t%d = sp_PtrArray_new(); SP_GC_ROOT(_t%d);\n", tout, tout);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);\n", tcur, tcur);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++) {\n", ti, ti, ta, ti);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "lv_%s = sp_IntArray_get(_t%d, _t%d);\n", p0, ta, ti);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_IntArray_push(_t%d, lv_%s);\n", tcur, p0);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "if (_t%d + 1 < sp_IntArray_length(_t%d)) {\n", ti, ta);
    emit_indent(g_pre, g_indent + 2);
    buf_printf(g_pre, "lv_%s = sp_IntArray_get(_t%d, _t%d + 1);\n", p1, ta, ti);
    /* emit block body condition */
    Scope *bsc = comp_scope_of(c, block);
    LocalVar *lva = bsc ? scope_local(bsc, p0n) : NULL;
    LocalVar *lvb = bsc ? scope_local(bsc, p1n) : NULL;
    TyKind pta = lva ? lva->type : TY_UNKNOWN, ptb = lvb ? lvb->type : TY_UNKNOWN;
    if (lva) lva->type = TY_INT;
    if (lvb) lvb->type = TY_INT;
    for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 2);
    int save = g_indent; g_indent += 2;
    Buf cb; memset(&cb, 0, sizeof cb); emit_expr(c, bb[bn - 1], &cb); g_indent = save;
    if (lva) lva->type = pta;
    if (lvb) lvb->type = ptb;
    emit_indent(g_pre, g_indent + 2);
    buf_printf(g_pre, "if (%s) {\n", cb.p ? cb.p : "0"); free(cb.p);
    emit_indent(g_pre, g_indent + 3);
    buf_printf(g_pre, "sp_PtrArray_push(_t%d, _t%d);\n", tout, tcur);
    emit_indent(g_pre, g_indent + 3);
    buf_printf(g_pre, "_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);\n", tcur, tcur);
    emit_indent(g_pre, g_indent + 2); buf_puts(g_pre, "}\n");
    emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "if (sp_IntArray_length(_t%d) > 0) sp_PtrArray_push(_t%d, _t%d);\n", tcur, tout, tcur);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "const char *_t%d = sp_IntArrayPtrArray_inspect(_t%d);\n", tres, tout);
    buf_printf(b, "_t%d", tres);
    return 1;
  }

  /* chunk { |x| key_expr } -- group consecutive elements by key */
  int ta = ++g_tmp, tkeys = ++g_tmp, tgrps = ++g_tmp, tcur = ++g_tmp;
  int tpk = ++g_tmp, ti = ++g_tmp, tstr = ++g_tmp, tj = ++g_tmp, tres = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, pr, &rb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_IntArray *_t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);\n", tkeys, tkeys);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PtrArray *_t%d = sp_PtrArray_new(); SP_GC_ROOT(_t%d);\n", tgrps, tgrps);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_IntArray *_t%d = NULL;\n", tcur);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "mrb_int _t%d = 0;\n", tpk);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++) {\n", ti, ti, ta, ti);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "lv_%s = sp_IntArray_get(_t%d, _t%d);\n", p0, ta, ti);
  /* emit key expression */
  Scope *bsc = comp_scope_of(c, block);
  LocalVar *lv0 = bsc ? scope_local(bsc, p0n) : NULL;
  TyKind pt0 = lv0 ? lv0->type : TY_UNKNOWN;
  if (lv0) lv0->type = TY_INT;
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
  int save = g_indent; g_indent++;
  Buf kb; memset(&kb, 0, sizeof kb); emit_expr(c, bb[bn - 1], &kb); g_indent = save;
  if (lv0) lv0->type = pt0;
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "mrb_int _tkey_%d = %s;\n", ta, kb.p ? kb.p : "0"); free(kb.p);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "if (_t%d == 0 || _tkey_%d != _t%d) {\n", ti, ta, tpk);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);\n", tcur, tcur);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "sp_IntArray_push(_t%d, _tkey_%d);\n", tkeys, ta);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "sp_PtrArray_push(_t%d, _t%d);\n", tgrps, tcur);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "_t%d = _tkey_%d;\n", tpk, ta);
  emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_IntArray_push(_t%d, lv_%s);\n", tcur, p0);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  /* build inspect string */
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_String *_t%d = sp_String_new(\"[\"); SP_GC_ROOT(_t%d);\n", tstr, tstr);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", tj, tj, tkeys, tj);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "if (_t%d > 0) sp_String_append(_t%d, \", \");\n", tj, tstr);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_String_append(_t%d, \"[\");\n", tstr);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_String_append(_t%d, sp_int_to_s(sp_IntArray_get(_t%d, _t%d)));\n", tstr, tkeys, tj);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_String_append(_t%d, \", \");\n", tstr);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_String_append(_t%d, sp_IntArray_inspect((sp_IntArray*)_t%d->data[_t%d]));\n", tstr, tgrps, tj);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_String_append(_t%d, \"]\");\n", tstr);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_String_append(_t%d, \"]\");\n", tstr);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "const char *_t%d = _t%d->data;\n", tres, tstr);
  buf_printf(b, "_t%d", tres);
  return 1;
}

/* int_array.product(int_array)[.to_a].inspect -> the Cartesian product
   rendered as a nested-array string. The product result has no first-class
   type, so only this inline inspect chain is supported. Returns 1 if handled. */
static int emit_product_inspect_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || strcmp(name, "inspect")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  /* allow an intervening .to_a */
  if (nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "to_a"))
    recv = nt_ref(nt, recv, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || strcmp(nt_type(nt, recv), "CallNode")) return 0;
  const char *m = nt_str(nt, recv, "name");
  if (!m) return 0;
  int is_product = !strcmp(m, "product");
  int is_slice = !strcmp(m, "slice_before") || !strcmp(m, "slice_after");
  if (!is_product && !is_slice) return 0;
  int pr = nt_ref(nt, recv, "receiver");
  int pargs = nt_ref(nt, recv, "arguments");
  int pac = 0; const int *pav = pargs >= 0 ? nt_arr(nt, pargs, "arguments", &pac) : NULL;
  if (pr < 0 || pac != 1) return 0;
  if (comp_ntype(c, pr) != TY_INT_ARRAY) return 0;
  if (is_product) {
    /* the other operand is an int_array or an empty array literal */
    TyKind at = comp_ntype(c, pav[0]);
    int empty_lit = nt_type(nt, pav[0]) && !strcmp(nt_type(nt, pav[0]), "ArrayNode") &&
                    ({ int en = 0; nt_arr(nt, pav[0], "elements", &en); en == 0; });
    if (at != TY_INT_ARRAY && !empty_lit) return 0;
    buf_puts(b, "sp_IntArrayPtrArray_inspect(sp_IntArray_product(");
    emit_expr(c, pr, b); buf_puts(b, ", ");
    if (empty_lit) buf_puts(b, "sp_IntArray_new()"); else emit_expr(c, pav[0], b);
    buf_puts(b, "))");
    return 1;
  }
  /* slice_before / slice_after with an int delimiter */
  if (comp_ntype(c, pav[0]) != TY_INT) return 0;
  buf_printf(b, "sp_IntArrayPtrArray_inspect(sp_IntArray_%s(", m);
  emit_expr(c, pr, b); buf_puts(b, ", "); emit_expr(c, pav[0], b); buf_puts(b, "))");
  return 1;
}

/* numeric.step(limit[, step]) without a block, materialized as an int or
   float array (so a following .to_a / .inspect works). Returns 1 if handled. */
static int emit_step_array_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  if (nt_ref(nt, id, "block") >= 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || strcmp(name, "step")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (rt != TY_INT && rt != TY_FLOAT) return 0;
  int args = nt_ref(nt, id, "arguments");
  int sc = 0; const int *sv = args >= 0 ? nt_arr(nt, args, "arguments", &sc) : NULL;
  if (sc < 1) return 0;
  int is_float = (rt == TY_FLOAT) || comp_ntype(c, sv[0]) == TY_FLOAT ||
                 (sc >= 2 && comp_ntype(c, sv[1]) == TY_FLOAT);
  int tr = ++g_tmp, tl = ++g_tmp, ts = ++g_tmp, ti = ++g_tmp;
  if (!is_float) {
    buf_printf(b, "({ sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d); mrb_int _t%d = ", tr, tr, tl);
    emit_expr(c, sv[0], b); buf_printf(b, "; mrb_int _t%d = ", ts);
    if (sc >= 2) emit_expr(c, sv[1], b); else buf_puts(b, "1");
    buf_printf(b, "; for (mrb_int _t%d = ", ti); emit_expr(c, recv, b);
    buf_printf(b, "; _t%d >= 0 ? _t%d <= _t%d : _t%d >= _t%d; _t%d += _t%d) sp_IntArray_push(_t%d, _t%d); _t%d; })",
               ts, ti, tl, ti, tl, ti, ts, tr, ti, tr);
    return 1;
  }
  int tb = ++g_tmp, tn = ++g_tmp;
  buf_printf(b, "({ sp_FloatArray *_t%d = sp_FloatArray_new(); SP_GC_ROOT(_t%d); mrb_float _t%d = ", tr, tr, tb);
  emit_expr(c, recv, b); buf_printf(b, "; mrb_float _t%d = ", tl); emit_expr(c, sv[0], b);
  buf_printf(b, "; mrb_float _t%d = ", ts);
  if (sc >= 2) emit_expr(c, sv[1], b); else buf_puts(b, "1.0");
  buf_printf(b, "; mrb_float _t%d_e = (fabs(_t%d)+fabs(_t%d)+fabs(_t%d-_t%d))/fabs(_t%d)*DBL_EPSILON;"
                " if (_t%d_e > 0.5) _t%d_e = 0.5;"
                " mrb_int _t%d = (mrb_int)floor((_t%d-_t%d)/_t%d + _t%d_e);"
                " for (mrb_int _t%d = 0; _t%d <= _t%d; _t%d++) sp_FloatArray_push(_t%d, _t%d + _t%d * _t%d); _t%d; })",
             tn, tb, tl, tl, tb, ts, tn, tn, tn, tl, tb, ts, tn, ti, ti, tn, ti, tr, tb, ti, ts, tr);
  return 1;
}

/* inject(:op) / reduce(:op) / inject(&:op) / inject(init, :op) as an
   expression: fold the array with a symbol-named arithmetic operator. The
   block-fold form (inject { |a, e| ... }) is not handled here. Returns 1 if
   handled. */
static int emit_inject_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || (strcmp(name, "inject") && strcmp(name, "reduce"))) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  /* empty array literal `[]` has TY_UNKNOWN; treat as TY_INT_ARRAY */
  if (rt == TY_UNKNOWN && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ArrayNode")) {
    int en = 0; nt_arr(nt, recv, "elements", &en);
    if (en == 0) rt = TY_INT_ARRAY;
  }
  if (!ty_is_array(rt)) return 0;
  const char *k = array_kind(rt);
  if (!k) return 0;
  TyKind et = ty_array_elem(rt);

  /* find the operator symbol (from a &:op block or a trailing :op arg) and
     any explicit initial value */
  const char *op = NULL; int init = -1;
  int block = nt_ref(nt, id, "block");
  if (block >= 0 && nt_type(nt, block) && !strcmp(nt_type(nt, block), "BlockArgumentNode")) {
    int ex = nt_ref(nt, block, "expression");
    if (ex >= 0 && nt_type(nt, ex) && !strcmp(nt_type(nt, ex), "SymbolNode")) op = nt_str(nt, ex, "value");
  }
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (!op && argc >= 1) {
    int last = argv[argc - 1];
    if (nt_type(nt, last) && !strcmp(nt_type(nt, last), "SymbolNode")) {
      op = nt_str(nt, last, "value");
      if (argc == 2) init = argv[0];
    }
  }
  if (!op) return 0;

  const char *ifn = (et == TY_INT) ? int_arith_fn(op) : NULL;
  /* bitwise ops on integers: &, |, ^, <<, >> -- use operator directly */
  int int_bitop = (et == TY_INT) && !ifn &&
                  (!strcmp(op, "&") || !strcmp(op, "|") || !strcmp(op, "^") ||
                   !strcmp(op, "<<") || !strcmp(op, ">>"));
  int float_op = (et == TY_FLOAT) && (!strcmp(op, "+") || !strcmp(op, "-") ||
                                      !strcmp(op, "*") || !strcmp(op, "/"));
  int str_op = (et == TY_STRING) && !strcmp(op, "+");
  if (!ifn && !int_bitop && !float_op && !str_op) return 0;

  int ta = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp;
  buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
  buf_printf(b, "; mrb_int _t%d = sp_%sArray_length(_t%d); ", tn, k, ta);
  emit_ctype(c, et, b); buf_printf(b, " _t%d = ", tacc);
  int start;
  if (init >= 0) { emit_expr(c, init, b); start = 0; }
  else { buf_printf(b, "_t%d > 0 ? sp_%sArray_get(_t%d, 0) : %s", tn, k, ta, default_value(et)); start = 1; }
  buf_printf(b, "; for (mrb_int _t%d = %d; _t%d < _t%d; _t%d++) _t%d = ", ti, start, ti, tn, ti, tacc);
  if (ifn)
    buf_printf(b, "%s(_t%d, sp_%sArray_get(_t%d, _t%d))", ifn, tacc, k, ta, ti);
  else if (str_op)
    buf_printf(b, "sp_str_concat(_t%d, sp_%sArray_get(_t%d, _t%d))", tacc, k, ta, ti);
  else /* int_bitop or float direct-op */
    buf_printf(b, "_t%d %s sp_%sArray_get(_t%d, _t%d)", tacc, op, k, ta, ti);
  buf_printf(b, "; _t%d; })", tacc);
  return 1;
}

/* reduce/inject with a block { |acc, elem| body } as an expression.
   Handles typed (non-poly) arrays where both params are scalar. */
static int emit_reduce_block_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || (strcmp(name, "inject") && strcmp(name, "reduce"))) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = array_kind(rt);
  if (!k) return 0;
  TyKind et = ty_array_elem(rt);
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *bty = nt_type(nt, block);
  if (!bty || strcmp(bty, "BlockNode")) return 0;
  const char *p0_orig = block_param_name(c, block, 0);
  const char *p1_orig = block_param_name(c, block, 1);
  if (!p0_orig || !p1_orig) return 0;
  const char *p0 = rename_local(p0_orig);
  const char *p1 = rename_local(p1_orig);
  int bbody = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
  if (bn == 0) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int init = (argc > 0 && argv) ? argv[0] : -1;

  /* Accumulator type comes from the seed init when provided, else from the element type. */
  TyKind acc_ty = et;
  if (init >= 0) {
    TyKind it = comp_ntype(c, init);
    if (it != TY_UNKNOWN) acc_ty = it;
  }
  int ta = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp;
  buf_puts(b, "({ ");
  emit_ctype(c, rt, b); buf_printf(b, " _t%d = ", ta); emit_expr(c, recv, b); buf_puts(b, "; ");
  emit_ctype(c, acc_ty, b); buf_printf(b, " _t%d = ", tacc);
  int start;
  if (init >= 0) { emit_expr(c, init, b); buf_puts(b, "; "); start = 0; }
  else { buf_printf(b, "sp_%sArray_length(_t%d) > 0 ? sp_%sArray_get(_t%d, 0) : 0; ", k, ta, k, ta); start = 1; }
  /* Temporarily override block param types to match acc_ty/et so the body
     expression uses the correct C types (same pattern as emit_sort_cmp_expr). */
  Scope *rsc = comp_scope_of(c, block);
  LocalVar *rlv0 = rsc ? scope_local(rsc, p0_orig) : NULL;
  LocalVar *rlv1 = rsc ? scope_local(rsc, p1_orig) : NULL;
  TyKind rpt0 = rlv0 ? rlv0->type : TY_UNKNOWN;
  TyKind rpt1 = rlv1 ? rlv1->type : TY_UNKNOWN;
  if (rlv0) rlv0->type = acc_ty;
  if (rlv1) rlv1->type = et;
  for (int j = 0; j < bn; j++) infer_type(c, bb[j]);  /* refresh ntype cache */
  buf_printf(b, "for (mrb_int _t%d = %d; _t%d < sp_%sArray_length(_t%d); _t%d++) { ",
             ti, start, ti, k, ta, ti);
  buf_puts(b, "{ ");
  emit_ctype(c, acc_ty, b); buf_printf(b, " lv_%s = _t%d; ", p0, tacc);
  emit_ctype(c, et, b); buf_printf(b, " lv_%s = sp_%sArray_get(_t%d, _t%d); ", p1, k, ta, ti);
  for (int j = 0; j < bn - 1; j++) {
    emit_stmt(c, bb[j], b, 0);
    buf_puts(b, " ");
  }
  buf_printf(b, "_t%d = ", tacc); emit_expr(c, bb[bn - 1], b); buf_puts(b, "; } } ");
  buf_printf(b, "_t%d; })", tacc);
  if (rlv0) rlv0->type = rpt0;
  if (rlv1) rlv1->type = rpt1;
  return 1;
}

/* sort_by { |x| key } as an expression: a stable bubble sort of a copy of
   the receiver, ordering by the block's computed (scalar) key. Returns 1 if
   handled. */
static int emit_sortby_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (strcmp(name, "sort_by")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind et = ty_array_elem(rt);
  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  TyKind kt = comp_ntype(c, bb[bn - 1]);
  if (kt != TY_INT && kt != TY_FLOAT && kt != TY_STRING && kt != TY_POLY) return 0;  /* scalar or poly key */
  int trv = ++g_tmp, tr = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp, ta = ++g_tmp, tb = ++g_tmp, tka = ++g_tmp, tkb = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", trv);
  buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
  /* copy so the receiver is left unsorted (sort_by is non-mutating) */
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = sp_%sArray_slice(_t%d, 0, sp_%sArray_length(_t%d));\n", tr, k, trv, k, trv);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tr);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = sp_%sArray_length(_t%d);\n", tn, k, tr);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d - 1; _t%d++)\n", ti, ti, tn, ti);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d - 1 - _t%d; _t%d++) {\n", tj, tj, tn, ti, tj);
  emit_indent(g_pre, g_indent + 2); emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = sp_%sArray_get(_t%d, _t%d);\n", ta, k, tr, tj);
  emit_indent(g_pre, g_indent + 2); emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = sp_%sArray_get(_t%d, _t%d + 1);\n", tb, k, tr, tj);
  int save = g_indent; g_indent += 2;
  /* key of _ta */
  if (p0) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "lv_%s = _t%d;\n", p0, ta); }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent);
  Buf ka; memset(&ka, 0, sizeof ka); emit_expr(c, bb[bn - 1], &ka);
  emit_indent(g_pre, g_indent); emit_ctype(c, kt, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tka, ka.p ? ka.p : "0"); free(ka.p);
  /* key of _tb */
  if (p0) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "lv_%s = _t%d;\n", p0, tb); }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent);
  Buf kb; memset(&kb, 0, sizeof kb); emit_expr(c, bb[bn - 1], &kb);
  emit_indent(g_pre, g_indent); emit_ctype(c, kt, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tkb, kb.p ? kb.p : "0"); free(kb.p);
  g_indent = save;
  emit_indent(g_pre, g_indent + 2);
  if (kt == TY_STRING) buf_printf(g_pre, "if (strcmp(_t%d, _t%d) > 0) {", tka, tkb);
  else if (kt == TY_POLY) { int tcmp = ++g_tmp; buf_printf(g_pre, "{ mrb_bool _tcmp%d = 0; if (sp_poly_cmp(_t%d, _t%d, &_tcmp%d) > 0 && _tcmp%d) {", tcmp, tka, tkb, tcmp, tcmp); }
  else buf_printf(g_pre, "if (_t%d > _t%d) {", tka, tkb);
  buf_printf(g_pre, " sp_%sArray_set(_t%d, _t%d, _t%d); sp_%sArray_set(_t%d, _t%d + 1, _t%d); }%s\n",
             k, tr, tj, tb, k, tr, tj, ta, kt == TY_POLY ? " }" : "");
  emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tr);
  return 1;
}

/* sort { |a, b| a <=> b } as an expression: stable bubble sort of a copy,
   ordered by the comparator block (which yields the <=> sign). Returns 1 if
   handled. */
static int emit_sort_cmp_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int is_bang = !strcmp(name, "sort!");
  if (strcmp(name, "sort") && !is_bang) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind et = ty_array_elem(rt);
  const char *p0 = block_param_name(c, block, 0);
  const char *p1 = block_param_name(c, block, 1);
  if (!p0 || !p1) return 0;
  p0 = rename_local(p0); p1 = rename_local(p1);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1 || infer_type(c, bb[bn - 1]) != TY_INT) return 0;
  int trv = ++g_tmp, tr = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp, ta = ++g_tmp, tb = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", trv); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
  if (!is_bang) {
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = sp_%sArray_slice(_t%d, 0, sp_%sArray_length(_t%d));\n", tr, k, trv, k, trv);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tr);
  } else {
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = _t%d;\n", tr, trv);  /* sort! operates on self */
  }
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = sp_%sArray_length(_t%d);\n", tn, k, tr);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d - 1; _t%d++)\n", ti, ti, tn, ti);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d - 1 - _t%d; _t%d++) {\n", tj, tj, tn, ti, tj);
  emit_indent(g_pre, g_indent + 2); emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = sp_%sArray_get(_t%d, _t%d);\n", ta, k, tr, tj);
  emit_indent(g_pre, g_indent + 2); emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = sp_%sArray_get(_t%d, _t%d + 1);\n", tb, k, tr, tj);
  Scope *sbsc = comp_scope_of(c, block);
  LocalVar *slv0 = sbsc ? scope_local(sbsc, p0) : NULL;
  LocalVar *slv1 = sbsc ? scope_local(sbsc, p1) : NULL;
  TyKind spt0 = slv0 ? slv0->type : TY_UNKNOWN;
  TyKind spt1 = slv1 ? slv1->type : TY_UNKNOWN;
  if (slv0) slv0->type = et;
  if (slv1) slv1->type = et;
  for (int j = 0; j < bn; j++) infer_type(c, bb[j]);  /* refresh ntype cache */
  int save = g_indent; g_indent += 2;
  /* Shadow the outer (possibly poly) block params with et-typed locals */
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "{\n"); g_indent++;
  emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = _t%d; ", p0, ta);
  emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = _t%d;\n", p1, tb);
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent);
  Buf cb; memset(&cb, 0, sizeof cb); emit_expr(c, bb[bn - 1], &cb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "if ((%s) > 0) { sp_%sArray_set(_t%d, _t%d, _t%d); sp_%sArray_set(_t%d, _t%d + 1, _t%d); }\n",
             cb.p ? cb.p : "0", k, tr, tj, tb, k, tr, tj, ta); free(cb.p);
  g_indent--; g_indent = save;
  emit_indent(g_pre, g_indent + 2); buf_puts(g_pre, "}\n");
  if (slv0) slv0->type = spt0;
  if (slv1) slv1->type = spt1;
  emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tr);
  return 1;
}

/* Emit "lv_<nm> = _t<tidx>" with boxing if the outer local is TY_POLY
   but the element type et is scalar (string, int, float, bool). */
static void emit_block_param_assign(Compiler *c, int scope_id, const char *nm, int tidx, TyKind et, Buf *b) {
  Scope *sc = comp_scope_of(c, scope_id);
  LocalVar *lv = sc ? scope_local(sc, nm) : NULL;
  int box = lv && lv->type == TY_POLY && et != TY_POLY;
  if (box) {
    if (et == TY_INT)    buf_printf(b, "lv_%s = sp_box_int(_t%d);", nm, tidx);
    else if (et == TY_STRING) buf_printf(b, "lv_%s = sp_box_str(_t%d);", nm, tidx);
    else if (et == TY_FLOAT)  buf_printf(b, "lv_%s = sp_box_float(_t%d);", nm, tidx);
    else if (et == TY_BOOL)   buf_printf(b, "lv_%s = sp_box_bool(_t%d);", nm, tidx);
    else buf_printf(b, "lv_%s = _t%d;", nm, tidx);
  } else {
    buf_printf(b, "lv_%s = _t%d;", nm, tidx);
  }
}

/* min / max / minmax { |a, b| a <=> b } as an expression: a single scan
   tracking the extreme(s) under the comparator block. min/max yield one
   element; minmax yields a fresh [min, max]. Returns 1 if handled. */
static int emit_minmax_cmp_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int is_min = !strcmp(name, "min"), is_max = !strcmp(name, "max"), is_mm = !strcmp(name, "minmax");
  if (!is_min && !is_max && !is_mm) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = infer_type(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind et = ty_array_elem(rt);
  const char *p0 = block_param_name(c, block, 0);
  const char *p1 = block_param_name(c, block, 1);
  if (!p0 || !p1) return 0;
  p0 = rename_local(p0); p1 = rename_local(p1);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1 || infer_type(c, bb[bn - 1]) != TY_INT) return 0;
  int trv = ++g_tmp, tn = ++g_tmp, tmin = ++g_tmp, tmax = ++g_tmp, ti = ++g_tmp, te = ++g_tmp, tres = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", trv); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = sp_%sArray_length(_t%d);\n", tn, k, trv);
  emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre);
  buf_printf(g_pre, " _t%d = _t%d > 0 ? sp_%sArray_get(_t%d, 0) : %s;\n", tmin, tn, k, trv, et == TY_RANGE ? "(sp_Range){0}" : default_value(et));
  emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = _t%d;\n", tmax, tmin);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 1; _t%d < _t%d; _t%d++) {\n", ti, ti, tn, ti);
  emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = sp_%sArray_get(_t%d, _t%d);\n", te, k, trv, ti);
  /* Block params may be widened to TY_POLY across multiple block sites.
     Pin them to the element type for body emission:
     - temporarily set scope types to `et`
     - refresh ntype cache for body nodes (infer_type writes to cache)
     - emit C shadow declarations inside { } to give lv_p0/lv_p1 the right C type */
  Scope *bsc = comp_scope_of(c, block);
  LocalVar *lv_p0 = bsc ? scope_local(bsc, p0) : NULL;
  LocalVar *lv_p1 = bsc ? scope_local(bsc, p1) : NULL;
  TyKind saved_p0 = lv_p0 ? lv_p0->type : TY_UNKNOWN;
  TyKind saved_p1 = lv_p1 ? lv_p1->type : TY_UNKNOWN;
  if (lv_p0) lv_p0->type = et;
  if (lv_p1) lv_p1->type = et;
  for (int j = 0; j < bn; j++) infer_type(c, bb[j]);  /* refresh cache */
  int save = g_indent; g_indent++;
  if (is_min || is_mm) {
    /* Open C shadow scope with et-typed block param vars */
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "{\n"); g_indent++;
    emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = _t%d; ", p0, te);
    emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = _t%d;\n", p1, tmin);
    for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent);
    Buf cm; memset(&cm, 0, sizeof cm); emit_expr(c, bb[bn - 1], &cm);
    g_indent--;
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "if ((%s) < 0) _t%d = _t%d;\n", cm.p ? cm.p : "0", tmin, te); free(cm.p);
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  }
  if (is_max || is_mm) {
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "{\n"); g_indent++;
    emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = _t%d; ", p0, te);
    emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = _t%d;\n", p1, tmax);
    for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent);
    Buf cx; memset(&cx, 0, sizeof cx); emit_expr(c, bb[bn - 1], &cx);
    g_indent--;
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "if ((%s) > 0) _t%d = _t%d;\n", cx.p ? cx.p : "0", tmax, te); free(cx.p);
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  }
  if (lv_p0) lv_p0->type = saved_p0;
  if (lv_p1) lv_p1->type = saved_p1;
  g_indent = save;
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  if (is_min) { buf_printf(b, "_t%d", tmin); return 1; }
  if (is_max) { buf_printf(b, "_t%d", tmax); return 1; }
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = sp_%sArray_new();\n", tres, k);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "if (_t%d > 0) { sp_%sArray_push(_t%d, _t%d); sp_%sArray_push(_t%d, _t%d); }\n", tn, k, tres, tmin, k, tres, tmax);
  buf_printf(b, "_t%d", tres);
  return 1;
}

/* partition { |x| ... } as an expression: emits two typed result arrays
   pushed into a PolyArray. Returns 1 if handled. */
static int emit_partition_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || strcmp(name, "partition")) return 0;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = infer_type(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;

  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0;
  const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;

  TyKind et = ty_array_elem(rt);
  Scope *psc = p0 ? comp_scope_of(c, block) : NULL;
  LocalVar *plv0 = (psc && p0) ? scope_local(psc, p0) : NULL;
  TyKind psaved0 = plv0 ? plv0->type : TY_UNKNOWN;
  int use_shadow = plv0 && plv0->type != et && et != TY_UNKNOWN;
  if (use_shadow) {
    plv0->type = et;
    for (int j = 0; j < bn; j++) infer_type(c, bb[j]);
  }

  int trecv = ++g_tmp, ttrue = ++g_tmp, tfalse = ++g_tmp, ti = ++g_tmp;

  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);

  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", k, ttrue, k);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", ttrue);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", k, tfalse, k);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tfalse);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
             ti, ti, k, trecv, ti);

  int bodyIndent = g_indent + 1;
  int innerIndent = use_shadow ? bodyIndent + 1 : bodyIndent;
  if (use_shadow) {
    emit_indent(g_pre, bodyIndent); buf_puts(g_pre, "{\n");
    emit_indent(g_pre, innerIndent); emit_ctype(c, et, g_pre);
    buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
  }
  else if (p0) {
    emit_indent(g_pre, bodyIndent);
    buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
  }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, innerIndent);
  int saveIndent = g_indent; g_indent = innerIndent;
  Buf vb; memset(&vb, 0, sizeof vb);
  emit_expr(c, bb[bn - 1], &vb);
  g_indent = saveIndent;

  emit_indent(g_pre, innerIndent);
  buf_printf(g_pre, "if (%s) sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d));\n",
             vb.p ? vb.p : "0", k, ttrue, k, trecv, ti);
  emit_indent(g_pre, innerIndent);
  buf_printf(g_pre, "else sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d));\n",
             k, tfalse, k, trecv, ti);
  free(vb.p);

  if (use_shadow) { emit_indent(g_pre, bodyIndent); buf_puts(g_pre, "}\n"); }
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  if (use_shadow && plv0) plv0->type = psaved0;

  int tres = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new();\n", tres);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres);

  const char *box_fn = (rt == TY_INT_ARRAY) ? "sp_box_int_array"
                     : (rt == TY_STR_ARRAY) ? "sp_box_str_array"
                     : (rt == TY_FLOAT_ARRAY) ? "sp_box_float_array"
                     : "sp_box_poly_array";
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s(_t%d));\n", tres, box_fn, ttrue);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s(_t%d));\n", tres, box_fn, tfalse);

  buf_printf(b, "_t%d", tres);
  return 1;
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
  if (ty_is_hash(rt)) return emit_hash_collect_expr(c, id, b);
  int range_recv = (rt == TY_RANGE);
  if (!ty_is_array(rt) && !range_recv) return 0;
  const char *k = range_recv ? "Int" : (rt == TY_POLY_ARRAY ? "Poly" : array_kind(rt));
  if (!k) return 0;

  int is_map = !strcmp(name, "map") || !strcmp(name, "collect");
  int is_sel = !strcmp(name, "select") || !strcmp(name, "filter");
  int is_rej = !strcmp(name, "reject");
  if (!is_map && !is_sel && !is_rej) return 0;

  TyKind restype = comp_ntype(c, id);
  int res_poly = (restype == TY_POLY_ARRAY);
  const char *rk = res_poly ? "Poly" : array_kind(restype);
  if (!rk) return 0;

  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0;
  const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;

  /* map {} with empty block: poly array of nil with same length as receiver */
  if (bn == 0 && is_map) {
    int tlen = ++g_tmp, tres0 = ++g_tmp, ti0 = ++g_tmp;
    Buf rb0; memset(&rb0, 0, sizeof rb0);
    emit_expr(c, recv, &rb0);  /* preludes land in g_pre, value in rb0 */
    emit_indent(g_pre, g_indent);
    if (range_recv) {
      int tr = ++g_tmp;
      buf_printf(g_pre, "sp_Range _t%d = %s;\n", tr, rb0.p ? rb0.p : "");
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "mrb_int _t%d = _t%d.last - _t%d.excl - _t%d.first + 1; if (_t%d < 0) _t%d = 0;\n",
                 tlen, tr, tr, tr, tlen, tlen);
    }
    else {
      buf_printf(g_pre, "mrb_int _t%d = sp_%sArray_length(%s);\n", tlen, k, rb0.p ? rb0.p : "NULL");
    }
    free(rb0.p);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres0, tres0);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) sp_PolyArray_push(_t%d, sp_box_nil());\n",
               ti0, ti0, tlen, ti0, tres0);
    buf_printf(b, "_t%d", tres0);
    return 1;
  }

  if (bn < 1) return 0;

  int trecv = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;

  /* eval receiver once (its own preludes must land before the decl line);
     a range receiver is materialized to an int array first */
  Buf rb; memset(&rb, 0, sizeof rb);
  if (range_recv) {
    int tr = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_Range _t%d = ", tr); emit_expr(c, recv, g_pre); buf_puts(g_pre, ";\n");
    buf_printf(&rb, "sp_IntArray_from_range(_t%d.first, _t%d.last - _t%d.excl)", tr, tr, tr);
    rt = TY_INT_ARRAY;
  }
  else emit_expr(c, recv, &rb);
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

  TyKind et_elem = ty_array_elem(rt);
  /* If the block param's scope type was widened (e.g. TY_POLY), pin it to
     the element type and use a C shadow declaration so body emission sees the
     right type. */
  Scope *csc = p0 ? comp_scope_of(c, block) : NULL;
  LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
  TyKind csaved0 = clv0 ? clv0->type : TY_UNKNOWN;
  int use_shadow = clv0 && clv0->type != et_elem && et_elem != TY_UNKNOWN;
  if (use_shadow) {
    clv0->type = et_elem;
    for (int j = 0; j < bn; j++) infer_type(c, bb[j]);
  }

  int bodyIndent = g_indent + 1;
  int innerIndent = use_shadow ? bodyIndent + 1 : bodyIndent;
  if (use_shadow) {
    emit_indent(g_pre, bodyIndent); buf_puts(g_pre, "{\n");
    emit_indent(g_pre, innerIndent); emit_ctype(c, et_elem, g_pre);
    buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
  } else if (p0) {
    emit_indent(g_pre, bodyIndent);
    buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
  }
  /* body statements except the last */
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, innerIndent);

  int saveIndent = g_indent;
  g_indent = innerIndent;
  Buf vb; memset(&vb, 0, sizeof vb);
  emit_expr(c, bb[bn - 1], &vb);  /* value preludes flow to g_pre at innerIndent */
  g_indent = saveIndent;

  TyKind body_ty = comp_ntype(c, bb[bn - 1]);
  if (is_map) {
    emit_indent(g_pre, innerIndent);
    buf_printf(g_pre, "sp_%sArray_push(_t%d, ", rk, tres);
    /* a poly result array stores boxed values */
    if (res_poly && body_ty != TY_POLY) {
      Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, body_ty, vb.p ? vb.p : "", &bx);
      buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p);
    }
    else buf_puts(g_pre, vb.p ? vb.p : "");
    buf_puts(g_pre, ");\n");
  }
  else {
    emit_indent(g_pre, innerIndent);
    buf_printf(g_pre, "if (%s(", is_rej ? "!" : "");
    buf_puts(g_pre, vb.p ? vb.p : ""); buf_puts(g_pre, ")) ");
    buf_printf(g_pre, "sp_%sArray_push(_t%d, lv_%s);\n", rk, tres, p0 ? p0 : "");
  }
  free(vb.p);
  if (use_shadow) { emit_indent(g_pre, bodyIndent); buf_puts(g_pre, "}\n"); }
  if (use_shadow && clv0) clv0->type = csaved0;
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "}\n");

  buf_printf(b, "_t%d", tres);
  return 1;
}

/* all?/any?/none?/one? with a block: loop, count the truthy block results,
   and reduce to the predicate. Returns 1 if handled. */
static int emit_predicate_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int is_all = !strcmp(name, "all?"), is_any = !strcmp(name, "any?"),
      is_none = !strcmp(name, "none?"), is_one = !strcmp(name, "one?");
  if (!(is_all || is_any || is_none || is_one)) return 0;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  int range_recv = (rt == TY_RANGE);
  if (!ty_is_array(rt) && !range_recv) return 0;
  const char *k = range_recv ? "Int" : (rt == TY_POLY_ARRAY ? "Poly" : array_kind(rt));
  if (!k) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0;
  const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  /* the block's last expression is the C condition: require a bool. A bare
     `if (value)` would mis-handle Ruby truthiness for other types (0 / 0.0
     are truthy in Ruby but false in C), so leave those unsupported. */
  if (comp_ntype(c, bb[bn - 1]) != TY_BOOL) return 0;

  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int trecv = ++g_tmp, tcnt = ++g_tmp, ti = ++g_tmp;

  Buf rb; memset(&rb, 0, sizeof rb);
  if (range_recv) {
    int tr = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_Range _t%d = ", tr); emit_expr(c, recv, g_pre); buf_puts(g_pre, ";\n");
    buf_printf(&rb, "sp_IntArray_from_range(_t%d.first, _t%d.last - _t%d.excl)", tr, tr, tr);
    rt = TY_INT_ARRAY;
  }
  else emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent);
  emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "mrb_int _t%d = 0;\n", tcnt);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, k, trecv, ti);
  int bodyIndent = g_indent + 1;
  if (p0) {
    emit_indent(g_pre, bodyIndent);
    buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
  }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, bodyIndent);
  int saveIndent = g_indent;
  g_indent = bodyIndent;
  Buf vb; memset(&vb, 0, sizeof vb);
  emit_expr(c, bb[bn - 1], &vb);
  g_indent = saveIndent;
  emit_indent(g_pre, bodyIndent);
  buf_printf(g_pre, "if (%s) _t%d++;\n", vb.p ? vb.p : "0", tcnt);
  free(vb.p);
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "}\n");

  if (is_all) buf_printf(b, "(_t%d == sp_%sArray_length(_t%d))", tcnt, k, trecv);
  else if (is_any) buf_printf(b, "(_t%d > 0)", tcnt);
  else if (is_none) buf_printf(b, "(_t%d == 0)", tcnt);
  else buf_printf(b, "(_t%d == 1)", tcnt);
  return 1;
}

/* Emit the `pattern === elem` membership test for grep, given the element
   bound to C variable `ev`. Returns 1 if the pattern kind is supported. */
static int emit_grep_pred(Compiler *c, int pat, const char *ev, Buf *b) {
  const NodeTable *nt = c->nt;
  int re = re_lit_index(c, pat);
  if (re >= 0) { buf_printf(b, "sp_re_match_p(sp_re_pat_%d, %s)", re, ev); return 1; }
  const char *pty = nt_type(nt, pat);
  if (pty && !strcmp(pty, "RangeNode")) {
    int tr = ++g_tmp;
    buf_printf(b, "({ sp_Range _t%d = ", tr); emit_expr(c, pat, b);
    buf_printf(b, "; sp_range_include(&_t%d, %s); })", tr, ev);
    return 1;
  }
  if (pty && !strcmp(pty, "ConstantReadNode")) {
    const char *cn = nt_str(nt, pat, "name");
    if (!cn) return 0;
    if (!strcmp(cn, "Integer") || !strcmp(cn, "Fixnum")) buf_printf(b, "(%s).tag == SP_TAG_INT", ev);
    else if (!strcmp(cn, "String"))   buf_printf(b, "(%s).tag == SP_TAG_STR", ev);
    else if (!strcmp(cn, "Float"))    buf_printf(b, "(%s).tag == SP_TAG_FLT", ev);
    else if (!strcmp(cn, "Symbol"))   buf_printf(b, "(%s).tag == SP_TAG_SYM", ev);
    else if (!strcmp(cn, "Numeric"))  buf_printf(b, "((%s).tag == SP_TAG_INT || (%s).tag == SP_TAG_FLT)", ev, ev);
    else return 0;
    return 1;
  }
  return 0;
}

/* grep(pattern) / grep_v(pattern) without a block: collect elements for which
   `pattern === e` holds (or fails, for grep_v). Returns 1 if handled. */
static int emit_grep_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || (strcmp(name, "grep") && strcmp(name, "grep_v"))) return 0;
  if (nt_ref(nt, id, "block") >= 0) return 0;   /* block form unsupported */
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (argc != 1) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  int pat = argv[0];

  /* probe predicate support before emitting anything */
  Buf probe; memset(&probe, 0, sizeof probe);
  if (!emit_grep_pred(c, pat, "_e", &probe)) { free(probe.p); return 0; }
  free(probe.p);

  int neg = !strcmp(name, "grep_v");
  TyKind et = ty_array_elem(rt);
  int trecv = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp, te = ++g_tmp;

  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent);
  emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", k, tres, k);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, k, trecv, ti);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "%s _t%d = sp_%sArray_get(_t%d, _t%d);\n", c_type_name(et), te, k, trecv, ti);
  emit_indent(g_pre, g_indent + 1);
  char ev[16]; snprintf(ev, sizeof ev, "_t%d", te);
  buf_printf(g_pre, "if (%s(", neg ? "!" : "");
  emit_grep_pred(c, pat, ev, g_pre);
  buf_printf(g_pre, ")) sp_%sArray_push(_t%d, _t%d);\n", k, tres, te);
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "}\n");

  buf_printf(b, "_t%d", tres);
  return 1;
}

/* Emit the value for callee param `idx`: the provided arg node if any,
   else the param's default (a nil default becomes the type's default). */
static void emit_arg_or_default(Compiler *c, Scope *m, int idx, int provided, Buf *out) {
  LocalVar *p = scope_local(m, m->pnames[idx]);
  TyKind pt = p ? p->type : TY_INT;
  if (provided >= 0) {
    if (pt == TY_POLY) emit_boxed(c, provided, out);   /* box into a poly param */
    else {
      /* empty array literal `[]` defaults to IntArray in emit_expr; if the
         parameter expects a different array type, emit the right constructor */
      int nen = 0;
      const char *pty_node = nt_type(c->nt, provided);
      int is_empty_arr = pty_node && !strcmp(pty_node, "ArrayNode") &&
                         (nt_arr(c->nt, provided, "elements", &nen), nen == 0);
      if (is_empty_arr && ty_is_array(pt) && pt != TY_INT_ARRAY) {
        if (pt == TY_POLY_ARRAY) buf_puts(out, "sp_PolyArray_new()");
        else { const char *k = array_kind(pt); if (k) buf_printf(out, "sp_%sArray_new()", k); else emit_expr(c, provided, out); }
      }
      else emit_expr(c, provided, out);
    }
    return;
  }
  int dv = m->pdefault[idx];
  const char *dty = dv >= 0 ? nt_type(c->nt, dv) : NULL;
  if (dv < 0) {
    buf_puts(out, pt == TY_RANGE ? "(sp_Range){0}" : default_value(pt));
  } else if (dty && !strcmp(dty, "NilNode")) {
    /* nil default: emit the nil sentinel for the type */
    if (pt == TY_INT)    buf_puts(out, "SP_INT_NIL");
    else if (pt == TY_FLOAT) buf_puts(out, "sp_float_nil()");
    else if (pt == TY_STRING) buf_puts(out, "NULL");
    else buf_puts(out, pt == TY_RANGE ? "(sp_Range){0}" : default_value(pt));
  }
  else if (pt == TY_POLY) emit_boxed(c, dv, out);
  else {
    /* Default empty `[]` literal: emit the correct array constructor for
       the parameter type rather than always sp_IntArray_new(). */
    int den = 0;
    int is_empty_arr_dv = dty && !strcmp(dty, "ArrayNode") &&
                          (nt_arr(c->nt, dv, "elements", &den), den == 0);
    if (is_empty_arr_dv && ty_is_array(pt) && pt != TY_INT_ARRAY) {
      if (pt == TY_POLY_ARRAY) buf_puts(out, "sp_PolyArray_new()");
      else { const char *k = array_kind(pt); if (k) buf_printf(out, "sp_%sArray_new()", k); else emit_expr(c, dv, out); }
    }
    else emit_expr(c, dv, out);
  }
}

/* Emit a comma-separated argument list filling defaults for omitted
   optional params. `lead` is prepended before the first arg. */
/* Find the value node for keyword param named `kname` in a KeywordHashNode `kwh`. */
static int kwh_lookup(const NodeTable *nt, int kwh, const char *kname) {
  if (kwh < 0 || !kname) return -1;
  int en = 0;
  const int *elems = nt_arr(nt, kwh, "elements", &en);
  for (int e = 0; e < en; e++) {
    int key = nt_ref(nt, elems[e], "key");
    if (key < 0) continue;
    const char *kty = nt_type(nt, key);
    const char *kn = (kty && !strcmp(kty, "SymbolNode")) ? nt_str(nt, key, "value") : NULL;
    if (kn && !strcmp(kn, kname)) return nt_ref(nt, elems[e], "value");
  }
  return -1;
}

/* Emit a PolyArray expression that collects call args[from..pos_argc-1].
   SplatNode arguments are expanded element-by-element into the array. */
static void emit_rest_pack(Compiler *c, int from, int pos_argc, const int *argv, Buf *b) {
  const NodeTable *nt = c->nt;
  /* Optimize: single pure-splat → direct conversion */
  if (pos_argc == from + 1) {
    const char *aty = argv ? nt_type(nt, argv[from]) : NULL;
    if (aty && !strcmp(aty, "SplatNode")) {
      int inner = nt_ref(nt, argv[from], "expression");
      TyKind at = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
      if (at == TY_INT_ARRAY) {
        buf_puts(b, "sp_IntArray_to_poly("); emit_expr(c, inner, b); buf_puts(b, ")");
        return;
      }
      if (at == TY_STR_ARRAY) {
        buf_puts(b, "sp_StrArray_to_poly_fmt("); emit_expr(c, inner, b); buf_puts(b, ")");
        return;
      }
      if (at == TY_FLOAT_ARRAY) {
        buf_puts(b, "sp_typed_to_poly("); emit_expr(c, inner, b); buf_puts(b, ", SP_BUILTIN_FLT_ARRAY)");
        return;
      }
      if (at == TY_POLY_ARRAY) {
        emit_expr(c, inner, b);
        return;
      }
    }
  }
  /* Empty rest */
  if (!argv || pos_argc <= from) {
    buf_puts(b, "sp_PolyArray_new()");
    return;
  }
  /* General case: build PolyArray as statement expression */
  int t = ++g_tmp;
  buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", t, t);
  for (int i = from; i < pos_argc; i++) {
    const char *aty = nt_type(nt, argv[i]);
    if (aty && !strcmp(aty, "SplatNode")) {
      int inner = nt_ref(nt, argv[i], "expression");
      TyKind at = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
      Buf arr; memset(&arr, 0, sizeof arr);
      emit_expr(c, inner, &arr);
      const char *ap = arr.p ? arr.p : "NULL";
      if (at == TY_INT_ARRAY)
        buf_printf(b, " { sp_IntArray *_sa = %s; for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_int(_sa->data[_sa->start+_si])); }", ap, t);
      else if (at == TY_STR_ARRAY)
        buf_printf(b, " { sp_StrArray *_sa = %s; for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_str(_sa->data[_si])); }", ap, t);
      else if (at == TY_FLOAT_ARRAY)
        buf_printf(b, " { sp_FloatArray *_sa = %s; for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_float(_sa->data[_si])); }", ap, t);
      else if (at == TY_POLY_ARRAY)
        buf_printf(b, " { sp_PolyArray *_sa = %s; for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, _sa->data[_si]); }", ap, t);
      else { /* scalar splat: single element */
        Buf el; memset(&el, 0, sizeof el);
        emit_boxed(c, inner, &el);
        buf_printf(b, " sp_PolyArray_push(_t%d, %s);", t, el.p ? el.p : "sp_box_nil()");
        free(el.p);
      }
      free(arr.p);
    } else {
      Buf el; memset(&el, 0, sizeof el);
      emit_boxed(c, argv[i], &el);
      buf_printf(b, " sp_PolyArray_push(_t%d, %s);", t, el.p ? el.p : "sp_box_nil()");
      free(el.p);
    }
  }
  buf_printf(b, " _t%d; })", t);
}

/* Emit the element at index `elem_idx` from a typed array temp `tmp`. */
static void emit_array_elem_at(TyKind at, int tmp, int elem_idx, Buf *b) {
  if (at == TY_INT_ARRAY)
    buf_printf(b, "(_t%d && %d < _t%d->len ? _t%d->data[_t%d->start+%d] : 0)", tmp, elem_idx, tmp, tmp, tmp, elem_idx);
  else if (at == TY_STR_ARRAY)
    buf_printf(b, "(_t%d && %d < _t%d->len ? _t%d->data[%d] : NULL)", tmp, elem_idx, tmp, tmp, elem_idx);
  else if (at == TY_FLOAT_ARRAY)
    buf_printf(b, "(_t%d && %d < _t%d->len ? _t%d->data[%d] : 0.0)", tmp, elem_idx, tmp, tmp, elem_idx);
  else
    buf_printf(b, "(_t%d && %d < _t%d->len ? _t%d->data[%d] : sp_box_nil())", tmp, elem_idx, tmp, tmp, elem_idx);
}

/* Emit a PolyArray containing elements from array temp `tmp` starting at `from_idx`,
   then the remaining positional args from argv[argv_from..pos_argc-1]. */
static void emit_rest_from_splat_and_argv(int tmp, TyKind at, int from_idx,
                                          Compiler *c, int argv_from, int pos_argc,
                                          const int *argv, Buf *b) {
  int t = ++g_tmp;
  buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", t, t);
  /* elements from the splatted array starting at from_idx */
  if (at == TY_INT_ARRAY)
    buf_printf(b, " if (_t%d) for (mrb_int _si = %d; _si < _t%d->len; _si++) sp_PolyArray_push(_t%d, sp_box_int(_t%d->data[_t%d->start+_si]));", tmp, from_idx, tmp, t, tmp, tmp);
  else if (at == TY_STR_ARRAY)
    buf_printf(b, " if (_t%d) for (mrb_int _si = %d; _si < _t%d->len; _si++) sp_PolyArray_push(_t%d, sp_box_str(_t%d->data[_si]));", tmp, from_idx, tmp, t, tmp);
  else if (at == TY_FLOAT_ARRAY)
    buf_printf(b, " if (_t%d) for (mrb_int _si = %d; _si < _t%d->len; _si++) sp_PolyArray_push(_t%d, sp_box_float(_t%d->data[_si]));", tmp, from_idx, tmp, t, tmp);
  else if (at == TY_POLY_ARRAY)
    buf_printf(b, " if (_t%d) for (mrb_int _si = %d; _si < _t%d->len; _si++) sp_PolyArray_push(_t%d, _t%d->data[_si]);", tmp, from_idx, tmp, t, tmp);
  /* then suffix args after the splat */
  for (int j = argv_from; j < pos_argc; j++) {
    const char *jty = argv ? nt_type(c->nt, argv[j]) : NULL;
    if (jty && !strcmp(jty, "SplatNode")) {
      int inner2 = nt_ref(c->nt, argv[j], "expression");
      TyKind at2 = inner2 >= 0 ? comp_ntype(c, inner2) : TY_UNKNOWN;
      Buf arr2; memset(&arr2, 0, sizeof arr2); emit_expr(c, inner2, &arr2);
      const char *ap2 = arr2.p ? arr2.p : "NULL";
      if (at2 == TY_INT_ARRAY)
        buf_printf(b, " { sp_IntArray *_sa = %s; for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_int(_sa->data[_sa->start+_si])); }", ap2, t);
      else if (at2 == TY_POLY_ARRAY)
        buf_printf(b, " { sp_PolyArray *_sa = %s; for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, _sa->data[_si]); }", ap2, t);
      else { Buf el2; memset(&el2, 0, sizeof el2); emit_boxed(c, inner2, &el2); buf_printf(b, " sp_PolyArray_push(_t%d, %s);", t, el2.p ? el2.p : "sp_box_nil()"); free(el2.p); }
      free(arr2.p);
    } else {
      Buf el; memset(&el, 0, sizeof el); emit_boxed(c, argv[j], &el);
      buf_printf(b, " sp_PolyArray_push(_t%d, %s);", t, el.p ? el.p : "sp_box_nil()");
      free(el.p);
    }
  }
  buf_printf(b, " _t%d; })", t);
}

static void emit_args_filled(Compiler *c, int callee_idx, int argsNode, const char *lead, Buf *out) {
  Scope *m = &c->scopes[callee_idx];
  const NodeTable *nt = c->nt;
  int argc = 0;
  const int *argv = argsNode >= 0 ? nt_arr(nt, argsNode, "arguments", &argc) : NULL;
  /* Separate trailing keyword-hash arg (if any) from positional args. */
  int kwh = -1;
  int pos_argc = argc;
  if (argc > 0 && nt_type(nt, argv[argc - 1]) &&
      !strcmp(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) {
    kwh = argv[argc - 1];
    pos_argc = argc - 1;
  }
  /* Find the first SplatNode in positional args. If it comes before rest_idx
     (or before nparams for rest-less methods), pre-evaluate it to a temp so
     we can index into it per fixed param. */
  int splat_idx = -1;  /* index into argv[] of the SplatNode */
  int splat_tmp = -1;  TyKind splat_at = TY_UNKNOWN;
  for (int k = 0; k < pos_argc; k++) {
    if (argv && nt_type(nt, argv[k]) && !strcmp(nt_type(nt, argv[k]), "SplatNode")) {
      int need_expand = (m->rest_idx >= 0 && k < m->rest_idx) ||
                        (m->rest_idx < 0 && k < m->nparams);
      if (need_expand) {
        splat_idx = k;
        int inner = nt_ref(nt, argv[k], "expression");
        splat_at = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
        if (ty_is_array(splat_at) || splat_at == TY_POLY_ARRAY) {
          splat_tmp = ++g_tmp;
          emit_indent(g_pre, g_indent);
          emit_ctype(c, splat_at, g_pre);
          buf_printf(g_pre, " _t%d = ", splat_tmp);
          emit_expr(c, inner, g_pre);
          buf_puts(g_pre, ";\n");
        }
      }
      break;
    }
  }
  for (int i = 0; i < m->nparams; i++) {
    buf_puts(out, i == 0 ? lead : ", ");
    if (m->rest_idx >= 0 && i == m->rest_idx) {
      if (splat_tmp >= 0) {
        /* rest = splat_arr[i-splat_idx..] + argv[splat_idx+1..] */
        emit_rest_from_splat_and_argv(splat_tmp, splat_at, i - splat_idx,
                                      c, splat_idx + 1, pos_argc, argv, out);
      } else {
        emit_rest_pack(c, i, pos_argc, argv, out);
      }
    } else if (splat_tmp >= 0 && i >= splat_idx) {
      /* this param comes from the splatted array at offset (i - splat_idx) */
      emit_array_elem_at(splat_at, splat_tmp, i - splat_idx, out);
    } else {
      /* Check if this param has a keyword match (lookup by param name in kwh). */
      int kv = kwh >= 0 ? kwh_lookup(nt, kwh, m->pnames[i]) : -1;
      if (kv >= 0)
        emit_arg_or_default(c, m, i, kv, out);
      else
        emit_arg_or_default(c, m, i, i < pos_argc ? argv[i] : -1, out);
    }
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
  /* separate keyword-hash arg */
  int kwh_d = -1, pos_argc_d = argc;
  if (argc > 0 && nt_type(nt, argv[argc - 1]) &&
      !strcmp(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) {
    kwh_d = argv[argc - 1]; pos_argc_d = argc - 1;
  }
  int np = m ? m->nparams : pos_argc_d;
  /* evaluate each param value (provided arg or default) into a temp so the
     virtual-dispatch cases reuse them without re-evaluating */
  int *atmp = np ? malloc(sizeof(int) * np) : NULL;
  const char *saved_self = g_self;
  for (int k = 0; k < np; k++) {
    atmp[k] = ++g_tmp;
    Buf ab; memset(&ab, 0, sizeof ab);
    LocalVar *p = m ? scope_local(m, m->pnames[k]) : NULL;
    if (m && m->rest_idx >= 0 && k == m->rest_idx) {
      /* rest param: pack remaining positional args into PolyArray */
      emit_rest_pack(c, k, pos_argc_d, argv, &ab);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = %s;\n", atmp[k], ab.p ? ab.p : "sp_PolyArray_new()");
    } else {
      int kv = (m && kwh_d >= 0) ? kwh_lookup(nt, kwh_d, m->pnames[k]) : -1;
      int provided = kv >= 0 ? kv : (k < pos_argc_d ? argv[k] : -1);
      /* Default expressions (e.g. `@ivar * 10`) reference the callee's self,
         not the caller's. Temporarily point g_self at the receiver. */
      if (provided < 0) g_self = selfptr;
      emit_arg_or_default(c, m, k, provided, &ab);
      g_self = saved_self;
      emit_indent(g_pre, g_indent);
      emit_ctype(c, p ? p->type : comp_ntype(c, k < argc ? argv[k] : -1), g_pre);
      buf_printf(g_pre, " _t%d = ", atmp[k]);
      buf_puts(g_pre, ab.p ? ab.p : ""); buf_puts(g_pre, ";\n");
    }
    free(ab.p);
  }

  /* The aliased name may differ from the defining method's real name. */
  const char *mname = m ? m->name : name;

  int virtual = dispatch_impl_count(c, cid, name) > 1 && is_scalar_ret(ret);

  if (!virtual) {
    buf_printf(b, "sp_%s_%s((sp_%s *)%s", c->classes[defcls].name, mc(mname), c->classes[defcls].name, selfptr);
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
    int kmi = comp_method_in_chain(c, k, name, &kd);
    if (kmi < 0) continue;
    buf_printf(b, " case %d: _t%d = sp_%s_%s((sp_%s *)%s", k, rtmp,
               c->classes[kd].name, mc(c->scopes[kmi].name), c->classes[kd].name, selfptr);
    for (int a = 0; a < np; a++) buf_printf(b, ", _t%d", atmp[a]);
    buf_puts(b, "); break;");
  }
  buf_printf(b, " default: _t%d = sp_%s_%s((sp_%s *)%s", rtmp,
             c->classes[defcls].name, mc(mname), c->classes[defcls].name, selfptr);
  for (int a = 0; a < np; a++) buf_printf(b, ", _t%d", atmp[a]);
  buf_printf(b, "); break; } _t%d; })", rtmp);
  free(atmp);
}

/* array.each_with_object(init) { |x, acc| ... } → acc (pre-statements to g_pre) */
static int emit_each_with_object_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || strcmp(name, "each_with_object")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *bty = nt_type(nt, block);
  if (!bty || strcmp(bty, "BlockNode")) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (argc < 1 || !argv) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind et = ty_array_elem(rt);
  TyKind accT = infer_type(c, argv[0]);
  if (accT == TY_UNKNOWN) {
    const char *a0ty = nt_type(nt, argv[0]);
    int an0 = 0;
    if (a0ty && !strcmp(a0ty, "ArrayNode")) nt_arr(nt, argv[0], "elements", &an0);
    if (a0ty && !strcmp(a0ty, "ArrayNode") && an0 == 0) accT = TY_INT_ARRAY;
    else return 0;
  }
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  const char *p0_orig = block_param_name(c, block, 0);
  const char *p1_orig = block_param_name(c, block, 1);
  const char *p0 = p0_orig ? rename_local(p0_orig) : NULL;
  const char *p1 = p1_orig ? rename_local(p1_orig) : NULL;

  /* Receiver */
  int trecv = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);

  /* Accumulator */
  int tacc = ++g_tmp;
  Buf accb; memset(&accb, 0, sizeof accb); emit_expr(c, argv[0], &accb);
  emit_indent(g_pre, g_indent); emit_ctype(c, accT, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", tacc, accb.p ? accb.p : default_value(accT)); free(accb.p);

  /* Save outer vars if block params shadow them */
  Scope *cs = comp_scope_of(c, id);
  LocalVar *outer_p0 = (p0 && cs) ? scope_local(cs, p0) : NULL;
  int ts_p0 = 0;
  if (outer_p0) {
    ts_p0 = ++g_tmp;
    Buf ot; memset(&ot, 0, sizeof ot); emit_ctype(c, outer_p0->type, &ot);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "%s _t%d = lv_%s;\n", ot.p ? ot.p : "mrb_int", ts_p0, p0); free(ot.p);
  }
  LocalVar *outer_p1 = (p1 && cs) ? scope_local(cs, p1) : NULL;
  int ts_p1 = 0;
  if (outer_p1) {
    ts_p1 = ++g_tmp;
    Buf ot; memset(&ot, 0, sizeof ot); emit_ctype(c, outer_p1->type, &ot);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "%s _t%d = lv_%s;\n", ot.p ? ot.p : "mrb_int", ts_p1, p1); free(ot.p);
  }

  /* Bind accumulator to p1 before loop */
  if (p1) {
    emit_indent(g_pre, g_indent);
    TyKind p1_type = outer_p1 ? outer_p1->type : accT;
    if (p1_type == TY_POLY && accT != TY_POLY) {
      char tacc_s[32]; snprintf(tacc_s, sizeof tacc_s, "_t%d", tacc);
      Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, accT, tacc_s, &bx);
      buf_printf(g_pre, "lv_%s = %s;\n", p1, bx.p ? bx.p : tacc_s); free(bx.p);
    }
    else {
      buf_printf(g_pre, "lv_%s = _t%d;\n", p1, tacc);
    }
  }

  /* Loop */
  int ti = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
             ti, ti, k, trecv, ti);

  /* Assign element to p0 */
  if (p0) {
    emit_indent(g_pre, g_indent + 1);
    TyKind p0_type = outer_p0 ? outer_p0->type : et;
    if (p0_type == TY_POLY && et != TY_POLY) {
      char elem_s[64];
      snprintf(elem_s, sizeof elem_s, "sp_%sArray_get(_t%d, _t%d)", k, trecv, ti);
      Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, et, elem_s, &bx);
      buf_printf(g_pre, "lv_%s = %s;\n", p0, bx.p ? bx.p : elem_s); free(bx.p);
    }
    else {
      buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
    }
  }

  /* Body */
  int save_indent = g_indent; g_indent++;
  for (int j = 0; j < bn; j++) emit_stmt(c, bb[j], g_pre, g_indent);
  g_indent = save_indent;

  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");

  /* Restore outer vars */
  if (p0 && ts_p0 > 0) {
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "lv_%s = _t%d;\n", p0, ts_p0);
  }
  if (p1 && ts_p1 > 0) {
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "lv_%s = _t%d;\n", p1, ts_p1);
  }

  /* The expression evaluates to the accumulator */
  buf_printf(b, "_t%d", tacc);
  return 1;
}

static void emit_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  if (emit_partition_expr(c, id, b)) return;
  if (emit_collect_expr(c, id, b)) return;
  if (emit_predicate_expr(c, id, b)) return;
  if (emit_grep_expr(c, id, b)) return;
  if (emit_minmax_by_expr(c, id, b)) return;
  if (emit_sort_cmp_expr(c, id, b)) return;
  if (emit_minmax_cmp_expr(c, id, b)) return;
  if (emit_step_array_expr(c, id, b)) return;
  if (emit_slice_when_chunk_inspect_expr(c, id, b)) return;
  if (emit_product_inspect_expr(c, id, b)) return;
  if (emit_bsearch_expr(c, id, b)) return;
  if (emit_sum_block_expr(c, id, b)) return;
  if (emit_transform_hash_expr(c, id, b)) return;
  if (emit_gsub_block_expr(c, id, b)) return;
  if (emit_inject_expr(c, id, b)) return;
  if (emit_reduce_block_expr(c, id, b)) return;
  if (emit_sortby_expr(c, id, b)) return;
  if (emit_each_with_object_expr(c, id, b)) return;
  if (emit_inline_expr(c, id, b)) return;  /* value-returning yield method */
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  if (!name) unsupported(c, id, "call (no name)");

  /* system(cmd, ...) expr: run and return bool */
  if (recv < 0 && !strcmp(name, "system") && argc >= 1) {
    int ts = ++g_tmp;
    buf_printf(b, "({ const char *_sys_%d[] = { ", ts);
    for (int k = 0; k < argc; k++) { if (k > 0) buf_puts(b, ", "); emit_expr(c, argv[k], b); }
    buf_printf(b, ", NULL }; (mrb_bool)sp_system_args(%d, _sys_%d); })", argc, ts);
    return;
  }
  /* trap(...) / Signal.trap(...) expr: return "DEFAULT" */
  {
    int is_trap = (recv < 0 && !strcmp(name, "trap"));
    if (!is_trap && recv >= 0 && !strcmp(name, "trap") && argc >= 1) {
      const char *rty2 = nt_type(nt, recv);
      if (rty2 && (!strcmp(rty2, "ConstantReadNode") || !strcmp(rty2, "ConstantPathNode"))) {
        const char *rn = nt_str(nt, recv, "name");
        if (rn && !strcmp(rn, "Signal")) is_trap = 1;
      }
    }
    if (is_trap && argc >= 1) { emit_str_literal(b, "DEFAULT"); return; }
  }

  /* proc {} / lambda {} / Proc.new {} literal -> a first-class Proc value */
  if (comp_ntype(c, id) == TY_PROC && nt_ref(nt, id, "block") >= 0) {
    emit_proc_literal(c, id, b);
    return;
  }

  /* Safe navigation &. : nil receiver -> return nil/0; non-nil -> emit conditional */
  {
    const char *safe_op = nt_str(nt, id, "call_operator");
    if (recv >= 0 && safe_op && !strcmp(safe_op, "&.")) {
      TyKind rrt = comp_ntype(c, recv);
      if (rrt == TY_NIL) {
        /* nil&.foo always returns nil */
        TyKind ret = comp_ntype(c, id);
        const char *dv = default_value(ret);
        buf_puts(b, dv ? dv : "0");
        return;
      }
      if (rrt == TY_POLY) {
        /* poly &. method: nil check + dispatch on non-nil string/int/other */
        int tsn = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _sn_%d = ", tsn); emit_expr(c, recv, b); buf_puts(b, "; ");
        buf_printf(b, "_sn_%d.tag == SP_TAG_NIL ? sp_box_nil() : ", tsn);
        /* dispatch the method on the non-nil value */
        if (!strcmp(name, "upcase")) {
          buf_printf(b, "sp_box_str(sp_str_upcase(_sn_%d.v.s))", tsn);
        }
        else if (!strcmp(name, "downcase")) {
          buf_printf(b, "sp_box_str(sp_str_downcase(_sn_%d.v.s))", tsn);
        }
        else if (!strcmp(name, "length") || !strcmp(name, "size")) {
          buf_printf(b, "sp_box_int(sp_poly_length(_sn_%d))", tsn);
        }
        else if (!strcmp(name, "inspect")) {
          buf_printf(b, "sp_box_str(sp_poly_inspect(_sn_%d))", tsn);
        }
        else if (!strcmp(name, "to_s")) {
          buf_printf(b, "sp_box_str(sp_poly_to_s(_sn_%d))", tsn);
        }
        else {
          /* fallback: return the poly value unchanged */
          buf_printf(b, "_sn_%d", tsn);
        }
        buf_puts(b, "; })");
        return;
      }
      /* non-nil typed receiver: for concrete types, dispatch as normal (always non-nil) */
    }
  }

  /* n.times/upto/downto/step { ... } in expression position: run the loop
     (lowered to a statement) and evaluate to the receiver (Ruby returns self) */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 && comp_ntype(c, recv) == TY_INT &&
      (!strcmp(name, "times") || !strcmp(name, "upto") ||
       !strcmp(name, "downto") || !strcmp(name, "step"))) {
    buf_puts(b, "({ ");
    emit_iteration_stmt(c, id, b, 0);
    emit_expr(c, recv, b); buf_puts(b, "; })");
    return;
  }
  /* n.times / lo.upto(hi) / hi.downto(lo) without block: produce sp_Range for chaining */
  if (recv >= 0 && nt_ref(nt, id, "block") < 0 && comp_ntype(c, recv) == TY_INT &&
      comp_ntype(c, id) == TY_RANGE) {
    if (!strcmp(name, "times")) {
      buf_puts(b, "(sp_Range){ .first = 0, .last = "); emit_expr(c, recv, b); buf_puts(b, ", .excl = 1 }");
      return;
    }
    if (!strcmp(name, "upto") && argc == 1) {
      buf_puts(b, "(sp_Range){ .first = "); emit_expr(c, recv, b);
      buf_puts(b, ", .last = "); emit_expr(c, argv[0], b); buf_puts(b, ", .excl = 0 }");
      return;
    }
    if (!strcmp(name, "downto") && argc == 1) {
      buf_puts(b, "(sp_Range){ .first = "); emit_expr(c, argv[0], b);
      buf_puts(b, ", .last = "); emit_expr(c, recv, b); buf_puts(b, ", .excl = 0 }");
      return;
    }
  }

  /* <proc>.call(args) / .() / [] -> sp_proc_call with the mrb_int[] ABI.
     (A `&block`-param `.call` is handled earlier by the inline path, whose
     receiver name matches g_block_param_name; this is the escaped-value case.) */
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC &&
      (!strcmp(name, "call") || !strcmp(name, "()") || !strcmp(name, "[]"))) {
    TyKind rty = comp_ntype(c, id);          /* the call's result = proc's body return */
    int unbox_ptr = proc_slot_is_ptr(rty);
    if (unbox_ptr) { buf_puts(b, "("); emit_ctype(c, rty, b); buf_puts(b, ")(uintptr_t)("); }
    buf_puts(b, "sp_proc_call(");
    emit_expr(c, recv, b);
    buf_puts(b, ", (mrb_int[16]){");
    for (int k = 0; k < argc; k++) {
      if (k) buf_puts(b, ", ");
      /* a heap-pointer argument is laundered into the mrb_int slot */
      if (proc_slot_is_ptr(comp_ntype(c, argv[k]))) { buf_puts(b, "(mrb_int)(uintptr_t)("); emit_expr(c, argv[k], b); buf_puts(b, ")"); }
      else emit_expr(c, argv[k], b);
    }
    buf_puts(b, "})");
    if (unbox_ptr) buf_puts(b, ")");
    return;
  }

  /* Proc introspection: arity / lambda? read the sp_Proc metadata directly. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC && argc == 0 && !strcmp(name, "arity")) {
    buf_puts(b, "sp_proc_arity("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
  }
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC && argc == 0 && !strcmp(name, "lambda?")) {
    buf_puts(b, "sp_proc_lambda_p("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
  }
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC && argc == 0 && !strcmp(name, "parameters")) {
    buf_puts(b, "sp_proc_parameters("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
  }

  /* `arr << x` / push / append in value position: mutate, then yield the array
     (statement position is handled earlier by emit_array_mutate_stmt). */
  if (recv >= 0 && (!strcmp(name, "<<") || !strcmp(name, "push") || !strcmp(name, "append")) &&
      argc >= 1 && ty_is_array(comp_ntype(c, recv))) {
    TyKind art = comp_ntype(c, recv);
    /* Lift: when a typed-array literal is pushed a heterogeneous element,
       rebuild the receiver as a PolyArray rather than emitting a type mismatch. */
    int needs_lift = 0;
    if (art != TY_POLY_ARRAY && array_kind(art)) {
      TyKind elem_t = ty_array_elem(art);
      const char *rty = nt_type(nt, recv);
      if (rty && !strcmp(rty, "ArrayNode")) {
        for (int a = 0; a < argc; a++) {
          TyKind at = comp_ntype(c, argv[a]);
          if (at != TY_UNKNOWN && at != elem_t) { needs_lift = 1; break; }
        }
      }
    }
    if (needs_lift) {
      int en = 0;
      const int *els = nt_arr(nt, recv, "elements", &en);
      int t = ++g_tmp;
      buf_puts(b, "({ ");
      buf_printf(b, "sp_PolyArray *_t%d = sp_PolyArray_new(); ", t);
      for (int j = 0; j < en; j++) {
        Buf el; memset(&el, 0, sizeof el);
        emit_boxed(c, els[j], &el);
        buf_printf(b, "sp_PolyArray_push(_t%d, %s); ", t, el.p ? el.p : "sp_box_nil()");
        free(el.p);
      }
      for (int a = 0; a < argc; a++) {
        buf_printf(b, "sp_PolyArray_push(_t%d, ", t);
        emit_boxed(c, argv[a], b);
        buf_puts(b, "); ");
      }
      buf_printf(b, "_t%d; })", t);
      return;
    }
    const char *k = (art == TY_POLY_ARRAY) ? "Poly" : array_kind(art);
    int t = ++g_tmp;
    buf_puts(b, "({ ");
    emit_ctype(c, art, b); buf_printf(b, " _t%d = ", t); emit_expr(c, recv, b); buf_puts(b, "; ");
    for (int a = 0; a < argc; a++) {
      buf_printf(b, "sp_%sArray_push(_t%d, ", k, t);
      if (art == TY_POLY_ARRAY) emit_boxed(c, argv[a], b); else emit_expr(c, argv[a], b);
      buf_puts(b, "); ");
    }
    buf_printf(b, "_t%d; })", t);
    return;
  }

  /* __dir__ -> the source file's directory (compile-time literal, mirroring
     the legacy generator). */
  if (recv < 0 && !strcmp(name, "__dir__") && argc == 0) {
    const char *sf = nt->source_file;
    char dir[1024];
    if (sf && strrchr(sf, '/')) { size_t n = (size_t)(strrchr(sf, '/') - sf); if (n >= sizeof dir) n = sizeof dir - 1; if (n == 0) { dir[0] = '/'; dir[1] = 0; } else { memcpy(dir, sf, n); dir[n] = 0; } }
    else { dir[0] = '.'; dir[1] = 0; }
    emit_str_literal(b, dir);
    return;
  }

  /* at_exit { ... } -> register the block as a Proc; main()'s tail runs the
     hooks in reverse order. The registration expression evaluates to the proc. */
  if (recv < 0 && !strcmp(name, "at_exit") && nt_ref(nt, id, "block") >= 0) {
    g_needs_at_exit = 1;
    buf_puts(b, "(sp_at_exit_hooks[sp_at_exit_count++] = ");
    emit_proc_literal(c, id, b);
    buf_puts(b, ")");
    return;
  }

  /* __method__ / __callee__ -> the enclosing method's name as a symbol
     (nil at the top level) */
  if (recv < 0 && argc == 0 &&
      (!strcmp(name, "__method__") || !strcmp(name, "__callee__"))) {
    Scope *s = comp_scope_of(c, id);
    if (s && s->name && s->name[0]) buf_printf(b, "(sp_sym)%d", comp_sym_intern(c, s->name));
    else buf_puts(b, "sp_box_nil()");
    return;
  }

  /* block_given? / self.block_given? -> true inside an inlined yielding
     method (we only inline when a block is present) */
  if (!strcmp(name, "block_given?") &&
      (recv < 0 || (nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "SelfNode")))) {
    buf_puts(b, g_block_id >= 0 ? "1" : "0");
    return;
  }

  /* Kernel conversions */
  if (recv < 0 && comp_method_index(c, name) < 0) {
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (!strcmp(name, "Integer") && ac == 1) {
      TyKind at = comp_ntype(c, av[0]);
      if (at == TY_STRING) { buf_puts(b, "sp_str_to_i_strict("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else if (at == TY_FLOAT) { buf_puts(b, "((mrb_int)("); emit_expr(c, av[0], b); buf_puts(b, "))"); }
      else if (at == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else { buf_puts(b, "("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      return;
    }
    if (!strcmp(name, "Float") && ac == 1) {
      TyKind at = comp_ntype(c, av[0]);
      if (at == TY_STRING) { buf_puts(b, "sp_str_to_f_strict("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else if (at == TY_INT) { buf_puts(b, "((mrb_float)("); emit_expr(c, av[0], b); buf_puts(b, "))"); }
      else if (at == TY_POLY) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else { buf_puts(b, "("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      return;
    }
  }

  /* raise */
  if (recv < 0 && !strcmp(name, "raise")) {
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (ac == 0) {
      if (g_rescue_cls) buf_printf(b, "sp_raise_cls(%s, %s)", g_rescue_cls, g_rescue_msg);
      else buf_puts(b, "sp_raise((&(\"\\xff\")[1]))");
    }
    else if (ac == 1 && nt_type(nt, av[0]) &&
             (!strcmp(nt_type(nt, av[0]), "ConstantReadNode") || !strcmp(nt_type(nt, av[0]), "ConstantPathNode"))) {
      buf_printf(b, "sp_raise_cls(\"%s\", (&(\"\\xff\")[1]))", nt_str(nt, av[0], "name"));
    }
    else if (ac >= 2 && nt_type(nt, av[0]) &&
             (!strcmp(nt_type(nt, av[0]), "ConstantReadNode") || !strcmp(nt_type(nt, av[0]), "ConstantPathNode"))) {
      buf_printf(b, "sp_raise_cls(\"%s\", ", nt_str(nt, av[0], "name"));
      emit_expr(c, av[1], b); buf_puts(b, ")");
    }
    else {
      buf_puts(b, "sp_raise("); emit_expr(c, av[0], b); buf_puts(b, ")");
    }
    return;
  }

  /* exception object methods */
  if (recv >= 0 && comp_ntype(c, recv) == TY_EXCEPTION) {
    if (!strcmp(name, "message") || !strcmp(name, "to_s") || !strcmp(name, "to_str")) {
      buf_puts(b, "sp_exc_message("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (!strcmp(name, "full_message")) {
      int t = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Exception *_t%d = ", t);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      buf_printf(b, "sp_sprintf(\"%%s: %%s\", sp_exc_class_name(_t%d), sp_exc_message(_t%d))", t, t);
      return;
    }
    if (!strcmp(name, "inspect")) {
      /* #<ClassName: message> */
      int t = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Exception *_t%d = ", t);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      buf_printf(b, "sp_sprintf(\"#<%%s: %%s>\", sp_exc_class_name(_t%d), sp_exc_message(_t%d))", t, t);
      return;
    }
    if (!strcmp(name, "class")) {  /* used as .class.to_s / .class.name */
      buf_puts(b, "sp_exc_class_name("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (!strcmp(name, "backtrace")) {
      /* spinel does not capture stack frames: an empty array */
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), sp_StrArray_new())");
      return;
    }
  }

  if (recv < 0 && comp_method_index(c, name) >= 0) { emit_method_call(c, id, b); return; }
  /* bare call to a sibling class method (inside def self.foo, calling bar()) */
  if (recv < 0) {
    Scope *encl = comp_scope_of(c, id);
    if (encl && encl->is_cmethod && encl->class_id >= 0) {
      int smi = comp_cmethod_in_chain(c, encl->class_id, name, NULL);
      if (smi >= 0) {
        Scope *ms = &c->scopes[smi];
        emit_method_cname(c, ms, b);
        buf_puts(b, "(");
        emit_args_filled(c, smi, nt_ref(nt, id, "arguments"), "", b);
        buf_puts(b, ")");
        return;
      }
    }
  }
  /* bare call to a module_function method made available via top-level include */
  if (recv < 0) {
    int imi = comp_included_method_index(c, name);
    if (imi >= 0) {
      Scope *ms = &c->scopes[imi];
      emit_method_cname(c, ms, b);
      buf_puts(b, "(");
      emit_args_filled(c, imi, nt_ref(nt, id, "arguments"), "", b);
      buf_puts(b, ")");
      return;
    }
  }

  /* X.class.name / .to_s -> identity: X.class already evaluates to the
     class-name string. */
  if (recv >= 0 && argc == 0 && (!strcmp(name, "name") || !strcmp(name, "to_s")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "class")) {
    emit_expr(c, recv, b);
    return;
  }
  /* SomeClass.name / .to_s / .inspect -> the class-name string */
  if (recv >= 0 && argc == 0 &&
      (!strcmp(name, "name") || !strcmp(name, "to_s") || !strcmp(name, "inspect")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && comp_class_index(c, nt_str(nt, recv, "name")) >= 0) {
    buf_printf(b, "SPL(\"%s\")", nt_str(nt, recv, "name"));
    return;
  }

  /* SomeClass.superclass -> the parent class name (Object when implicit) */
  if (recv >= 0 && argc == 0 && !strcmp(name, "superclass") &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name")) {
    int ci = comp_class_index(c, nt_str(nt, recv, "name"));
    if (ci >= 0) {
      int par = c->classes[ci].parent;
      buf_printf(b, "SPL(\"%s\")", par >= 0 ? c->classes[par].name : "Object");
      return;
    }
  }

  /* x.class -> the class-name string (compile-time for known types) */
  if (recv >= 0 && !strcmp(name, "class") && argc == 0) {
    TyKind rt = comp_ntype(c, recv);
    const char *cn = NULL;
    if (rt == TY_INT) cn = "Integer";
    else if (rt == TY_FLOAT) cn = "Float";
    else if (rt == TY_STRING) cn = "String";
    else if (rt == TY_SYMBOL) cn = "Symbol";
    else if (rt == TY_RANGE) cn = "Range";
    else if (rt == TY_TIME) cn = "Time";
    else if (rt == TY_NIL) cn = "NilClass";
    else if (ty_is_array(rt)) cn = "Array";
    else if (ty_is_hash(rt)) cn = "Hash";
    else if (ty_is_object(rt)) cn = c->classes[ty_object_class(rt)].name;
    if (cn) { buf_printf(b, "SPL(\"%s\")", cn); return; }
    if (rt == TY_BOOL) {
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") ? SPL(\"TrueClass\") : SPL(\"FalseClass\"))");
      return;
    }
    if (rt == TY_POLY) {
      buf_puts(b, "sp_poly_class_name("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
  }

  /* freeze / frozen? on an array set/read the struct's frozen flag */
  if (recv >= 0 && argc == 0 && comp_ntype(c, recv) != TY_POLY) {
    TyKind crt = comp_ntype(c, recv);
    const char *ck = (crt == TY_POLY_ARRAY) ? "Poly" : array_kind(crt);
    if (ck && !strcmp(name, "freeze")) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", ck, t); emit_expr(c, recv, b);
      buf_printf(b, "; if (_t%d) _t%d->frozen = 1; _t%d; })", t, t, t);
      return;
    }
    if (ck && !strcmp(name, "frozen?")) {
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ")->frozen != 0)");
      return;
    }
  }

  /* freeze / frozen? on hashes: use the GC-header frozen bit */
  if (recv >= 0 && argc == 0 && ty_is_hash(comp_ntype(c, recv))) {
    if (!strcmp(name, "freeze")) {
      buf_puts(b, "sp_gc_freeze("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (!strcmp(name, "frozen?")) {
      buf_puts(b, "sp_gc_is_frozen("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
  }

  /* frozen? on numeric/symbol scalars: always frozen in Ruby semantics.
     TY_STRING is excluded -- dup/String.new produce unfrozen strings so
     string frozen-ness cannot be determined statically. */
  if (recv >= 0 && argc == 0 && !strcmp(name, "frozen?")) {
    TyKind frt = comp_ntype(c, recv);
    if (frt == TY_INT || frt == TY_FLOAT || frt == TY_SYMBOL || frt == TY_BOOL || frt == TY_NIL) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 1)");
      return;
    }
  }

  /* identity methods -> the receiver itself */
  if (recv >= 0 &&
      (!strcmp(name, "freeze") || !strcmp(name, "itself") ||
       !strcmp(name, "dup") || !strcmp(name, "clone"))) {
    int args = nt_ref(nt, id, "arguments");
    int argc0 = 0; if (args >= 0) nt_arr(nt, args, "arguments", &argc0);
    /* hash dup/clone requires a deep copy -- skip the identity shortcut */
    if (argc0 == 0 && !ty_is_hash(recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN)) { emit_expr(c, recv, b); return; }
  }

  /* then / yield_self: pass receiver to block, return block result */
  if (recv >= 0 && (!strcmp(name, "then") || !strcmp(name, "yield_self"))) {
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0) {
      TyKind rtype = infer_type(c, recv);
      const char *bp0 = block_param_name(c, blk, 0); if (bp0) bp0 = rename_local(bp0);
      int blk_body = nt_ref(nt, blk, "body");
      int then_bn = 0; const int *then_bb = blk_body >= 0 ? nt_arr(nt, blk_body, "body", &then_bn) : NULL;
      if (then_bn >= 1) {
        Scope *tsc = bp0 ? comp_scope_of(c, blk) : NULL;
        LocalVar *tlv0 = (tsc && bp0) ? scope_local(tsc, bp0) : NULL;
        TyKind tsaved0 = tlv0 ? tlv0->type : TY_UNKNOWN;
        int use_shadow_th = tlv0 && tlv0->type != rtype && rtype != TY_UNKNOWN;
        /* Pin block param type early so body_ty is computed with correct cache */
        if (use_shadow_th && tlv0) {
          tlv0->type = rtype;
          for (int j = 0; j < then_bn; j++) infer_type(c, then_bb[j]);
        }
        TyKind body_ty = infer_type(c, then_bb[then_bn - 1]);
        int tr = ++g_tmp, tres = ++g_tmp;
        Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
        emit_indent(g_pre, g_indent); emit_ctype(c, rtype, g_pre);
        buf_printf(g_pre, " _t%d = %s;\n", tr, rb.p ? rb.p : ""); free(rb.p);
        /* Declare tres at outer scope so it is visible after any shadow block */
        emit_indent(g_pre, g_indent); emit_ctype(c, body_ty, g_pre);
        buf_printf(g_pre, " _t%d;\n", tres);
        int bodyIndent = g_indent;
        if (use_shadow_th) {
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "{\n");
          bodyIndent = g_indent + 1;
          emit_indent(g_pre, bodyIndent); emit_ctype(c, rtype, g_pre);
          buf_printf(g_pre, " lv_%s = _t%d;\n", bp0, tr);
        }
        else if (bp0) {
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "lv_%s = _t%d;\n", bp0, tr);
        }
        for (int j = 0; j < then_bn - 1; j++) emit_stmt(c, then_bb[j], g_pre, bodyIndent);
        int save_ind = g_indent; g_indent = bodyIndent;
        Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, then_bb[then_bn - 1], &vb);
        g_indent = save_ind;
        emit_indent(g_pre, bodyIndent); buf_printf(g_pre, "_t%d = %s;\n", tres, vb.p ? vb.p : "0"); free(vb.p);
        if (use_shadow_th) { emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n"); }
        if (use_shadow_th && tlv0) tlv0->type = tsaved0;
        buf_printf(b, "_t%d", tres);
        return;
      }
    }
  }

  /* implicit-self call inside an instance method */
  if (recv < 0) {
    Scope *self = comp_scope_of(c, id);
    if (self->class_id >= 0) {
      if (comp_reader_in_chain(c, self->class_id, name, NULL)) {
        buf_printf(b, "%s->iv_%s", g_self, name);
        return;
      }
      int mi = comp_method_in_chain(c, self->class_id, name, NULL);
      if (mi >= 0) {
        emit_dispatch(c, self->class_id, name, g_self, nt_ref(nt, id, "arguments"), b);
        return;
      }
    }
  }

  /* self.class.new(args) in a leaf-class instance method -> construct the
     enclosing class statically (no subclass can shadow it at runtime). */
  if (recv >= 0 && !strcmp(name, "new") && nt_type(nt, recv) &&
      !strcmp(nt_type(nt, recv), "CallNode") && nt_str(nt, recv, "name") &&
      !strcmp(nt_str(nt, recv, "name"), "class")) {
    Scope *self = comp_scope_of(c, id);
    int cid = self ? self->class_id : -1;
    int has_sub = 0;
    for (int j = 0; cid >= 0 && j < c->nclasses; j++) if (c->classes[j].parent == cid) { has_sub = 1; break; }
    if (cid >= 0 && !has_sub) {
      buf_printf(b, "sp_%s_new(", c->classes[cid].name);
      for (int a = 0; a < argc; a++) { if (a) buf_puts(b, ", "); emit_expr(c, argv[a], b); }
      buf_puts(b, ")");
      return;
    }
  }

  /* namespaced class M::Sub.new -> sp_<Sub>_new(args) (resolve by final name) */
  if (recv >= 0 && !strcmp(name, "new") && nt_type(nt, recv) &&
      !strcmp(nt_type(nt, recv), "ConstantPathNode")) {
    const char *cn = nt_str(nt, recv, "name");
    int ci = cn ? comp_class_index(c, cn) : -1;
    if (ci >= 0 && !c->classes[ci].is_struct) {
      buf_printf(b, "sp_%s_new(", c->classes[ci].name);
      int initm = comp_method_in_chain(c, ci, "initialize", NULL);
      if (initm >= 0) emit_args_filled(c, initm, nt_ref(nt, id, "arguments"), "", b);
      buf_puts(b, ")");
      return;
    }
  }

  /* Class.new(args) -> sp_<Class>_new(args) */
  if (recv >= 0 && !strcmp(name, "new")) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ConstantReadNode")) {
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      if (ci >= 0 && c->classes[ci].is_struct) {
        /* Struct.new members: positional args, or keyword args mapping each
           member by name; each coerced to the member ivar type. */
        ClassInfo *cls = &c->classes[ci];
        int kwh = (argc == 1 && nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "KeywordHashNode")) ? argv[0] : -1;
        buf_printf(b, "sp_%s_new(", cls->name);
        for (int a = 0; a < cls->nivars; a++) {
          if (a) buf_puts(b, ", ");
          int vnode = -1;
          if (kwh >= 0) vnode = struct_kwarg_value(c, kwh, cls->ivars[a] + 1);
          else if (a < argc) vnode = argv[a];
          if (vnode >= 0) {
            if (cls->ivar_types[a] == TY_POLY && comp_ntype(c, vnode) != TY_POLY) emit_boxed(c, vnode, b);
            else emit_expr(c, vnode, b);
          }
          else buf_puts(b, default_value(cls->ivar_types[a]));
        }
        buf_puts(b, ")");
        return;
      }
      if (ci >= 0) {
        buf_printf(b, "sp_%s_new(", c->classes[ci].name);
        int initm = comp_method_in_chain(c, ci, "initialize", NULL);
        if (initm >= 0) emit_args_filled(c, initm, nt_ref(nt, id, "arguments"), "", b);
        buf_puts(b, ")");
        return;
      }
      const char *cn = nt_str(nt, recv, "name");
      if (cn && !strcmp(cn, "String")) {
        /* String.new / String.new(s) */
        if (argc == 1) emit_expr(c, argv[0], b);
        else buf_puts(b, "(&(\"\\xff\")[1])");
        return;
      }
      if (cn && !strcmp(cn, "StringIO")) {
        if (argc == 0) buf_puts(b, "sp_StringIO_new()");
        else if (argc == 1) { buf_puts(b, "sp_StringIO_new_s("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else { buf_puts(b, "sp_StringIO_new_sm("); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
        return;
      }
      if (cn && !strcmp(cn, "StringScanner") && argc == 1) {
        buf_puts(b, "sp_StringScanner_new("); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (cn && !strcmp(cn, "Array") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        buf_puts(b, "sp_PolyArray_new()"); return;
      }
      if (cn && !strcmp(cn, "Array") && argc == 1 && nt_ref(nt, id, "block") < 0) {
        /* Array.new(n) -> PolyArray of n nils */
        int tn = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
        Buf nb; memset(&nb, 0, sizeof nb); emit_expr(c, argv[0], &nb);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "mrb_int _t%d = ", tn); buf_puts(g_pre, nb.p ? nb.p : "0"); buf_puts(g_pre, ";\n");
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new();\n", tr);
        emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tr);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) sp_PolyArray_push(_t%d, sp_box_nil());\n",
                   ti, ti, tn, ti, tr);
        free(nb.p);
        buf_printf(b, "_t%d", tr); return;
      }
      if (cn && !strcmp(cn, "Array") && nt_ref(nt, id, "block") >= 0) {
        /* Array.new(n) { |i| body } / Array.new(0) { body } */
        int blk = nt_ref(nt, id, "block");
        TyKind at = comp_ntype(c, id);
        const char *k = (at == TY_POLY_ARRAY) ? "Poly" : array_kind(at);
        if (!k) k = "Poly";
        int tn = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
        int bbody = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
        const char *ip = block_param_name(c, blk, 0);
        const char *irn = ip ? rename_local(ip) : NULL;
        Buf nb; memset(&nb, 0, sizeof nb);
        if (argc >= 1) emit_expr(c, argv[0], &nb);
        emit_indent(g_pre, g_indent);
        if (argc >= 1) { buf_printf(g_pre, "mrb_int _t%d = ", tn); buf_puts(g_pre, nb.p ? nb.p : "0"); buf_puts(g_pre, ";\n"); }
        else { buf_printf(g_pre, "mrb_int _t%d = 0;\n", tn); }
        free(nb.p);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", k, tr, k);
        emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tr);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) {\n", ti, ti, tn, ti);
        g_indent++;
        if (irn) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int lv_%s = _t%d;\n", irn, ti); }
        if (bn > 0 && bb) {
          TyKind elem_t = ty_array_elem(at);
          Buf vb; memset(&vb, 0, sizeof vb);
          for (int bi = 0; bi < bn - 1; bi++) {
            Buf sb; memset(&sb, 0, sizeof sb);
            emit_expr(c, bb[bi], &sb);
            emit_indent(g_pre, g_indent); buf_puts(g_pre, sb.p ? sb.p : ""); buf_puts(g_pre, ";\n"); free(sb.p);
          }
          emit_expr(c, bb[bn - 1], &vb);
          emit_indent(g_pre, g_indent);
          if (!strcmp(k, "Poly")) {
            buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", tr);
            TyKind vt = comp_ntype(c, bb[bn - 1]);
            if (vt != TY_POLY) emit_boxed_text(c, vt, vb.p ? vb.p : "sp_box_nil()", g_pre);
            else buf_puts(g_pre, vb.p ? vb.p : "sp_box_nil()");
            buf_puts(g_pre, ");\n");
          }
          else { buf_printf(g_pre, "sp_%sArray_push(_t%d, %s);\n", k, tr, vb.p ? vb.p : ""); }
          free(vb.p);
        }
        g_indent--;
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        buf_printf(b, "_t%d", tr);
        return;
      }
      if (cn && !strcmp(cn, "Array") && argc == 2) {
        /* Array.new(n, v) -> n copies of v */
        TyKind at = comp_ntype(c, id);
        const char *k = (at == TY_POLY_ARRAY) ? "Poly" : array_kind(at);
        if (k) {
          int tn = ++g_tmp, tv = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
          Buf nb; memset(&nb, 0, sizeof nb); emit_expr(c, argv[0], &nb);
          Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, argv[1], &vb);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = ", tn); buf_puts(g_pre, nb.p ? nb.p : ""); buf_puts(g_pre, ";\n");
          emit_indent(g_pre, g_indent);
          if (at == TY_POLY_ARRAY) {
            buf_printf(g_pre, "sp_RbVal _t%d = ", tv);
            TyKind fvt = comp_ntype(c, argv[1]);
            if (fvt != TY_POLY) emit_boxed_text(c, fvt, vb.p ? vb.p : "sp_box_nil()", g_pre);
            else buf_puts(g_pre, vb.p ? vb.p : "sp_box_nil()");
          }
          else {
            emit_ctype(c, ty_array_elem(at), g_pre);
            buf_printf(g_pre, " _t%d = ", tv); buf_puts(g_pre, vb.p ? vb.p : "");
          }
          buf_puts(g_pre, ";\n");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", k, tr, k);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tr);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) sp_%sArray_push(_t%d, _t%d);\n",
                     ti, ti, tn, ti, k, tr, tv);
          free(nb.p); free(vb.p);
          buf_printf(b, "_t%d", tr);
          return;
        }
      }
    }
  }

  /* StringIO.open(args) { |io| body } -> run the block with a fresh IO,
     return the block's value, then close. */
  if (recv >= 0 && !strcmp(name, "open") && nt_type(nt, recv) &&
      !strcmp(nt_type(nt, recv), "ConstantReadNode") && nt_str(nt, recv, "name") &&
      !strcmp(nt_str(nt, recv, "name"), "StringIO")) {
    int block = nt_ref(nt, id, "block");
    if (block < 0) {
      /* no block: behaves like StringIO.new */
      if (argc == 0) buf_puts(b, "sp_StringIO_new()");
      else if (argc == 1) { buf_puts(b, "sp_StringIO_new_s("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else { buf_puts(b, "sp_StringIO_new_sm("); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      return;
    }
    const char *fp = block_param_name(c, block, 0); if (fp) fp = rename_local(fp);
    int bbody = nt_ref(nt, block, "body");
    int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
    TyKind res = comp_ntype(c, id);
    int rv = ++g_tmp;
    int scalar = is_scalar_ret(res);
    buf_puts(b, "({ ");
    if (fp) {
      buf_printf(b, "lv_%s = ", fp);
      if (argc == 0) buf_puts(b, "sp_StringIO_new()");
      else if (argc == 1) { buf_puts(b, "sp_StringIO_new_s("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else { buf_puts(b, "sp_StringIO_new_sm("); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      buf_puts(b, "; ");
    }
    /* leading statements first, then capture the last as the value */
    for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], b, 0);
    if (scalar && bn > 0) {
      emit_ctype(c, res, b); buf_printf(b, " _t%d = ", rv);
      if (res == TY_POLY && comp_ntype(c, bb[bn - 1]) != TY_POLY) emit_boxed(c, bb[bn - 1], b);
      else emit_expr(c, bb[bn - 1], b);
      buf_puts(b, "; ");
    }
    else if (bn > 0) emit_stmt(c, bb[bn - 1], b, 0);
    if (fp) buf_printf(b, "sp_StringIO_close(lv_%s); ", fp);
    buf_printf(b, "%s; })", scalar && bn > 0 ? ({ static char _tb[16]; snprintf(_tb, sizeof _tb, "_t%d", rv); _tb; }) : "0");
    return;
  }

  /* GC module methods */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "GC")) {
    if (!strcmp(name, "start") && argc == 0) { buf_puts(b, "0LL"); return; }
    if (!strcmp(name, "compact") && argc == 0) { buf_puts(b, "0LL"); return; }
    if (!strcmp(name, "stat") && argc == 0) { buf_puts(b, "sp_box_nil()"); return; }
  }

  /* Process module methods */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Process")) {
    if (!strcmp(name, "pid") && argc == 0) { buf_puts(b, "((mrb_int)getpid())"); return; }
    if (!strcmp(name, "ppid") && argc == 0) { buf_puts(b, "sp_process_ppid()"); return; }
  }

  /* Integer.sqrt(n) -> integer square root (exact, Newton's method) */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Integer") &&
      !strcmp(name, "sqrt") && argc == 1) {
    buf_puts(b, "sp_int_sqrt("); emit_expr(c, argv[0], b); buf_puts(b, ")");
    return;
  }

  /* Math module functions -> C math.h equivalents */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Math")) {
    /* 1-arg functions */
    const char *cfn = NULL;
    if      (!strcmp(name, "sin"))   cfn = "sin";
    else if (!strcmp(name, "cos"))   cfn = "cos";
    else if (!strcmp(name, "tan"))   cfn = "tan";
    else if (!strcmp(name, "asin"))  cfn = "asin";
    else if (!strcmp(name, "acos"))  cfn = "acos";
    else if (!strcmp(name, "atan"))  cfn = "atan";
    else if (!strcmp(name, "sinh"))  cfn = "sinh";
    else if (!strcmp(name, "cosh"))  cfn = "cosh";
    else if (!strcmp(name, "tanh"))  cfn = "tanh";
    else if (!strcmp(name, "asinh")) cfn = "asinh";
    else if (!strcmp(name, "acosh")) cfn = "acosh";
    else if (!strcmp(name, "atanh")) cfn = "atanh";
    else if (!strcmp(name, "exp"))   cfn = "exp";
    else if (!strcmp(name, "sqrt"))  cfn = "sqrt";
    else if (!strcmp(name, "cbrt"))  cfn = "cbrt";
    else if (!strcmp(name, "erf"))   cfn = "erf";
    else if (!strcmp(name, "erfc"))  cfn = "erfc";
    if (cfn && argc == 1) {
      TyKind a0t = comp_ntype(c, argv[0]);
      buf_printf(b, "%s(", cfn);
      if (a0t == TY_INT) buf_puts(b, "(double)");
      emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
    /* Math.log(x) or Math.log(x, base) */
    if (!strcmp(name, "log") && (argc == 1 || argc == 2)) {
      TyKind a0t = comp_ntype(c, argv[0]);
      if (argc == 1) {
        buf_puts(b, "log(");
        if (a0t == TY_INT) buf_puts(b, "(double)");
        emit_expr(c, argv[0], b);
        buf_puts(b, ")");
      }
      else {
        TyKind a1t = comp_ntype(c, argv[1]);
        int t0 = ++g_tmp, t1 = ++g_tmp;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "double _t%d = ", t0);
        if (a0t == TY_INT) buf_puts(g_pre, "(double)");
        emit_expr(c, argv[0], g_pre); buf_puts(g_pre, ";\n");
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "double _t%d = ", t1);
        if (a1t == TY_INT) buf_puts(g_pre, "(double)");
        emit_expr(c, argv[1], g_pre); buf_puts(g_pre, ";\n");
        buf_printf(b, "(log(_t%d) / log(_t%d))", t0, t1);
      }
      return;
    }
    /* Math.log2(x), Math.log10(x) */
    if (!strcmp(name, "log2") && argc == 1) {
      TyKind a0t = comp_ntype(c, argv[0]);
      buf_puts(b, "log2(");
      if (a0t == TY_INT) buf_puts(b, "(double)");
      emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    if (!strcmp(name, "log10") && argc == 1) {
      TyKind a0t = comp_ntype(c, argv[0]);
      buf_puts(b, "log10(");
      if (a0t == TY_INT) buf_puts(b, "(double)");
      emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    /* Math.atan2(y, x), Math.hypot(x, y), Math.ldexp(x, e) */
    if ((!strcmp(name, "atan2") || !strcmp(name, "hypot")) && argc == 2) {
      TyKind a0t = comp_ntype(c, argv[0]);
      TyKind a1t = comp_ntype(c, argv[1]);
      buf_printf(b, "%s(", name);
      if (a0t == TY_INT) buf_puts(b, "(double)");
      emit_expr(c, argv[0], b); buf_puts(b, ", ");
      if (a1t == TY_INT) buf_puts(b, "(double)");
      emit_expr(c, argv[1], b); buf_puts(b, ")");
      return;
    }
    if (!strcmp(name, "ldexp") && argc == 2) {
      TyKind a0t = comp_ntype(c, argv[0]);
      buf_puts(b, "ldexp(");
      if (a0t == TY_INT) buf_puts(b, "(double)");
      emit_expr(c, argv[0], b); buf_puts(b, ", (int)");
      emit_expr(c, argv[1], b); buf_puts(b, ")");
      return;
    }
  }

  /* JSON.generate(x) / JSON.dump(x) -> serialize a boxed value */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "JSON") &&
      (!strcmp(name, "generate") || !strcmp(name, "dump")) && argc == 1) {
    TyKind at = comp_ntype(c, argv[0]);
    /* a Struct serializes as a JSON object of its members */
    if (ty_is_object(at) && c->classes[ty_object_class(at)].is_struct) {
      ClassInfo *cls = &c->classes[ty_object_class(at)];
      int ts = ++g_tmp;
      buf_printf(b, "({ sp_%s *_t%d = ", cls->name, ts); emit_expr(c, argv[0], b); buf_puts(b, "; sp_sprintf(\"{");
      for (int a = 0; a < cls->nivars; a++) {
        if (a) buf_puts(b, ",");
        buf_printf(b, "\\\"%s\\\":%%s", cls->ivars[a] + 1);  /* member name, sans @ */
      }
      buf_puts(b, "}\"");
      for (int a = 0; a < cls->nivars; a++) {
        TyKind mt = cls->ivar_types[a];
        const char *iv = cls->ivars[a] + 1;  /* field name, sans @ */
        buf_puts(b, ", ");
        if (mt == TY_INT) buf_printf(b, "(_t%d->iv_%s == SP_INT_NIL ? SPL(\"null\") : sp_int_to_s(_t%d->iv_%s))", ts, iv, ts, iv);
        else if (mt == TY_STRING) buf_printf(b, "(_t%d->iv_%s ? sp_json_str(_t%d->iv_%s) : SPL(\"null\"))", ts, iv, ts, iv);
        else if (mt == TY_FLOAT) buf_printf(b, "sp_float_to_s(_t%d->iv_%s)", ts, iv);
        else if (mt == TY_BOOL) buf_printf(b, "(_t%d->iv_%s ? SPL(\"true\") : SPL(\"false\"))", ts, iv);
        else if (mt == TY_POLY) buf_printf(b, "sp_json_val(_t%d->iv_%s)", ts, iv);
        else buf_puts(b, "SPL(\"null\")");
      }
      buf_printf(b, "); })");
      return;
    }
    if (!ty_is_object(at)) {  /* other user objects have no JSON serializer yet */
      buf_puts(b, "sp_json_val("); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return;
    }
  }

  /* Dir.exist? / Dir.exists? -> directory test */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Dir") &&
      (!strcmp(name, "exist?") || !strcmp(name, "exists?")) && argc == 1) {
    buf_puts(b, "sp_file_directory("); emit_expr(c, argv[0], b); buf_puts(b, ")");
    return;
  }

  /* File class methods -> runtime helpers (the runtime has long carried
     these; only the dispatch was missing). */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "File")) {
    if ((!strcmp(name, "basename") || !strcmp(name, "dirname") || !strcmp(name, "extname")) && argc == 1) {
      buf_printf(b, "sp_file_%s(", name); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (!strcmp(name, "read") && argc == 1) {
      buf_puts(b, "sp_file_read("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (!strcmp(name, "write") && argc == 2) {
      /* runtime write is void; Ruby returns the byte count */
      buf_puts(b, "({ const char *_wp = "); emit_expr(c, argv[0], b);
      buf_puts(b, "; const char *_wd = "); emit_expr(c, argv[1], b);
      buf_puts(b, "; sp_file_write(_wp, _wd); (mrb_int)sp_str_byte_len(_wd); })"); return;
    }
    if ((!strcmp(name, "exist?") || !strcmp(name, "exists?")) && argc == 1) {
      buf_puts(b, "sp_file_exist("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (!strcmp(name, "delete") && argc == 1) {
      buf_puts(b, "({ sp_file_delete("); emit_expr(c, argv[0], b); buf_puts(b, "); (mrb_int)1; })"); return;
    }
    if (!strcmp(name, "mtime") && argc == 1) {
      buf_puts(b, "sp_file_mtime("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (!strcmp(name, "expand_path") && (argc == 1 || argc == 2)) {
      buf_puts(b, "sp_file_expand_path("); emit_expr(c, argv[0], b); buf_puts(b, ", ");
      if (argc == 2) emit_expr(c, argv[1], b); else buf_puts(b, "(const char *)0");
      buf_puts(b, ")"); return;
    }
  }
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Dir")) {
    if (!strcmp(name, "pwd") && argc == 0) { buf_puts(b, "sp_dir_pwd()"); return; }
    if (!strcmp(name, "home") && argc == 0) { buf_puts(b, "sp_dir_home()"); return; }
    if (!strcmp(name, "glob") && argc == 1) {
      buf_puts(b, "sp_dir_glob("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if ((!strcmp(name, "mkdir") || !strcmp(name, "rmdir") || !strcmp(name, "chdir")) && argc >= 1) {
      buf_printf(b, "sp_dir_%s(", name); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
  }

  /* Time class constructors */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Time")) {
    if (!strcmp(name, "now") && argc == 0) { buf_puts(b, "sp_time_now()"); return; }
    if (!strcmp(name, "at") && argc == 1) {
      TyKind at = comp_ntype(c, argv[0]);
      buf_printf(b, "sp_time_at_%s(", at == TY_FLOAT ? "float" : "int");
      emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    if ((!strcmp(name, "local") || !strcmp(name, "mktime") ||
         !strcmp(name, "utc") || !strcmp(name, "gm")) && argc >= 1) {
      /* y[,mo,d,h,mi,s] -- missing trailing parts default (mo/d=1, rest 0) */
      int is_utc = (!strcmp(name, "utc") || !strcmp(name, "gm"));
      buf_printf(b, "sp_time_new%s(", is_utc ? "_utc" : "");
      for (int i = 0; i < 6; i++) {
        if (i) buf_puts(b, ", ");
        if (i < argc) emit_expr(c, argv[i], b);
        else buf_puts(b, (i == 1 || i == 2) ? "1" : "0");
      }
      buf_puts(b, ")");
      return;
    }
  }

  /* Class.cmethod(args) -> sp_<Class>_s_<method>(args) */
  if (recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ConstantReadNode")) {
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      int defcls = -1;
      int mi = ci >= 0 ? comp_cmethod_in_chain(c, ci, name, &defcls) : -1;
      if (mi >= 0) {
        buf_printf(b, "sp_%s_s_%s(", c->classes[defcls].name, mc(c->scopes[mi].name));
        emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), "", b);
        buf_puts(b, ")");
        return;
      }
    }
  }

  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  TyKind res = comp_ntype(c, id);

  /* regex literal match predicates (bool-returning, no MatchData/globals):
     /re/.match?(str[, pos])  and  str !~ /re/  and  str.match?(/re/[, pos]) */
  {
    int rre = re_lit_index(c, recv);
    if (rre >= 0 && (!strcmp(name, "match?") || !strcmp(name, "===")) && argc == 1) {
      /* /re/ === str and /re/.match?(str) both yield a match boolean */
      buf_printf(b, "sp_re_match_p(sp_re_pat_%d, ", rre); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    if (rre >= 0 && !strcmp(name, "match?") && argc == 2) {
      buf_printf(b, "sp_re_match_p_at(sp_re_pat_%d, ", rre); emit_expr(c, argv[0], b);
      buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      return;
    }
    /* /re/ =~ str -> match offset or nil (poly) */
    if (rre >= 0 && !strcmp(name, "=~") && argc == 1 && a0 == TY_STRING) {
      buf_printf(b, "sp_re_match_poly(sp_re_pat_%d, ", rre); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    /* /re/.source and /re/.options are compile-time constants of the literal */
    if (rre >= 0 && !strcmp(name, "source") && argc == 0) {
      emit_str_literal(b, nt_str(nt, recv, "unescaped")); return;
    }
    if (rre >= 0 && !strcmp(name, "options") && argc == 0) {
      int pf = (int)nt_int(nt, recv, "flags", 0);
      int opt = ((pf & 4) ? 1 : 0) | ((pf & 8) ? 2 : 0) | ((pf & 16) ? 4 : 0);
      buf_printf(b, "%d", opt); return;
    }
  }
  if (recv >= 0 && argc >= 1 && (!strcmp(name, "match?") || !strcmp(name, "!~") || !strcmp(name, "=~") || !strcmp(name, "match"))) {
    int are = re_lit_index(c, argv[0]);
    if (are >= 0 && !strcmp(name, "=~") && rt == TY_STRING) {
      buf_printf(b, "sp_re_match_poly(sp_re_pat_%d, ", are); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (are >= 0 && !strcmp(name, "!~")) {
      buf_printf(b, "(!sp_re_match_p(sp_re_pat_%d, ", are); emit_expr(c, recv, b); buf_puts(b, "))");
      return;
    }
    if (are >= 0 && !strcmp(name, "match?")) {
      if (argc == 1) { buf_printf(b, "sp_re_match_p(sp_re_pat_%d, ", are); emit_expr(c, recv, b); buf_puts(b, ")"); return; }
      buf_printf(b, "sp_str_re_match_p_at(sp_re_pat_%d, ", are); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      return;
    }
    if (are >= 0 && !strcmp(name, "match")) {
      if (argc == 1) {
        buf_printf(b, "sp_re_matchdata(sp_re_pat_%d, ", are); emit_expr(c, recv, b); buf_puts(b, ")");
      }
      else {
        buf_printf(b, "sp_re_matchdata_at(sp_re_pat_%d, ", are); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      return;
    }
  }
  /* /re/.match(str) and /re/.match(str, pos) */
  {
    int rre = re_lit_index(c, recv);
    if (rre >= 0 && !strcmp(name, "match") && (argc == 1 || argc == 2)) {
      if (argc == 1) {
        buf_printf(b, "sp_re_matchdata(sp_re_pat_%d, ", rre); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else {
        buf_printf(b, "sp_re_matchdata_at(sp_re_pat_%d, ", rre); emit_expr(c, argv[0], b);
        buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      return;
    }
  }

  /* General handler for regex-related calls where the pattern is an
     interpolated regex (/foo_#{x}/) or a TY_REGEX local variable.
     Covers match?, =~, !~, match, gsub, sub, scan, split as regex arg. */
  {
    /* Pattern from argument (str.match?(/dyn/), str =~ /dyn/, etc.) */
    if (recv >= 0 && argc >= 1) {
      const char *a0ty = nt_type(nt, argv[0]);
      int is_interp_arg = a0ty && !strcmp(a0ty, "InterpolatedRegularExpressionNode");
      int is_regex_lv_arg = !is_interp_arg && argc >= 1 && comp_ntype(c, argv[0]) == TY_REGEX
                            && nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "LocalVariableReadNode");
      if (is_interp_arg || is_regex_lv_arg) {
        Buf rp; memset(&rp, 0, sizeof rp);
        if (emit_regex_pat_to_buf(c, argv[0], &rp) && rp.p) {
          if (!strcmp(name, "match?") && argc == 1) {
            buf_printf(b, "sp_re_match_p(%s, ", rp.p); emit_expr(c, recv, b); buf_puts(b, ")");
            free(rp.p); return;
          }
          if (!strcmp(name, "=~") && rt == TY_STRING) {
            buf_printf(b, "sp_re_match_poly(%s, ", rp.p); emit_expr(c, recv, b); buf_puts(b, ")");
            free(rp.p); return;
          }
          if (!strcmp(name, "!~")) {
            buf_printf(b, "(!sp_re_match_p(%s, ", rp.p); emit_expr(c, recv, b); buf_puts(b, "))");
            free(rp.p); return;
          }
          if (!strcmp(name, "match") && argc == 1) {
            buf_printf(b, "sp_re_matchdata(%s, ", rp.p); emit_expr(c, recv, b); buf_puts(b, ")");
            free(rp.p); return;
          }
          free(rp.p);
        }
      }
    }
    /* Pattern from receiver (rx.match?(str), rx =~ str, etc.) */
    {
      const char *rty = recv >= 0 ? nt_type(nt, recv) : NULL;
      int is_interp_recv = rty && !strcmp(rty, "InterpolatedRegularExpressionNode");
      int is_regex_lv_recv = !is_interp_recv && recv >= 0 && comp_ntype(c, recv) == TY_REGEX;
      if (is_interp_recv || is_regex_lv_recv) {
        Buf rp; memset(&rp, 0, sizeof rp);
        if (emit_regex_pat_to_buf(c, recv, &rp) && rp.p) {
          if ((!strcmp(name, "match?") || !strcmp(name, "===")) && argc == 1) {
            buf_printf(b, "sp_re_match_p(%s, ", rp.p); emit_expr(c, argv[0], b); buf_puts(b, ")");
            free(rp.p); return;
          }
          if (!strcmp(name, "=~") && argc == 1) {
            if (a0 == TY_STRING) {
              buf_printf(b, "sp_re_match_poly(%s, ", rp.p); emit_expr(c, argv[0], b); buf_puts(b, ")");
            }
            else if (a0 == TY_POLY) {
              /* runtime type check: raise TypeError if not a string */
              int tv = ++g_tmp;
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "sp_RbVal _t%d = ", tv); emit_expr(c, argv[0], g_pre); buf_puts(g_pre, ";\n");
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "if (_t%d.tag != SP_TAG_STR) sp_raise_cls(\"TypeError\", \"no implicit conversion into String\");\n", tv);
              buf_printf(b, "sp_re_match_poly(%s, _t%d.v.s)", rp.p, tv);
            }
            else {
              /* statically known non-string: always raises TypeError */
              const char *tn = (a0 == TY_INT) ? "Integer" : (a0 == TY_FLOAT) ? "Float"
                             : (a0 == TY_BOOL) ? "true/false" : (a0 == TY_NIL) ? "NilClass" : "Object";
              buf_printf(b, "((void)(");
              emit_expr(c, argv[0], b);
              buf_printf(b, "), sp_raise_cls(\"TypeError\", \"no implicit conversion of %s into String\"), sp_box_nil())", tn);
            }
            free(rp.p); return;
          }
          if (!strcmp(name, "match") && (argc == 1 || argc == 2)) {
            if (argc == 1) { buf_printf(b, "sp_re_matchdata(%s, ", rp.p); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
            else { buf_printf(b, "sp_re_matchdata_at(%s, ", rp.p); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
            free(rp.p); return;
          }
          free(rp.p);
        }
      }
    }
  }

  /* String#% with an array argument: printf-style formatting. Any typed array
     is boxed to poly so a single format path handles mixed specs. */
  if (recv >= 0 && rt == TY_STRING && !strcmp(name, "%") && argc == 1) {
    TyKind at = a0;
    if (at == TY_POLY_ARRAY) {
      buf_puts(b, "sp_str_format_polyarr("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    const char *ak = array_kind(at);
    if (ak) {
      const char *kind = at == TY_STR_ARRAY ? "SP_BUILTIN_STR_ARRAY"
                       : at == TY_FLOAT_ARRAY ? "SP_BUILTIN_FLT_ARRAY" : "SP_BUILTIN_INT_ARRAY";
      buf_puts(b, "sp_str_format_polyarr("); emit_expr(c, recv, b);
      buf_puts(b, ", sp_typed_to_poly((void *)("); emit_expr(c, argv[0], b);
      buf_printf(b, "), %s))", kind);
      return;
    }
    /* a single non-array argument formats as a one-element array */
    if (at == TY_INT || at == TY_FLOAT || at == TY_STRING || at == TY_SYMBOL || at == TY_POLY) {
      buf_puts(b, "sp_str_format_polyarr("); emit_expr(c, recv, b);
      buf_puts(b, ", ({ sp_PolyArray *_fa = sp_PolyArray_new(); sp_PolyArray_push(_fa, ");
      emit_boxed(c, argv[0], b); buf_puts(b, "); _fa; }))");
      return;
    }
  }

  /* an empty array literal as a receiver: its node type is unknown (element
     type is usage-folded, but a bare literal has no usage). Handle the common
     methods directly against an empty (poly) array. */
  if (recv >= 0 && rt == TY_UNKNOWN) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ArrayNode")) {
      int en = 0; nt_arr(nt, recv, "elements", &en);
      if (en == 0) {
        if ((!strcmp(name, "length") || !strcmp(name, "size") || !strcmp(name, "count")) && argc == 0) { buf_puts(b, "0"); return; }
        if (!strcmp(name, "empty?") && argc == 0) { buf_puts(b, "1"); return; }
        if ((!strcmp(name, "first") || !strcmp(name, "last") ||
             !strcmp(name, "min") || !strcmp(name, "max") ||
             !strcmp(name, "pop") || !strcmp(name, "shift")) && argc == 0) { buf_puts(b, "SP_INT_NIL"); return; }
        if (!strcmp(name, "sample") && argc == 0) { buf_puts(b, "0"); return; }
        if ((!strcmp(name, "inspect") || !strcmp(name, "to_s")) && argc == 0) { buf_puts(b, "\"[]\""); return; }
        if ((!strcmp(name, "join") || !strcmp(name, "pack")) && argc <= 1) { buf_puts(b, "(&(\"\\xff\")[1])"); return; }
        if ((!strcmp(name, "union")) && argc == 0) { buf_puts(b, "sp_IntArray_new()"); return; }
        if ((!strcmp(name, "flatten") || !strcmp(name, "compact") || !strcmp(name, "uniq") ||
             !strcmp(name, "sort") || !strcmp(name, "reverse") || !strcmp(name, "dup") ||
             !strcmp(name, "clone") || !strcmp(name, "to_a")) && argc <= 1) {
          buf_puts(b, "sp_PolyArray_new()"); return;
        }
      }
    }
  }

  /* respond_to?(:m): compile-time approximation. A universal method set is
     always true; otherwise consult the receiver's class / class-method chain.
     Unknown primitive methods answer conservatively false. */
  if (!strcmp(name, "respond_to?") && recv >= 0 && argc >= 1) {
    const char *aty = nt_type(nt, argv[0]);
    const char *qm = NULL;
    if (aty && !strcmp(aty, "SymbolNode")) qm = nt_str(nt, argv[0], "value");
    else if (aty && !strcmp(aty, "StringNode")) qm = nt_str(nt, argv[0], "unescaped");
    if (qm) {
      static const char *const uni[] = {
        "to_s", "inspect", "class", "nil?", "dup", "clone", "freeze",
        "frozen?", "hash", "==", "!=", "equal?", "eql?", "object_id",
        "respond_to?", "is_a?", "kind_of?", "instance_of?", "itself",
        "tap", "then", "send", "===", NULL };
      int yes = 0, resolved = 0;
      for (int u = 0; uni[u]; u++) if (!strcmp(qm, uni[u])) { yes = resolved = 1; break; }
      if (!resolved) {
        const char *rty = nt_type(nt, recv);
        if (rty && !strcmp(rty, "ConstantReadNode")) {
          int ci = comp_class_index(c, nt_str(nt, recv, "name"));
          if (ci >= 0) {
            resolved = 1;
            yes = comp_cmethod_in_chain(c, ci, qm, NULL) >= 0;
            /* a module also responds to its def'd (module_function) methods */
            if (!yes) {
              int dn = c->classes[ci].def_node;
              const char *dt = dn >= 0 ? nt_type(nt, dn) : NULL;
              if (dt && !strcmp(dt, "ModuleNode")) yes = comp_method_in_chain(c, ci, qm, NULL) >= 0;
            }
          }
        }
        else if (ty_is_object(rt)) {
          int cid = ty_object_class(rt);
          resolved = 1;
          yes = comp_method_in_chain(c, cid, qm, NULL) >= 0 ||
                comp_reader_in_chain(c, cid, qm, NULL) ||
                comp_writer_in_chain(c, cid, qm, NULL);
        }
        /* a primitive/poly/unknown receiver with a non-universal method: we
           lack a per-type method table, so leave it to the fall-through
           rather than answer a possibly-wrong false. */
      }
      if (resolved) { buf_printf(b, "%d", yes); return; }
    }
  }

  /* Class.method_defined?(:m[, inherit]): compile-time decided from the
     class's recorded method table (instance methods + attr readers/writers).
     inherit=false restricts the lookup to the receiver's own definitions. */
  if (!strcmp(name, "method_defined?") && recv >= 0 && argc >= 1 &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode")) {
    const char *aty = nt_type(nt, argv[0]);
    const char *qm = NULL;
    if (aty && !strcmp(aty, "SymbolNode")) qm = nt_str(nt, argv[0], "value");
    else if (aty && !strcmp(aty, "StringNode")) qm = nt_str(nt, argv[0], "content");
    int ci = comp_class_index(c, nt_str(nt, recv, "name"));
    if (qm && ci >= 0) {
      int inherit = 1;
      if (argc >= 2) {
        const char *it = nt_type(nt, argv[1]);
        if (it && !strcmp(it, "FalseNode")) inherit = 0;
      }
      /* a writer query (`m=`) consults the writer table under its base name */
      size_t ln = strlen(qm);
      int is_setter = ln > 0 && qm[ln - 1] == '=';
      char base[256];
      base[0] = '\0';
      if (is_setter && ln - 1 < sizeof base) { memcpy(base, qm, ln - 1); base[ln - 1] = '\0'; }
      int parent = c->classes[ci].parent;
      int mc = -1;
      int mi = comp_method_in_chain(c, ci, qm, &mc);
      int yes;
      if (inherit) {
        yes = mi >= 0 || comp_reader_in_chain(c, ci, qm, NULL) ||
              (is_setter && comp_writer_in_chain(c, ci, base, NULL));
      }
      else {
        /* attr readers/writers are flattened into descendants at analyze
           time, so "own" means present here but not in the parent chain */
        int rd_own = comp_is_reader(&c->classes[ci], qm) &&
                     (parent < 0 || !comp_reader_in_chain(c, parent, qm, NULL));
        int wr_own = is_setter && comp_is_writer(&c->classes[ci], base) &&
                     (parent < 0 || !comp_writer_in_chain(c, parent, base, NULL));
        yes = (mi >= 0 && mc == ci) || rd_own || wr_own;
      }
      buf_printf(b, "%d", yes);
      return;
    }
  }

  /* Class.const_defined?(:K): compile-time presence check. Constants are
     recorded in a flat namespace, so this consults the global const and class
     tables rather than the receiver's own constants. */
  if (!strcmp(name, "const_defined?") && recv >= 0 && argc >= 1 &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode")) {
    const char *aty = nt_type(nt, argv[0]);
    const char *qm = NULL;
    if (aty && !strcmp(aty, "SymbolNode")) qm = nt_str(nt, argv[0], "value");
    else if (aty && !strcmp(aty, "StringNode")) qm = nt_str(nt, argv[0], "content");
    if (qm) {
      int yes = comp_const(c, qm) != NULL || comp_class_index(c, qm) >= 0;
      buf_printf(b, "%d", yes);
      return;
    }
  }

  if ((!strcmp(name, "-@") || !strcmp(name, "+@")) && recv >= 0 && argc == 0 && !ty_is_object(rt)) {
    if (rt == TY_POLY) {
      if (name[0] == '-') { buf_puts(b, "sp_poly_neg("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      else { emit_expr(c, recv, b); }  /* +@ is identity on poly */
    }
    else { buf_puts(b, name[0] == '-' ? "(-" : "(+"); emit_expr(c, recv, b); buf_puts(b, ")"); }
    return;
  }
  if (!strcmp(name, "!") && recv >= 0 && argc == 0) {
    /* Ruby truthiness: only nil and false are falsy */
    if (rt == TY_BOOL) { buf_puts(b, "(!"); emit_expr(c, recv, b); buf_puts(b, ")"); }
    else if (rt == TY_NIL) { buf_puts(b, "1"); }
    else { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), 0)"); }  /* truthy -> false */
    return;
  }

  /* poly arithmetic: sp_poly_<op>(boxed, boxed) -> a (poly) result */
  if (recv >= 0 && argc == 1 && (rt == TY_POLY || a0 == TY_POLY)) {
    const char *pfn = NULL;
    if (!strcmp(name, "+")) pfn = "sp_poly_add";
    else if (!strcmp(name, "-")) pfn = "sp_poly_sub";
    else if (!strcmp(name, "*")) pfn = "sp_poly_mul";
    else if (!strcmp(name, "/")) pfn = "sp_poly_div";
    else if (!strcmp(name, "%")) pfn = "sp_poly_mod";
    else if (!strcmp(name, "**")) pfn = "sp_poly_pow";
    if (pfn) {
      buf_printf(b, "%s(", pfn); emit_boxed(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    const char *cfn = NULL;
    if (!strcmp(name, "<")) cfn = "sp_poly_lt";
    else if (!strcmp(name, ">")) cfn = "sp_poly_gt";
    else if (!strcmp(name, "<=")) cfn = "sp_poly_le";
    else if (!strcmp(name, ">=")) cfn = "sp_poly_ge";
    if (cfn) {
      buf_printf(b, "%s(", cfn); emit_boxed(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return;
    }
  }

  /* Array#* (join): arr * sep_str  ->  elements joined by separator string. */
  if (recv >= 0 && argc == 1 && !strcmp(name, "*") && (ty_is_array(rt) || rt == TY_POLY_ARRAY) &&
      comp_ntype(c, argv[0]) == TY_STRING) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) k = "Str";
    buf_printf(b, "sp_%sArray_join(", k); emit_expr(c, recv, b);
    buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
    return;
  }

  /* Array#* (repeat): arr * n  ->  new array with elements repeated n times. */
  if (recv >= 0 && argc == 1 && !strcmp(name, "*") && (ty_is_array(rt) || rt == TY_POLY_ARRAY) &&
      comp_ntype(c, argv[0]) == TY_INT) {
    int ta = ++g_tmp, tn = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp;
    if (rt == TY_POLY_ARRAY) {
      buf_printf(b, "({ sp_PolyArray *_t%d = ", ta); emit_expr(c, recv, b);
      buf_printf(b, "; mrb_int _t%d = ", tn); emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                    " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)"
                    " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                    " sp_PolyArray_push(_t%d, _t%d->data[_t%d]); _t%d; })",
                 tr, tr,
                 ti, ti, tn, ti,
                 tj, tj, ta, tj,
                 tr, ta, tj, tr);
    }
    else {
      const char *k = array_kind(rt);
      /* Only IntArray has a start offset; Float/StrArray index directly. */
      int has_start = (rt == TY_INT_ARRAY);
      buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
      buf_printf(b, "; mrb_int _t%d = ", tn); emit_expr(c, argv[0], b);
      if (has_start) {
        buf_printf(b, "; sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);"
                      " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)"
                      " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                      " sp_%sArray_push(_t%d, _t%d->data[_t%d->start + _t%d]); _t%d; })",
                   k, tr, k, tr,
                   ti, ti, tn, ti,
                   tj, tj, ta, tj,
                   k, tr, ta, ta, tj, tr);
      }
      else {
        buf_printf(b, "; sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);"
                      " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)"
                      " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                      " sp_%sArray_push(_t%d, _t%d->data[_t%d]); _t%d; })",
                   k, tr, k, tr,
                   ti, ti, tn, ti,
                   tj, tj, ta, tj,
                   k, tr, ta, tj, tr);
      }
    }
    return;
  }

  if (recv >= 0 && argc == 1 && int_arith_fn(name) && !ty_is_object(rt) && !ty_is_array(rt)) {
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

  if (recv >= 0 && argc == 1 && !strcmp(name, "<=>")) {
    /* Re-infer when stale cache has TY_POLY (e.g. block params temporarily pinned to element type). */
    TyKind lrt = (rt == TY_POLY || rt == TY_UNKNOWN) ? infer_type(c, recv) : rt;
    TyKind at = comp_ntype(c, argv[0]);
    TyKind lat = (at == TY_POLY || at == TY_UNKNOWN) ? infer_type(c, argv[0]) : at;
    if (ty_is_numeric(lrt) && ty_is_numeric(lat)) {
      int ta = ++g_tmp, tb = ++g_tmp;
      buf_puts(b, "({ "); emit_ctype(c, lrt, b); buf_printf(b, " _t%d = ", ta); emit_expr(c, recv, b);
      buf_puts(b, "; "); emit_ctype(c, lat, b); buf_printf(b, " _t%d = ", tb); emit_expr(c, argv[0], b);
      buf_printf(b, "; (_t%d > _t%d) - (_t%d < _t%d); })", ta, tb, ta, tb);
      return;
    }
    if (lrt == TY_STRING && lat == TY_STRING) {
      int tc = ++g_tmp;
      buf_printf(b, "({ int _t%d = strcmp(", tc); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
      buf_printf(b, "); (_t%d > 0) - (_t%d < 0); })", tc, tc);
      return;
    }
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
    /* Comparable: object with a user `<=>` method but no direct `<` etc. */
    if (ty_is_object(rt)) {
      int cid4 = ty_object_class(rt);
      if (comp_method_in_chain(c, cid4, name, NULL) < 0 &&
          comp_method_in_chain(c, cid4, "<=>", NULL) >= 0) {
        char selfptr[64];
        const char *rtyp = nt_type(nt, recv);
        if (rtyp && (!strcmp(rtyp, "LocalVariableReadNode") ||
                     !strcmp(rtyp, "InstanceVariableReadNode") ||
                     !strcmp(rtyp, "SelfNode"))) {
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          snprintf(selfptr, sizeof selfptr, "%s", rb.p ? rb.p : "");
          free(rb.p);
        }
        else {
          int t4 = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          emit_indent(g_pre, g_indent);
          emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", t4, rb.p ? rb.p : "");
          free(rb.p);
          snprintf(selfptr, sizeof selfptr, "_t%d", t4);
        }
        buf_puts(b, "(");
        emit_dispatch(c, cid4, "<=>", selfptr, nt_ref(nt, id, "arguments"), b);
        buf_printf(b, " %s 0)", name);
        return;
      }
    }
    unsupported(c, id, "comparison");
  }

  /* concrete builtin receiver: is_a?/kind_of?/instance_of? is known at compile
     time (evaluate the receiver for side effects, then yield the constant). */
  if (recv >= 0 && argc == 1 &&
      (!strcmp(name, "is_a?") || !strcmp(name, "kind_of?") || !strcmp(name, "instance_of?")) &&
      nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "ConstantReadNode")) {
    int yes = ty_matches_class(rt, nt_str(nt, argv[0], "name"), !strcmp(name, "instance_of?"));
    if (yes >= 0) { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), %d)", yes); return; }
  }

  /* poly.is_a?(Class) / kind_of?: runtime tag/cls_id check */
  if (recv >= 0 && rt == TY_POLY && argc == 1 &&
      (!strcmp(name, "is_a?") || !strcmp(name, "kind_of?") || !strcmp(name, "instance_of?"))) {
    const char *cty = nt_type(nt, argv[0]);
    const char *cn = cty && !strcmp(cty, "ConstantReadNode") ? nt_str(nt, argv[0], "name") : NULL;
    if (cn) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", t); emit_expr(c, recv, b); buf_printf(b, "; ");
      char v[32]; snprintf(v, sizeof v, "_t%d", t);
      if (!strcmp(cn, "Integer") || !strcmp(cn, "Fixnum")) buf_printf(b, "%s.tag == SP_TAG_INT", v);
      else if (!strcmp(cn, "String"))   buf_printf(b, "%s.tag == SP_TAG_STR", v);
      else if (!strcmp(cn, "Float"))    buf_printf(b, "%s.tag == SP_TAG_FLT", v);
      else if (!strcmp(cn, "Symbol"))   buf_printf(b, "%s.tag == SP_TAG_SYM", v);
      else if (!strcmp(cn, "NilClass")) buf_printf(b, "%s.tag == SP_TAG_NIL", v);
      else if (!strcmp(cn, "TrueClass"))  buf_printf(b, "(%s.tag == SP_TAG_BOOL && %s.v.b)", v, v);
      else if (!strcmp(cn, "FalseClass")) buf_printf(b, "(%s.tag == SP_TAG_BOOL && !%s.v.b)", v, v);
      else if (!strcmp(cn, "Numeric"))  buf_printf(b, "(%s.tag == SP_TAG_INT || %s.tag == SP_TAG_FLT)", v, v);
      else if (!strcmp(cn, "Array"))    buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id <= -1 && %s.cls_id >= -12)", v, v, v);
      else if (!strcmp(cn, "Hash"))     buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id <= -13 && %s.cls_id >= -20)", v, v, v);
      else {
        int cid = comp_class_index(c, cn);
        int exact = !strcmp(name, "instance_of?");
        if (cid >= 0) {
          buf_printf(b, "(%s.tag == SP_TAG_OBJ && (", v);
          int first = 1;
          for (int k = 0; k < c->nclasses; k++)
            if (k == cid || (!exact && is_descendant(c, k, cid))) {
              buf_printf(b, "%s%s.cls_id == %d", first ? "" : " || ", v, k); first = 0;
            }
          if (first) buf_puts(b, "0");
          buf_puts(b, "))");
        }
        else buf_puts(b, "0");
      }
      buf_puts(b, "; })");
      return;
    }
  }

  /* nil receiver: nil.inspect -> "nil", nil.to_s -> "", nil.nil? -> true.
     Evaluate the receiver for side effects, then yield the constant. */
  if (recv >= 0 && rt == TY_NIL && argc == 0) {
    if (!strcmp(name, "inspect")) { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), SPL(\"nil\"))"); return; }
    if (!strcmp(name, "to_s"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), SPL(\"\"))"); return; }
    if (!strcmp(name, "nil?"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 1)"); return; }
  }

  /* <str>.encoding.name -> the encoding name (the poly encoding's to_s) */
  if (!strcmp(name, "name") && argc == 0 && recv >= 0 && comp_ntype(c, recv) == TY_POLY &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "encoding")) {
    buf_puts(b, "sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
  }

  /* poly receiver: nil? / conversions / a few type-agnostic queries */
  if (recv >= 0 && rt == TY_POLY && argc == 0) {
    if (!strcmp(name, "nil?")) { buf_puts(b, "sp_poly_nil_p("); emit_expr(c, recv, b); buf_puts(b, ")"); return; }
    if (!strcmp(name, "to_s") || !strcmp(name, "inspect")) {
      buf_printf(b, "%s(", !strcmp(name, "to_s") ? "sp_poly_to_s" : "sp_poly_inspect");
      emit_expr(c, recv, b); buf_puts(b, ")"); return;
    }
    if (!strcmp(name, "to_i")) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, recv, b); buf_puts(b, ")"); return; }
    if (!strcmp(name, "to_f")) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, recv, b); buf_puts(b, ")"); return; }
  }
  /* poly receiver: []= with symbol, string, int, or poly key -> runtime dispatch */
  if (recv >= 0 && rt == TY_POLY && !strcmp(name, "[]=") && argc == 2) {
    TyKind at = comp_ntype(c, argv[0]);
    TyKind vt = comp_ntype(c, argv[1]);
    int tv = ++g_tmp;
    buf_puts(b, "({ sp_RbVal _t"); buf_printf(b, "%d = ", tv); emit_boxed(c, argv[1], b);
    buf_puts(b, "; ");
    if (at == TY_STRING) {
      buf_printf(b, "sp_poly_set_str("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_expr(c, argv[0], b);
    }
    else if (at == TY_SYMBOL) {
      buf_printf(b, "sp_poly_set_sym("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_expr(c, argv[0], b);
    }
    else if (at == TY_INT) {
      buf_printf(b, "sp_poly_arr_set("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_expr(c, argv[0], b);
    }
    else {
      buf_printf(b, "sp_poly_set_poly("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_boxed(c, argv[0], b);
    }
    buf_printf(b, ", _t%d); _t%d; })", tv, tv);
    (void)vt;
    return;
  }
  /* poly receiver: [] with symbol or string key -> runtime dispatch */
  if (recv >= 0 && rt == TY_POLY && !strcmp(name, "[]") && argc == 1) {
    TyKind at = comp_ntype(c, argv[0]);
    if (at == TY_SYMBOL) {
      buf_puts(b, "sp_poly_get_sym("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    if (at == TY_STRING) {
      buf_puts(b, "sp_poly_get_str("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    if (at == TY_INT) {
      buf_puts(b, "sp_poly_arr_get("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
  }
  /* poly receiver: join */
  if (recv >= 0 && rt == TY_POLY && !strcmp(name, "join")) {
    buf_puts(b, "sp_poly_join("); emit_expr(c, recv, b);
    buf_puts(b, ", "); if (argc >= 1) emit_expr(c, argv[0], b); else buf_puts(b, "\"\"");
    buf_puts(b, ")"); return;
  }

  /* poly receiver: gsub/sub with a regex literal -- extract the string
     payload (poly values reaching here are strings) and route to the
     engine, just like a TY_STRING receiver. */
  if (recv >= 0 && rt == TY_POLY && (!strcmp(name, "gsub") || !strcmp(name, "sub")) &&
      argc == 2 && re_lit_index(c, argv[0]) >= 0) {
    const char *suf = comp_ntype(c, argv[1]) == TY_STR_STR_HASH ? "_str_str_hash" : "";
    buf_printf(b, "sp_re_%s%s(sp_re_pat_%d, sp_poly_to_s(", name, suf, re_lit_index(c, argv[0]));
    emit_expr(c, recv, b); buf_puts(b, "), ");
    emit_expr(c, argv[1], b); buf_puts(b, ")");
    return;
  }

  /* between?(lo, hi): lo <= self <= hi */
  if (!strcmp(name, "between?") && argc == 2) {
    if (rt == TY_STRING) {
      int tv = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = ", tv); emit_expr(c, recv, b);
      buf_printf(b, "; (strcmp(_t%d, ", tv); emit_expr(c, argv[0], b);
      buf_printf(b, ") >= 0 && strcmp(_t%d, ", tv); emit_expr(c, argv[1], b); buf_puts(b, ") <= 0); })");
      return;
    }
    if (ty_is_numeric(rt)) {
      int tv = ++g_tmp;
      buf_puts(b, "({ "); emit_ctype(c, rt, b); buf_printf(b, " _t%d = ", tv); emit_expr(c, recv, b);
      buf_printf(b, "; (_t%d >= ", tv); emit_expr(c, argv[0], b);
      buf_printf(b, " && _t%d <= ", tv); emit_expr(c, argv[1], b); buf_puts(b, "); })");
      return;
    }
  }

  /* object_id: a stable integer id. Int uses MRI's 2n+1; pointer-backed
     values use the pointer bit pattern; a symbol uses its interned id. */
  if (!strcmp(name, "object_id") && recv >= 0 && argc == 0) {
    if (rt == TY_INT) { buf_puts(b, "(2*("); emit_expr(c, recv, b); buf_puts(b, ")+1)"); }
    else if (rt == TY_SYMBOL) { buf_puts(b, "((mrb_int)("); emit_expr(c, recv, b); buf_puts(b, ")*2)"); }
    else if (rt == TY_BOOL || rt == TY_NIL) { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 0)"); }
    else { buf_puts(b, "((mrb_int)(uintptr_t)("); emit_expr(c, recv, b); buf_puts(b, "))"); }
    return;
  }

  /* nil? on an integer: a nullable int carries the SP_INT_NIL sentinel
     (e.g. an int-valued hash miss). A plain int is never the sentinel, so
     `5.nil?` constant-folds to false; a missing-key value reads true. */
  if (recv >= 0 && rt == TY_INT && !strcmp(name, "nil?") && argc == 0) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == SP_INT_NIL)");
    return;
  }
  /* nil? on a string: a nullable string carries NULL (e.g. a scan miss) */
  if (recv >= 0 && rt == TY_STRING && !strcmp(name, "nil?") && argc == 0) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == 0)");
    return;
  }
  /* nil? on a float: a nullable float carries the NaN sentinel (e.g. first/
     last of an empty float array). A real float is never the sentinel. */
  if (recv >= 0 && rt == TY_FLOAT && !strcmp(name, "nil?") && argc == 0) {
    buf_puts(b, "sp_float_is_nil("); emit_expr(c, recv, b); buf_puts(b, ")");
    return;
  }
  /* nil? on an array/hash: a nil container is a NULL pointer */
  if (recv >= 0 && (ty_is_array(rt) || ty_is_hash(rt)) && !strcmp(name, "nil?") && argc == 0) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == NULL)");
    return;
  }
  /* a predicate on an empty array literal folds to a constant: the block (if
     any) never runs, so empty all?/none? are true, any?/one? false */
  if (recv >= 0 && argc == 0 &&
      (!strcmp(name, "all?") || !strcmp(name, "any?") ||
       !strcmp(name, "none?") || !strcmp(name, "one?")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ArrayNode") &&
      ({ int _n = 0; nt_arr(nt, recv, "elements", &_n); _n == 0; })) {
    buf_puts(b, (!strcmp(name, "all?") || !strcmp(name, "none?")) ? "1" : "0");
    return;
  }

  /* `===` on a scalar comparable (bool/int/float/string/symbol) is case
     equality == value equality. Range/Class/Regexp `===` have their own
     handlers and fall through here. */
  if (argc == 1 && !strcmp(name, "===")) {
    int fr = eq_family(rt), fa = eq_family(a0);
    if (fr && fr != 5 && fa && fa != 5) {
      if (fr == fa) {
        if (fr == 2) { buf_puts(b, "sp_str_eq("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else { buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, " == "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      }
      else { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), ("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); }
      return;
    }
  }

  if (argc == 1 && (!strcmp(name, "==") || !strcmp(name, "!="))) {
    int eq = !strcmp(name, "==");
    /* `x == nil` / `x != nil` for any receiver */
    int a_nil = nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "NilNode");
    int r_nil = nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "NilNode");
    if (a_nil || r_nil) {
      int other = a_nil ? recv : argv[0];
      TyKind ot = comp_ntype(c, other);
      if (ot == TY_POLY) {
        buf_puts(b, eq ? "sp_poly_nil_p(" : "(!sp_poly_nil_p(");
        emit_expr(c, other, b); buf_puts(b, eq ? ")" : "))");
      }
      else if (ot == TY_NIL) buf_puts(b, eq ? "1" : "0");
      else if (ot == TY_INT) {
        /* a nullable int compares equal to nil iff it holds the sentinel;
           a plain int constant-folds to false */
        buf_puts(b, "(("); emit_expr(c, other, b); buf_printf(b, ") %s SP_INT_NIL)", eq ? "==" : "!=");
      }
      else if (ot == TY_FLOAT) {
        /* a nullable float carries the NaN sentinel */
        buf_puts(b, eq ? "sp_float_is_nil(" : "(!sp_float_is_nil(");
        emit_expr(c, other, b); buf_puts(b, eq ? ")" : "))");
      }
      else if (ot == TY_STRING || ot == TY_MATCHDATA || ot == TY_STRINGIO || ot == TY_STRINGSCANNER) {
        /* nullable pointer: NULL == nil */
        buf_puts(b, "(("); emit_expr(c, other, b); buf_printf(b, ") %s 0)", eq ? "==" : "!=");
      }
      else { buf_puts(b, "(("); emit_expr(c, other, b); buf_printf(b, "), %d)", eq ? 0 : 1); }
      return;
    }
    /* arr == [] : an array equals the empty literal iff it has no elements */
    {
      int er = nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ArrayNode") &&
               ({ int _n = 0; nt_arr(nt, recv, "elements", &_n); _n == 0; });
      int ea = nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "ArrayNode") &&
               ({ int _n = 0; nt_arr(nt, argv[0], "elements", &_n); _n == 0; });
      if ((er && (array_kind(a0) || a0 == TY_POLY_ARRAY)) ||
          (ea && (array_kind(rt) || rt == TY_POLY_ARRAY))) {
        int arr = er ? argv[0] : recv;
        TyKind at = er ? a0 : rt;
        const char *kk = array_kind(at);
        buf_printf(b, "(%ssp_%sArray_length(", eq ? "" : "!", kk ? kk : "Poly");
        emit_expr(c, arr, b); buf_puts(b, ") == 0)");
        return;
      }
    }
    if (rt == TY_POLY_ARRAY && a0 == TY_POLY_ARRAY) {
      buf_puts(b, eq ? "sp_PolyArray_eq(" : "(!sp_PolyArray_eq(");
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
      buf_puts(b, eq ? ")" : "))");
      return;
    }
    /* two typed arrays of the same kind: element-wise compare */
    if (array_kind(rt) && rt == a0) {
      if (!eq) buf_puts(b, "(!");
      buf_printf(b, "sp_%sArray_eq(", array_kind(rt));
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
      buf_puts(b, eq ? ")" : "))");
      return;
    }
    /* poly array vs a typed array: box the typed side element-wise */
    if ((rt == TY_POLY_ARRAY && array_kind(a0)) || (a0 == TY_POLY_ARRAY && array_kind(rt))) {
      int polyn = rt == TY_POLY_ARRAY ? recv : argv[0];
      int typedn = rt == TY_POLY_ARRAY ? argv[0] : recv;
      TyKind tk = rt == TY_POLY_ARRAY ? a0 : rt;
      const char *kind = tk == TY_STR_ARRAY ? "SP_BUILTIN_STR_ARRAY"
                       : tk == TY_FLOAT_ARRAY ? "SP_BUILTIN_FLT_ARRAY" : "SP_BUILTIN_INT_ARRAY";
      buf_puts(b, eq ? "sp_PolyArray_eq_typed(" : "(!sp_PolyArray_eq_typed(");
      emit_expr(c, polyn, b); buf_puts(b, ", (void *)("); emit_expr(c, typedn, b);
      buf_printf(b, "), %s)%s", kind, eq ? "" : ")");
      return;
    }
    /* hash == hash */
    if (ty_is_hash(rt) || ty_is_hash(a0) || rt == TY_UNKNOWN || a0 == TY_UNKNOWN) {
      /* two empty hash literals are trivially equal */
      int re = nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "HashNode") &&
               ({ int _n = 0; nt_arr(nt, recv, "elements", &_n); _n == 0; });
      int ae = nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "HashNode") &&
               ({ int _n = 0; nt_arr(nt, argv[0], "elements", &_n); _n == 0; });
      if (re && ae) { buf_puts(b, eq ? "1" : "0"); return; }
      if (ty_is_hash(rt) && ty_is_hash(a0)) {
        if (rt == a0) {
          /* same typed hash: use the dedicated equality function */
          const char *hn = ty_hash_cname(rt);
          if (hn) {
            buf_puts(b, eq ? "" : "(!");
            buf_printf(b, "sp_%sHash_eq(", hn);
            emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
            buf_puts(b, eq ? ")" : "))");
            return;
          }
        }
        /* different hash types can never be equal */
        buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), (");
        emit_expr(c, argv[0], b); buf_printf(b, "), %d)", eq ? 0 : 1);
        return;
      }
      if (ty_is_hash(rt) || ty_is_hash(a0)) {
        /* hash vs non-hash */
        buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), (");
        emit_expr(c, argv[0], b); buf_printf(b, "), %d)", eq ? 0 : 1);
        return;
      }
    }
    /* a poly operand compares dynamically (covers string-vs-poly etc.) */
    if (rt == TY_POLY || a0 == TY_POLY) {
      buf_puts(b, eq ? "sp_poly_eq(" : "(!sp_poly_eq(");
      emit_boxed(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b);
      buf_puts(b, eq ? ")" : "))");
      return;
    }
    {
      int fr = eq_family(rt), fa = eq_family(a0);
      /* same comparable family: compare by value */
      if (fr && fa && fr == fa) {
        if (fr == 2) { buf_puts(b, eq ? "sp_str_eq(" : "(!sp_str_eq("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, eq ? ")" : "))"); }
        else if (fr == 5) { buf_puts(b, eq ? "sp_range_eq(" : "(!sp_range_eq("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, eq ? ")" : "))"); }
        else { buf_puts(b, "("); emit_expr(c, recv, b); buf_printf(b, " %s ", eq ? "==" : "!="); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        return;
      }
      /* two different concrete types are never == in Ruby (no coercion);
         still evaluate both operands for their side effects */
      if (fr && fa) {
        buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), (");
        emit_expr(c, argv[0], b); buf_printf(b, "), %d)", eq ? 0 : 1);
        return;
      }
    }
    /* object == / != : try direct method, then fall back to <=> == 0 */
    if (recv >= 0 && ty_is_object(rt)) {
      int ecid = ty_object_class(rt);
      int emi = comp_method_in_chain(c, ecid, name, NULL);
      if (emi >= 0) {
        char selfptr[64];
        const char *rty2 = nt_type(nt, recv);
        if (rty2 && (!strcmp(rty2, "LocalVariableReadNode") ||
                     !strcmp(rty2, "InstanceVariableReadNode") ||
                     !strcmp(rty2, "SelfNode"))) {
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          snprintf(selfptr, sizeof selfptr, "%s", rb.p ? rb.p : "");
          free(rb.p);
        }
        else {
          int t2 = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          emit_indent(g_pre, g_indent);
          emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", t2, rb.p ? rb.p : "");
          free(rb.p);
          snprintf(selfptr, sizeof selfptr, "_t%d", t2);
        }
        emit_dispatch(c, ecid, name, selfptr, nt_ref(nt, id, "arguments"), b);
        return;
      }
      /* no direct == : use <=> == 0 when the class supports Comparable */
      if (comp_method_in_chain(c, ecid, "<=>", NULL) >= 0) {
        char selfptr[64];
        const char *rty2 = nt_type(nt, recv);
        if (rty2 && (!strcmp(rty2, "LocalVariableReadNode") ||
                     !strcmp(rty2, "InstanceVariableReadNode") ||
                     !strcmp(rty2, "SelfNode"))) {
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          snprintf(selfptr, sizeof selfptr, "%s", rb.p ? rb.p : "");
          free(rb.p);
        }
        else {
          int t3 = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          emit_indent(g_pre, g_indent);
          emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", t3, rb.p ? rb.p : "");
          free(rb.p);
          snprintf(selfptr, sizeof selfptr, "_t%d", t3);
        }
        buf_puts(b, "(");
        emit_dispatch(c, ecid, "<=>", selfptr, nt_ref(nt, id, "arguments"), b);
        buf_printf(b, " %s 0)", eq ? "==" : "!=");
        return;
      }
    }
    unsupported(c, id, "equality");
  }

  /* obj.is_a?/kind_of?/instance_of?(Class): resolved at compile time from
     the receiver's static class. */
  if (recv >= 0 && ty_is_object(rt) && argc == 1 &&
      (!strcmp(name, "is_a?") || !strcmp(name, "kind_of?") || !strcmp(name, "instance_of?"))) {
    const char *cn = nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "ConstantReadNode")
                     ? nt_str(nt, argv[0], "name") : NULL;
    if (cn) {
      int cid = ty_object_class(rt);
      int target = comp_class_index(c, cn);
      int yes;
      if (target >= 0) yes = !strcmp(name, "instance_of?") ? (cid == target) : is_descendant(c, cid, target);
      else yes = 0;  /* a user object is not a builtin (Integer/String/...) */
      /* evaluate the receiver for side effects, then the constant result */
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_printf(b, "), %d)", yes);
      return;
    }
  }

  /* Struct instance methods (to_h / to_a / values / members / dig). */
  if (recv >= 0 && ty_is_object(rt) && c->classes[ty_object_class(rt)].is_struct) {
    ClassInfo *sc = &c->classes[ty_object_class(rt)];
    int is_to_a = (!strcmp(name, "to_a") || !strcmp(name, "values") || !strcmp(name, "deconstruct"));
    if (is_to_a && argc == 0) {
      int t = ++g_tmp; int rt2 = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
      buf_printf(b, "({ sp_%s *_t%d = %s; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);",
                 sc->name, t, rb.p ? rb.p : "", rt2, rt2);
      for (int i = 0; i < sc->nivars; i++) {
        buf_printf(b, " sp_PolyArray_push(_t%d, ", rt2);
        Buf fb; memset(&fb, 0, sizeof fb); buf_printf(&fb, "_t%d->iv_%s", t, sc->ivars[i] + 1);
        emit_boxed_text(c, sc->ivar_types[i], fb.p, b); free(fb.p);
        buf_puts(b, ");");
      }
      buf_printf(b, " _t%d; })", rt2);
      free(rb.p);
      return;
    }
    if (!strcmp(name, "to_h") && argc == 0) {
      int block = nt_ref(nt, id, "block");
      int t = ++g_tmp, rh = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
      TyKind res = comp_ntype(c, id);
      const char *hn = ty_hash_cname(res);
      if (!hn) hn = "SymPoly";
      buf_printf(b, "({ sp_%s *_t%d = %s; sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);",
                 sc->name, t, rb.p ? rb.p : "", hn, rh, hn, rh);
      free(rb.p);
      if (block >= 0) {
        /* to_h { |k, v| [nk, nv] }: per member, bind k/v then set hash[nk] = nv */
        const char *kp = block_param_name(c, block, 0); if (kp) kp = rename_local(kp);
        const char *vp = block_param_name(c, block, 1); if (vp) vp = rename_local(vp);
        int bbody = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
        int last = bn > 0 ? bb[bn - 1] : -1;
        int ke = -1, ve = -1;
        if (last >= 0 && nt_type(nt, last) && !strcmp(nt_type(nt, last), "ArrayNode")) {
          int en = 0; const int *els = nt_arr(nt, last, "elements", &en);
          if (en == 2) { ke = els[0]; ve = els[1]; }
        }
        TyKind kt = ty_hash_key(res), vt = ty_hash_val(res);
        for (int i = 0; i < sc->nivars; i++) {
          if (kp) buf_printf(b, " lv_%s = (sp_sym)%d;", kp, comp_sym_intern(c, sc->ivars[i] + 1));
          if (vp) {
            char fb[300]; snprintf(fb, sizeof fb, "_t%d->iv_%s", t, sc->ivars[i] + 1);
            buf_printf(b, " lv_%s = ", vp); emit_boxed_text(c, sc->ivar_types[i], fb, b); buf_puts(b, ";");
          }
          buf_printf(b, " sp_%sHash_set(_t%d, ", hn, rh);
          if (ke >= 0) emit_expr(c, ke, b); else buf_puts(b, "0");
          buf_puts(b, ", ");
          if (ve >= 0) { if (vt == TY_POLY && comp_ntype(c, ve) != TY_POLY) emit_boxed(c, ve, b); else emit_expr(c, ve, b); }
          else buf_puts(b, "0");
          buf_puts(b, ");");
        }
      }
      else {
        for (int i = 0; i < sc->nivars; i++) {
          buf_printf(b, " sp_SymPolyHash_set(_t%d, (sp_sym)%d, ", rh, comp_sym_intern(c, sc->ivars[i] + 1));
          char fb[300]; snprintf(fb, sizeof fb, "_t%d->iv_%s", t, sc->ivars[i] + 1);
          emit_boxed_text(c, sc->ivar_types[i], fb, b);
          buf_puts(b, ");");
        }
      }
      buf_printf(b, " _t%d; })", rh);
      return;
    }
    if ((!strcmp(name, "members")) && argc == 0) {
      int rm = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", rm, rm);
      for (int i = 0; i < sc->nivars; i++)
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym((sp_sym)%d));", rm, comp_sym_intern(c, sc->ivars[i] + 1));
      buf_printf(b, " _t%d; })", rm);
      return;
    }
    if (!strcmp(name, "dig") && argc >= 1) {
      /* literal key resolves a member at compile time */
      int mi = -1;
      const char *kty = nt_type(nt, argv[0]);
      if (kty && !strcmp(kty, "SymbolNode")) {
        char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", nt_str(nt, argv[0], "value"));
        mi = comp_ivar_index(sc, ivn);
      }
      else if (kty && !strcmp(kty, "IntegerNode")) {
        int v = (int)nt_int(nt, argv[0], "value", -1);
        if (v >= 0 && v < sc->nivars) mi = v;
      }
      if (mi >= 0) {
        int t = ++g_tmp;
        Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
        char fld[300]; snprintf(fld, sizeof fld, "_t%d->iv_%s", t, sc->ivars[mi] + 1);
        TyKind mt = sc->ivar_types[mi];
        buf_printf(b, "({ sp_%s *_t%d = %s; ", sc->name, t, rb.p ? rb.p : ""); free(rb.p);
        if (argc == 1) buf_puts(b, fld);
        else if (ty_is_hash(mt) && argc == 2) {
          const char *hn = ty_hash_cname(mt);
          buf_printf(b, "sp_%sHash_%s(%s, ", hn, ty_hash_val(mt) == TY_INT ? "get_opt" : "get", fld);
          emit_expr(c, argv[1], b); buf_puts(b, ")");
        }
        else if (ty_is_array(mt) && argc == 2) {
          buf_printf(b, "sp_%sArray_get(%s, ", array_kind(mt), fld); emit_expr(c, argv[1], b); buf_puts(b, ")");
        }
        else buf_puts(b, fld);
        buf_puts(b, "; })");
        return;
      }
    }
  }

  /* object method call: sp_<DefClass>_<m>((sp_<DefClass>*)&recv, args) */
  if (recv >= 0 && ty_is_object(rt)) {
    int cid = ty_object_class(rt);
    /* attr reader -> field access (recv).iv_x */
    if (comp_reader_in_chain(c, cid, name, NULL)) {
      buf_puts(b, "("); emit_expr(c, recv, b); buf_printf(b, ")->iv_%s", name);
      return;
    }
    int mi = comp_method_in_chain(c, cid, name, NULL);
    if (mi >= 0) {
      /* receiver is a pointer; reuse it directly if it's a simple lvalue,
         else stash in a temp (the virtual-dispatch switch references it
         multiple times) */
      char selfptr[64];
      const char *rty = nt_type(nt, recv);
      if (rty && (!strcmp(rty, "LocalVariableReadNode") || !strcmp(rty, "InstanceVariableReadNode") || !strcmp(rty, "SelfNode"))) {
        Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
        snprintf(selfptr, sizeof selfptr, "%s", rb.p ? rb.p : "");
        free(rb.p);
      }
      else {
        int t = ++g_tmp;
        /* emit the receiver first so any setup it pushes into g_pre is fully
           flushed before we write this temp's declaration line */
        Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
        emit_indent(g_pre, g_indent);
        emit_ctype(c, rt, g_pre);
        buf_printf(g_pre, " _t%d = ", t);
        buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
        snprintf(selfptr, sizeof selfptr, "_t%d", t);
      }
      emit_dispatch(c, cid, name, selfptr, nt_ref(nt, id, "arguments"), b);
      return;
    }
  }

  /* Time instance methods: sp_Time is a value -- splice the receiver once. */
  if (recv >= 0 && rt == TY_TIME) {
    Buf rs; memset(&rs, 0, sizeof rs); emit_expr(c, recv, &rs);
    const char *r = rs.p ? rs.p : "";
    int done = 1;
    if (!strcmp(name, "utc") || !strcmp(name, "gmtime") || !strcmp(name, "getutc")) buf_printf(b, "sp_time_utc(%s)", r);
    else if (!strcmp(name, "localtime") || !strcmp(name, "getlocal")) buf_printf(b, "sp_time_localtime(%s)", r);
    else if (!strcmp(name, "year"))  buf_printf(b, "sp_time_year(%s)", r);
    else if (!strcmp(name, "mon") || !strcmp(name, "month")) buf_printf(b, "sp_time_mon(%s)", r);
    else if (!strcmp(name, "day") || !strcmp(name, "mday"))  buf_printf(b, "sp_time_mday(%s)", r);
    else if (!strcmp(name, "hour")) buf_printf(b, "sp_time_hour(%s)", r);
    else if (!strcmp(name, "min"))  buf_printf(b, "sp_time_min(%s)", r);
    else if (!strcmp(name, "sec"))  buf_printf(b, "sp_time_sec(%s)", r);
    else if (!strcmp(name, "wday")) buf_printf(b, "sp_time_wday(%s)", r);
    else if (!strcmp(name, "yday")) buf_printf(b, "sp_time_yday(%s)", r);
    else if (!strcmp(name, "to_i") || !strcmp(name, "tv_sec")) buf_printf(b, "(%s).tv_sec", r);
    else if (!strcmp(name, "to_f")) buf_printf(b, "((mrb_float)(%s).tv_sec + (mrb_float)(%s).tv_nsec / 1e9)", r, r);
    else if (!strcmp(name, "subsec")) buf_printf(b, "((mrb_float)(%s).tv_nsec / 1e9)", r);
    else if (!strcmp(name, "tv_usec") || !strcmp(name, "usec")) buf_printf(b, "((mrb_int)(%s).tv_nsec / 1000)", r);
    else if (!strcmp(name, "tv_nsec") || !strcmp(name, "nsec")) buf_printf(b, "((mrb_int)(%s).tv_nsec)", r);
    else if (!strcmp(name, "utc?") || !strcmp(name, "gmt?")) buf_printf(b, "((%s).is_utc != 0)", r);
    else if (!strcmp(name, "dst?") || !strcmp(name, "isdst")) buf_printf(b, "(sp_time_isdst(%s) != 0)", r);
    else if (!strcmp(name, "utc_offset") || !strcmp(name, "gmt_offset") || !strcmp(name, "gmtoff")) buf_printf(b, "sp_time_utc_offset(%s)", r);
    else if (!strcmp(name, "to_s") || !strcmp(name, "inspect")) buf_printf(b, "sp_time_inspect_v(%s)", r);
    else if (!strcmp(name, "iso8601")) buf_printf(b, "sp_time_iso8601(%s)", r);
    else if (!strcmp(name, "zone")) buf_printf(b, "sp_time_zone(%s)", r);
    else if (!strcmp(name, "class")) buf_puts(b, "SPL(\"Time\")");
    else if (!strcmp(name, "strftime") && argc == 1) { buf_printf(b, "sp_time_strftime(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if ((!strcmp(name, "+") || !strcmp(name, "-")) && argc == 1) {
      buf_printf(b, "sp_time_add(%s, %s(mrb_float)(", r, name[0] == '-' ? "-" : "");
      emit_expr(c, argv[0], b); buf_puts(b, "))");
    }
    else done = 0;
    free(rs.p);
    if (done) return;
  }

  /* StringScanner instance methods. String-returning methods may yield NULL
     (nil) on a miss; the NULL-aware string output operators render that. */
  if (recv >= 0 && rt == TY_STRINGSCANNER) {
    Buf rs; memset(&rs, 0, sizeof rs); emit_expr(c, recv, &rs);
    const char *r = rs.p ? rs.p : "";
    int done = 1;
    if ((!strcmp(name, "scan") || !strcmp(name, "check") || !strcmp(name, "scan_until")) &&
        argc == 1 && re_lit_index(c, argv[0]) >= 0) {
      buf_printf(b, "sp_StringScanner_%s(%s, sp_re_pat_%d)", name, r, re_lit_index(c, argv[0]));
    }
    else if (!strcmp(name, "matched")) buf_printf(b, "sp_StringScanner_matched(%s)", r);
    else if (!strcmp(name, "matched?")) buf_printf(b, "sp_StringScanner_matched_p(%s)", r);
    else if (!strcmp(name, "pre_match")) buf_printf(b, "sp_StringScanner_pre_match(%s)", r);
    else if (!strcmp(name, "post_match")) buf_printf(b, "sp_StringScanner_post_match(%s)", r);
    else if (!strcmp(name, "pos") || !strcmp(name, "charpos")) buf_printf(b, "sp_StringScanner_pos(%s)", r);
    else if (!strcmp(name, "pos=") && argc == 1) { buf_printf(b, "sp_StringScanner_pos_set(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (!strcmp(name, "rest")) buf_printf(b, "sp_StringScanner_rest(%s)", r);
    else if (!strcmp(name, "rest?")) buf_printf(b, "sp_StringScanner_rest_p(%s)", r);
    else if (!strcmp(name, "rest_size")) buf_printf(b, "sp_StringScanner_rest_size(%s)", r);
    else if (!strcmp(name, "string")) buf_printf(b, "sp_StringScanner_string(%s)", r);
    else if (!strcmp(name, "eos?")) buf_printf(b, "sp_StringScanner_eos_p(%s)", r);
    else if (!strcmp(name, "getch")) buf_printf(b, "sp_StringScanner_getch(%s)", r);
    else if (!strcmp(name, "peek") && argc == 1) { buf_printf(b, "sp_StringScanner_peek(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (!strcmp(name, "[]") && argc == 1) { buf_printf(b, "sp_StringScanner_aref(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (!strcmp(name, "reset")) buf_printf(b, "(sp_StringScanner_reset(%s), %s)", r, r);
    else if (!strcmp(name, "terminate")) buf_printf(b, "(sp_StringScanner_terminate(%s), %s)", r, r);
    else if (!strcmp(name, "unscan")) buf_printf(b, "(sp_StringScanner_unscan(%s), %s)", r, r);
    else done = 0;
    free(rs.p);
    if (done) return;
  }

  /* MatchData instance methods (sp_MatchData *, nullable on no-match). */
  if (recv >= 0 && rt == TY_MATCHDATA) {
    Buf rs; memset(&rs, 0, sizeof rs); emit_expr(c, recv, &rs);
    const char *r = rs.p ? rs.p : "";
    if (!strcmp(name, "[]") && argc == 1) {
      buf_printf(b, "sp_MatchData_aref(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (!strcmp(name, "pre_match"))  buf_printf(b, "sp_MatchData_pre_match(%s)", r);
    else if (!strcmp(name, "post_match")) buf_printf(b, "sp_MatchData_post_match(%s)", r);
    else if (!strcmp(name, "to_s"))       buf_printf(b, "sp_MatchData_to_s(%s)", r);
    else if ((!strcmp(name, "length") || !strcmp(name, "size")) && argc == 0)
      buf_printf(b, "sp_MatchData_length(%s)", r);
    else if (!strcmp(name, "begin") && argc == 1) {
      buf_printf(b, "sp_MatchData_begin(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (!strcmp(name, "end") && argc == 1) {
      buf_printf(b, "sp_MatchData_end(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (!strcmp(name, "offset") && argc == 1) {
      buf_printf(b, "sp_MatchData_offset(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (!strcmp(name, "captures"))  buf_printf(b, "sp_MatchData_captures(%s)", r);
    else if (!strcmp(name, "to_a"))      buf_printf(b, "sp_MatchData_to_a(%s)", r);
    else if (!strcmp(name, "nil?"))      buf_printf(b, "(%s == 0)", r);
    else unsupported(c, id, "MatchData method");
    free(rs.p);
    return;
  }

  /* StringIO instance methods (a non-GC heap buffer behind sp_StringIO *). */
  if (recv >= 0 && rt == TY_STRINGIO) {
    Buf rs; memset(&rs, 0, sizeof rs); emit_expr(c, recv, &rs);
    const char *r = rs.p ? rs.p : "";
    int done = 1;
    if (!strcmp(name, "string")) buf_printf(b, "sp_StringIO_string(%s)", r);
    else if (!strcmp(name, "pos") || !strcmp(name, "tell")) buf_printf(b, "sp_StringIO_pos(%s)", r);
    else if (!strcmp(name, "size") || !strcmp(name, "length")) buf_printf(b, "sp_StringIO_size(%s)", r);
    else if (!strcmp(name, "lineno")) buf_printf(b, "(%s)->lineno", r);
    else if (!strcmp(name, "puts") && argc == 0) buf_printf(b, "sp_StringIO_puts_empty(%s)", r);
    else if (!strcmp(name, "puts") && argc == 1) { buf_printf(b, "sp_StringIO_puts(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (!strcmp(name, "print") && argc == 1) { buf_printf(b, "sp_StringIO_print(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if ((!strcmp(name, "write") || !strcmp(name, "<<")) && argc == 1) { buf_printf(b, "sp_StringIO_write(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (!strcmp(name, "putc") && argc == 1) {
      if (comp_ntype(c, argv[0]) == TY_STRING) { buf_printf(b, "sp_StringIO_putc(%s, (mrb_int)(unsigned char)(", r); emit_expr(c, argv[0], b); buf_puts(b, ")[0])"); }
      else { buf_printf(b, "sp_StringIO_putc(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    }
    else if (!strcmp(name, "fsync") || !strcmp(name, "fileno") || !strcmp(name, "pid")) buf_printf(b, "((void)(%s), 0)", r);
    else if (!strcmp(name, "read") && argc == 0) buf_printf(b, "sp_StringIO_read(%s)", r);
    else if (!strcmp(name, "read") && argc == 1) { buf_printf(b, "sp_StringIO_read_n(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (!strcmp(name, "gets")) buf_printf(b, "sp_box_nullable_str(sp_StringIO_gets(%s))", r);
    else if (!strcmp(name, "getc")) buf_printf(b, "sp_box_nullable_str(sp_StringIO_getc(%s))", r);
    else if (!strcmp(name, "getbyte")) buf_printf(b, "sp_StringIO_getbyte(%s)", r);
    else if (!strcmp(name, "rewind")) buf_printf(b, "sp_StringIO_rewind(%s)", r);
    else if (!strcmp(name, "seek") && argc >= 1) { buf_printf(b, "sp_StringIO_seek(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (!strcmp(name, "truncate") && argc == 1) { buf_printf(b, "sp_StringIO_truncate(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (!strcmp(name, "eof?") || !strcmp(name, "eof")) buf_printf(b, "sp_StringIO_eof_p(%s)", r);
    else if (!strcmp(name, "close")) buf_printf(b, "sp_StringIO_close(%s)", r);
    else if (!strcmp(name, "closed?")) buf_printf(b, "sp_StringIO_closed_p(%s)", r);
    else if (!strcmp(name, "flush")) buf_printf(b, "sp_StringIO_flush(%s)", r);
    else if (!strcmp(name, "sync")) buf_printf(b, "sp_StringIO_sync(%s)", r);
    else if (!strcmp(name, "isatty") || !strcmp(name, "tty?")) buf_printf(b, "sp_StringIO_isatty(%s)", r);
    else done = 0;
    free(rs.p);
    if (done) return;
  }

  /* poly method dispatch: switch on the boxed object's cls_id and call the
     matching class's method (walking the chain for inherited methods),
     unboxing the pointer. */
  if (recv >= 0 && rt == TY_POLY && argc == 0) {
    int is_lengthlike = !strcmp(name, "length") || !strcmp(name, "size") || !strcmp(name, "count");
    int ncand = 0;
    for (int k = 0; k < c->nclasses; k++)
      if (comp_method_in_chain(c, k, name, NULL) >= 0 || comp_reader_in_chain(c, k, name, NULL)) ncand++;
    if (ncand > 0 || is_lengthlike) {
      TyKind ret = comp_ntype(c, id);
      int tv = ++g_tmp, tr = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", tv); emit_expr(c, recv, b); buf_puts(b, "; ");
      emit_ctype(c, is_scalar_ret(ret) ? ret : TY_INT, b);
      buf_printf(b, " _t%d = %s; ", tr, is_scalar_ret(ret) ? default_value(ret) : "0");
      /* string/symbol-tagged poly values answer length/size directly */
      if (is_lengthlike) {
        buf_printf(b, "if (_t%d.tag == SP_TAG_SYM) _t%d = (mrb_int)strlen(sp_sym_to_s((sp_sym)_t%d.v.i)); else ", tv, tr, tv);
        buf_printf(b, "if (_t%d.tag == SP_TAG_STR) _t%d = (mrb_int)sp_str_length(_t%d.v.s); else ", tv, tr, tv);
      }
      buf_printf(b, "switch (_t%d.cls_id) {", tv);
      for (int k = 0; k < c->nclasses; k++) {
        int defcls = -1;
        int mi = comp_method_in_chain(c, k, name, &defcls);
        if (mi >= 0 && c->scopes[mi].nrequired == 0) {
          /* Build the call; append default values for any optional params
             not provided by the (zero-arg) call site. */
          Buf cb; memset(&cb, 0, sizeof cb);
          buf_printf(&cb, "sp_%s_%s((sp_%s *)_t%d.v.p",
                     c->classes[defcls].name, mc(c->scopes[mi].name), c->classes[defcls].name, tv);
          if (c->scopes[mi].nparams > 0) {
            const char *saved_self = g_self;
            static char selfpbuf[64];
            snprintf(selfpbuf, sizeof selfpbuf, "(sp_%s *)_t%d.v.p", c->classes[defcls].name, tv);
            g_self = selfpbuf;
            for (int a = 0; a < c->scopes[mi].nparams; a++) {
              buf_puts(&cb, ", "); emit_arg_or_default(c, &c->scopes[mi], a, -1, &cb);
            }
            g_self = saved_self;
          }
          buf_puts(&cb, ")");
          const char *call = cb.p ? cb.p : "";
          buf_printf(b, " case %d: ", k);
          if (method_is_void(&c->scopes[mi])) buf_puts(b, call);  /* void: no usable value */
          else {
            buf_printf(b, "_t%d = ", tr);
            if (ret == TY_POLY && c->scopes[mi].ret != TY_POLY) emit_boxed_text(c, c->scopes[mi].ret, call, b);
            else buf_puts(b, call);
          }
          buf_puts(b, "; break;");
          free(cb.p);
          continue;
        }
        int rdcls = -1;
        if (comp_reader_in_chain(c, k, name, &rdcls)) {
          char fld[600];
          snprintf(fld, sizeof fld, "((sp_%s *)_t%d.v.p)->iv_%s", c->classes[rdcls].name, tv, name);
          char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", name);
          int ivx = comp_ivar_index(&c->classes[rdcls], ivn);
          TyKind ivt = ivx >= 0 ? c->classes[rdcls].ivar_types[ivx] : TY_INT;
          buf_printf(b, " case %d: _t%d = ", k, tr);
          if (ret == TY_POLY && ivt != TY_POLY) emit_boxed_text(c, ivt, fld, b);
          else buf_puts(b, fld);
          buf_puts(b, "; break;");
        }
      }
      /* built-in array receivers reaching a length-like poly dispatch */
      if (!strcmp(name, "length") || !strcmp(name, "size") || !strcmp(name, "count")) {
        buf_printf(b, " case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_SYM_ARRAY: _t%d = sp_IntArray_length((sp_IntArray *)_t%d.v.p); break;", tr, tv);
        buf_printf(b, " case SP_BUILTIN_STR_ARRAY: _t%d = sp_StrArray_length((sp_StrArray *)_t%d.v.p); break;", tr, tv);
        buf_printf(b, " case SP_BUILTIN_FLT_ARRAY: _t%d = sp_FloatArray_length((sp_FloatArray *)_t%d.v.p); break;", tr, tv);
        buf_printf(b, " case SP_BUILTIN_POLY_ARRAY: _t%d = sp_PolyArray_length((sp_PolyArray *)_t%d.v.p); break;", tr, tv);
      }
      buf_printf(b, " } _t%d; })", tr);
      return;
    }
  }

  /* poly method dispatch with arguments: switch on the boxed object's cls_id
     and call the matching user method (or a builtin array `[]`), passing the
     arguments evaluated once into temps. */
  if (recv >= 0 && rt == TY_POLY && argc > 0) {
    /* the builtin-array `[]` arm only applies to an integer index */
    int is_index = !strcmp(name, "[]") && argc == 1 && comp_ntype(c, argv[0]) == TY_INT;
    int ncand = 0;
    for (int k = 0; k < c->nclasses; k++) {
      int mi = comp_method_in_chain(c, k, name, NULL);
      /* Include if call supplies all required params (pad defaults / truncate extras) */
      if (mi >= 0 && argc >= c->scopes[mi].nrequired) ncand++;
    }
    if (ncand > 0 || is_index) {
      TyKind ret = comp_ntype(c, id);
      int tv = ++g_tmp, tr = ++g_tmp;
      int *atmp = malloc(sizeof(int) * argc);
      buf_printf(b, "({ sp_RbVal _t%d = ", tv); emit_expr(c, recv, b); buf_puts(b, "; ");
      for (int a = 0; a < argc; a++) {
        atmp[a] = ++g_tmp;
        emit_ctype(c, infer_type(c, argv[a]), b);
        buf_printf(b, " _t%d = ", atmp[a]); emit_expr(c, argv[a], b); buf_puts(b, "; ");
      }
      emit_ctype(c, is_scalar_ret(ret) ? ret : TY_INT, b);
      buf_printf(b, " _t%d = %s; ", tr, is_scalar_ret(ret) ? default_value(ret) : "0");
      buf_printf(b, "switch (_t%d.cls_id) {", tv);
      for (int k = 0; k < c->nclasses; k++) {
        int defcls = -1;
        int mi = comp_method_in_chain(c, k, name, &defcls);
        if (mi < 0 || argc < c->scopes[mi].nrequired) continue;
        TyKind mret = c->scopes[mi].ret;
        int mnp = c->scopes[mi].nparams;
        Buf cb; memset(&cb, 0, sizeof cb);
        buf_printf(&cb, "sp_%s_%s((sp_%s *)_t%d.v.p", c->classes[defcls].name,
                   mc(c->scopes[mi].name), c->classes[defcls].name, tv);
        const char *saved_self = g_self;
        static char selfpbuf2[64];
        snprintf(selfpbuf2, sizeof selfpbuf2, "(sp_%s *)_t%d.v.p", c->classes[defcls].name, tv);
        for (int a = 0; a < mnp; a++) {
          /* box the call-site arg if this candidate's parameter is poly;
             emit default for args beyond the call-site count (padding) */
          TyKind pt = TY_UNKNOWN;
          LocalVar *pv = scope_local(&c->scopes[mi], c->scopes[mi].pnames[a]);
          if (pv) pt = pv->type;
          buf_puts(&cb, ", ");
          if (a < argc) {
            TyKind at = infer_type(c, argv[a]);
            char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[a]);
            if (pt == TY_POLY && at != TY_POLY) emit_boxed_text(c, at, tn, &cb);
            else buf_puts(&cb, tn);
          } else {
            g_self = selfpbuf2;
            emit_arg_or_default(c, &c->scopes[mi], a, -1, &cb);
            g_self = saved_self;
          }
        }
        g_self = saved_self;
        buf_puts(&cb, ")");
        buf_printf(b, " case %d: ", k);
        if (mret == TY_VOID || mret == TY_NIL || method_is_void(&c->scopes[mi])) buf_puts(b, cb.p);  /* no usable value */
        else {
          buf_printf(b, "_t%d = ", tr);
          if (ret == TY_POLY && mret != TY_POLY) emit_boxed_text(c, mret, cb.p, b);
          else buf_puts(b, cb.p);
        }
        buf_puts(b, "; break;");
        free(cb.p);
      }
      if (is_index) {
        if (ret == TY_POLY) {
          buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = sp_box_int(sp_IntArray_get((sp_IntArray *)_t%d.v.p, _t%d)); break;", tr, tv, atmp[0]);
          buf_printf(b, " case SP_BUILTIN_STR_ARRAY: _t%d = sp_box_str(sp_StrArray_get((sp_StrArray *)_t%d.v.p, _t%d)); break;", tr, tv, atmp[0]);
          buf_printf(b, " case SP_BUILTIN_FLT_ARRAY: _t%d = sp_box_float(sp_FloatArray_get((sp_FloatArray *)_t%d.v.p, _t%d)); break;", tr, tv, atmp[0]);
          buf_printf(b, " case SP_BUILTIN_POLY_ARRAY: _t%d = sp_PolyArray_get((sp_PolyArray *)_t%d.v.p, _t%d); break;", tr, tv, atmp[0]);
        }
        else {
          buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = sp_IntArray_get((sp_IntArray *)_t%d.v.p, _t%d); break;", tr, tv, atmp[0]);
        }
      }
      /* the poly value may actually be a string-keyed hash: dispatch `[]` /
         `fetch` to the matching hash storage, boxing the value into the poly
         result. */
      int is_aref = !strcmp(name, "[]") && argc == 1;
      int is_fetch = !strcmp(name, "fetch") && (argc == 1 || argc == 2);
      if ((is_aref || is_fetch) && infer_type(c, argv[0]) == TY_STRING) {
        TyKind trt = is_scalar_ret(ret) ? ret : TY_INT;  /* the result temp's type */
        static const struct { const char *cls, *hn; TyKind vt; } HV[] = {
          {"SP_BUILTIN_STR_STR_HASH", "StrStr", TY_STRING},
          {"SP_BUILTIN_STR_INT_HASH", "StrInt", TY_INT},
          {"SP_BUILTIN_STR_POLY_HASH", "StrPoly", TY_POLY},
        };
        for (unsigned hvi = 0; hvi < sizeof HV / sizeof HV[0]; hvi++) {
          /* only a variant whose value fits the result temp can be emitted */
          if (ret != TY_POLY && HV[hvi].vt != trt) continue;
          char getx[200];
          snprintf(getx, sizeof getx, "sp_%sHash_get((sp_%sHash *)_t%d.v.p, _t%d)", HV[hvi].hn, HV[hvi].hn, tv, atmp[0]);
          buf_printf(b, " case %s: _t%d = sp_%sHash_has_key((sp_%sHash *)_t%d.v.p, _t%d) ? ",
                     HV[hvi].cls, tr, HV[hvi].hn, HV[hvi].hn, tv, atmp[0]);
          if (ret == TY_POLY) emit_boxed_text(c, HV[hvi].vt, getx, b); else buf_puts(b, getx);
          buf_puts(b, " : ");
          if (is_fetch && argc == 2) {
            char dn[32]; snprintf(dn, sizeof dn, "_t%d", atmp[1]);
            if (ret == TY_POLY) emit_boxed_text(c, infer_type(c, argv[1]), dn, b); else buf_puts(b, dn);
          }
          else if (is_fetch) { buf_puts(b, "(sp_raise_cls(\"KeyError\", \"key not found\"), "); buf_puts(b, ret == TY_POLY ? "sp_box_nil()" : default_value(trt)); buf_puts(b, ")"); }
          else buf_puts(b, ret == TY_POLY ? "sp_box_nil()" : default_value(trt));
          buf_puts(b, "; break;");
        }
      }
      buf_printf(b, " } _t%d; })", tr);
      free(atmp);
      return;
    }
  }

  /* string-range literal methods: the int-only sp_Range struct can't hold
     string bounds, so inline strcmp / char-iteration for a literal
     `("a".."z")` receiver. */
  if (recv >= 0 && rt == TY_RANGE && nt_type(nt, unwrap_parens(c, recv)) &&
      !strcmp(nt_type(nt, unwrap_parens(c, recv)), "RangeNode")) {
    int rnode = unwrap_parens(c, recv);
    int lo = nt_ref(nt, rnode, "left"), hi = nt_ref(nt, rnode, "right");
    if (lo >= 0 && hi >= 0 && comp_ntype(c, lo) == TY_STRING && comp_ntype(c, hi) == TY_STRING) {
      int excl = (int)(nt_int(nt, rnode, "flags", 0) & 4) ? 1 : 0;
      if ((!strcmp(name, "include?") || !strcmp(name, "member?") ||
           !strcmp(name, "cover?") || !strcmp(name, "===")) && argc == 1) {
        if (a0 != TY_STRING) {
          /* a non-string can't be in a string range: false (eval arg) */
          buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)");
        }
        else {
          int ta = ++g_tmp;
          buf_printf(b, "({ const char *_t%d = ", ta); emit_expr(c, argv[0], b);
          buf_puts(b, "; (strcmp("); emit_expr(c, lo, b); buf_printf(b, ", _t%d) <= 0 && strcmp(_t%d, ", ta, ta);
          emit_expr(c, hi, b); buf_printf(b, ") %s 0); })", excl ? "<" : "<=");
        }
        return;
      }
      if (!strcmp(name, "to_a") && argc == 0) {
        /* single-char ASCII range -> [lo, lo+1, ..., hi(-excl)] */
        int tl = ++g_tmp, th = ++g_tmp, to = ++g_tmp, ci = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = ", tl); emit_expr(c, lo, b);
        buf_printf(b, "; const char *_t%d = ", th); emit_expr(c, hi, b);
        buf_printf(b, "; sp_StrArray *_t%d = sp_StrArray_new();"
                      " for (int _t%d = (unsigned char)_t%d[0]; _t%d <= (unsigned char)_t%d[0] - %d; _t%d++)"
                      " sp_StrArray_push(_t%d, sp_int_chr(_t%d)); _t%d; })",
                   to, ci, tl, ci, th, excl, ci, to, ci, to);
        return;
      }
    }
  }

  /* range value methods (evaluate the range once into a temp) */
  if (recv >= 0 && rt == TY_RANGE) {
    int block = nt_ref(nt, id, "block");
    if (!strcmp(name, "step") && argc == 1) {
      int t = ++g_tmp, ar = ++g_tmp, ii = ++g_tmp, st = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
      Buf sb; memset(&sb, 0, sizeof sb); emit_expr(c, argv[0], &sb);
      buf_printf(b, "({ sp_Range _t%d = %s; mrb_int _t%d = %s; sp_IntArray *_t%d = sp_IntArray_new();"
                    " for (mrb_int _t%d = _t%d.first; _t%d <= _t%d.last - _t%d.excl; _t%d += _t%d)"
                    " sp_IntArray_push(_t%d, _t%d); _t%d; })",
                 t, rb.p ? rb.p : "", st, sb.p ? sb.p : "", ar,
                 ii, t, ii, t, t, ii, st, ar, ii, ar);
      free(rb.p); free(sb.p);
      return;
    }
    if (!strcmp(name, "each") && block < 0) {  /* enumerator: materialize to_a */
      int t = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
      buf_printf(b, "({ sp_Range _t%d = %s; sp_IntArray_from_range(_t%d.first, _t%d.last - _t%d.excl); })",
                 t, rb.p ? rb.p : "", t, t, t);
      free(rb.p);
      return;
    }
    static const char *const rmeths[] = {
      "to_a", "include?", "member?", "cover?", "===", "sum", "min", "max",
      "first", "last", "size", "count", "begin", "end",
      "exclude_end?", "eql?", "minmax", "overlap?", NULL };
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
      else if (!strcmp(name, "include?") || !strcmp(name, "member?") ||
               !strcmp(name, "cover?") || !strcmp(name, "===")) {
        buf_printf(b, "sp_range_include(&_t%d, ", t); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "first") || !strcmp(name, "min") || !strcmp(name, "begin"))
        buf_printf(b, "(_t%d.first)", t);
      else if (!strcmp(name, "max"))  /* max element: end minus the exclusive bound */
        buf_printf(b, "(_t%d.last - _t%d.excl)", t, t);
      else if (!strcmp(name, "last") || !strcmp(name, "end"))  /* the end value itself */
        buf_printf(b, "(_t%d.last)", t);
      else if (!strcmp(name, "size") || !strcmp(name, "count"))
        buf_printf(b, "(_t%d.last - _t%d.excl - _t%d.first + 1)", t, t, t);
      else if (!strcmp(name, "sum"))
        buf_printf(b, "sp_IntArray_sum(sp_IntArray_from_range(_t%d.first, _t%d.last - _t%d.excl), 0)", t, t, t);
      else if (!strcmp(name, "exclude_end?"))
        buf_printf(b, "(_t%d.excl != 0)", t);
      else if (!strcmp(name, "eql?")) {
        buf_printf(b, "sp_range_eq(_t%d, ", t); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "overlap?")) {
        int t2 = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", t2); emit_expr(c, argv[0], b);
        buf_printf(b, "; (_t%d.first <= _t%d.last - _t%d.excl && _t%d.first <= _t%d.last - _t%d.excl); })",
                   t, t2, t2, t2, t, t, t);
      }
      else if (!strcmp(name, "minmax")) {
        int ma = ++g_tmp;
        buf_printf(b, "({ sp_IntArray *_t%d = sp_IntArray_new(); sp_IntArray_push(_t%d, _t%d.first);"
                      " sp_IntArray_push(_t%d, _t%d.last - _t%d.excl); _t%d; })", ma, ma, t, ma, t, t, ma);
      }
      return;
    }
  }

  /* hash value methods */
  /* {}.default (empty hash literal with unknown type) always returns nil */
  if (recv >= 0 && !strcmp(name, "default") && argc == 0 && !ty_is_hash(rt)) {
    buf_puts(b, "sp_box_nil()");
    return;
  }
  if (recv >= 0 && ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    if (hn) {
      if ((!strcmp(name, "dup") || !strcmp(name, "clone")) && argc == 0) {
        buf_printf(b, "sp_%sHash_dup(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "[]") && argc == 1) {
        if (rt == TY_POLY_POLY_HASH) {
          buf_printf(b, "sp_%sHash_get(", hn);
          emit_expr(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
        }
        else {
          /* int-valued hashes have a nullable get_opt; string-valued use get */
          const char *getter = ty_hash_val(rt) == TY_INT ? "get_opt" : "get";
          buf_printf(b, "sp_%sHash_%s(", hn, getter);
          emit_expr(c, recv, b); buf_puts(b, ", "); emit_hash_key(c, argv[0], ty_hash_key(rt), b); buf_puts(b, ")");
        }
        return;
      }
      if (!strcmp(name, "dig") && argc >= 1) {
        TyKind vt = ty_hash_val(rt);
        TyKind kt = ty_hash_key(rt);
        /* Static key-type mismatch (string key on sym hash, etc.) -> nil. */
        TyKind arg0t = comp_ntype(c, argv[0]);
        if ((kt == TY_SYMBOL && arg0t == TY_STRING) ||
            (kt == TY_STRING && arg0t == TY_SYMBOL)) {
          if (vt == TY_INT) buf_puts(b, "SP_INT_NIL");
          else if (vt == TY_STRING) buf_puts(b, "NULL");
          else buf_puts(b, "sp_box_nil()");
          return;
        }
        const char *getter = vt == TY_INT ? "get_opt" : "get";
        if (argc == 1) {
          buf_printf(b, "sp_%sHash_%s(", hn, getter);
          emit_expr(c, recv, b); buf_puts(b, ", "); emit_hash_key(c, argv[0], kt, b); buf_puts(b, ")");
        }
        else {
          /* multi-step dig: use a compound statement to guarantee
             left-to-right key-expression evaluation order. */
          int tr = ++g_tmp, th = ++g_tmp;
          buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th);
          emit_expr(c, recv, b); buf_puts(b, ";");
          /* first key -> box to sp_RbVal so remaining steps are uniform */
          buf_printf(b, " sp_RbVal _t%d = ", tr);
          if (vt == TY_INT) {
            int tk0 = ++g_tmp;
            buf_printf(b, "({ mrb_int _t%d = sp_%sHash_%s(_t%d, ", tk0, hn, getter, th);
            emit_hash_key(c, argv[0], kt, b);
            buf_printf(b, "); _t%d == SP_INT_NIL ? sp_box_nil() : sp_box_int(_t%d); });", tk0, tk0);
          }
          else if (vt == TY_STRING) {
            int tk0 = ++g_tmp;
            buf_printf(b, "({ const char *_t%d = sp_%sHash_%s(_t%d, ", tk0, hn, getter, th);
            emit_hash_key(c, argv[0], kt, b);
            buf_printf(b, "); _t%d ? sp_box_str(_t%d) : sp_box_nil(); });", tk0, tk0);
          }
          else {
            /* TY_POLY: getter already returns sp_RbVal */
            buf_printf(b, "sp_%sHash_%s(_t%d, ", hn, getter, th);
            emit_hash_key(c, argv[0], kt, b);
            buf_puts(b, ");");
          }
          /* remaining keys via sp_poly_get_sym / sp_poly_get_str / sp_poly_arr_get */
          for (int di = 1; di < argc; di++) {
            int tk = ++g_tmp;
            if (kt == TY_SYMBOL) {
              buf_printf(b, " sp_sym _t%d = ", tk);
              emit_expr(c, argv[di], b);
              buf_printf(b, "; _t%d = sp_poly_get_sym(_t%d, _t%d);", tr, tr, tk);
            }
            else if (kt == TY_STRING) {
              buf_printf(b, " const char *_t%d = ", tk);
              emit_expr(c, argv[di], b);
              buf_printf(b, "; _t%d = sp_poly_get_str(_t%d, _t%d);", tr, tr, tk);
            }
            else {
              buf_printf(b, " mrb_int _t%d = ", tk);
              emit_expr(c, argv[di], b);
              buf_printf(b, "; _t%d = sp_poly_arr_get(_t%d, _t%d);", tr, tr, tk);
            }
          }
          buf_printf(b, " _t%d; })", tr);
        }
        return;
      }
      if ((!strcmp(name, "values_at") || !strcmp(name, "fetch_values")) && argc >= 1) {
        /* collect looked-up values into a poly array; values_at yields nil for
           a missing key, fetch_values raises KeyError */
        int is_fetch = !strcmp(name, "fetch_values");
        TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
        int th = ++g_tmp, tr = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
        buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
        for (int a = 0; a < argc; a++) {
          int tk = ++g_tmp;
          buf_printf(b, " %s _t%d = ", c_type_name(kt), tk); emit_hash_key(c, argv[a], kt, b); buf_puts(b, ";");
          buf_printf(b, " if (sp_%sHash_has_key(_t%d, _t%d)) sp_PolyArray_push(_t%d, ", hn, th, tk, tr);
          char getexpr[128]; snprintf(getexpr, sizeof getexpr, "sp_%sHash_get(_t%d, _t%d)", hn, th, tk);
          if (vt == TY_POLY) buf_puts(b, getexpr);
          else emit_boxed_text(c, vt, getexpr, b);
          buf_puts(b, ");");
          if (is_fetch) buf_puts(b, " else sp_raise_cls(\"KeyError\", \"key not found\");");
          else buf_printf(b, " else sp_PolyArray_push(_t%d, sp_box_nil());", tr);
        }
        buf_printf(b, " _t%d; })", tr);
        return;
      }
      if (!strcmp(name, "fetch") && argc == 1) {
        int blk = nt_ref(nt, id, "block");
        if (blk >= 0) {
          /* fetch(key) { default } -> has_key? ? get : block-default */
          TyKind vt = ty_hash_val(rt);
          int th = ++g_tmp, tk = ++g_tmp;
          buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
          buf_printf(b, "; %s _t%d = ", c_type_name(ty_hash_key(rt)), tk); emit_hash_key(c, argv[0], ty_hash_key(rt), b);
          int bbody = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
          int bval = bn > 0 ? bb[bn - 1] : -1;
          TyKind bvt = bval >= 0 ? comp_ntype(c, bval) : vt;
          /* When the block's return type differs from the hash value type,
             box both arms so the ternary produces a consistent sp_RbVal. */
          int mismatch = vt != TY_POLY && bvt != vt;
          if (mismatch) {
            buf_printf(b, "; sp_%sHash_has_key(_t%d, _t%d) ? ", hn, th, tk);
            char getexpr[128]; snprintf(getexpr, sizeof getexpr, "sp_%sHash_get(_t%d, _t%d)", hn, th, tk);
            emit_boxed_text(c, vt, getexpr, b);
            buf_puts(b, " : ({ ");
          } else {
            buf_printf(b, "; sp_%sHash_has_key(_t%d, _t%d) ? sp_%sHash_get(_t%d, _t%d) : ({ ",
                       hn, th, tk, hn, th, tk);
          }
          const char *fp0 = block_param_name(c, blk, 0);  /* fetch yields the key */
          if (fp0) { buf_printf(b, "lv_%s = _t%d; ", rename_local(fp0), tk); }
          for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], b, 0);  /* leading stmts */
          if (bval >= 0) {
            if ((vt == TY_POLY || mismatch) && bvt != TY_POLY) emit_boxed(c, bval, b);
            else emit_expr(c, bval, b);
          }
          else buf_puts(b, (vt == TY_POLY || mismatch) ? "sp_box_nil()" : default_value(vt));
          buf_printf(b, "; }); })");
          return;
        }
        /* fetch(key) with no default raises KeyError on a miss */
        TyKind vt = ty_hash_val(rt);
        int th = ++g_tmp, tk = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
        buf_printf(b, "; %s _t%d = ", c_type_name(ty_hash_key(rt)), tk); emit_hash_key(c, argv[0], ty_hash_key(rt), b);
        buf_printf(b, "; sp_%sHash_has_key(_t%d, _t%d) ? sp_%sHash_get(_t%d, _t%d)"
                      " : (sp_raise_cls(\"KeyError\", \"key not found\"), %s); })",
                   hn, th, tk, hn, th, tk, vt == TY_POLY ? "sp_box_nil()" : default_value(vt));
        return;
      }
      if (!strcmp(name, "fetch") && argc == 2) {
        /* fetch(key, default) -> has_key? ? value : default */
        TyKind vt = ty_hash_val(rt);
        int th = ++g_tmp, tk = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
        buf_printf(b, "; %s _t%d = ", c_type_name(ty_hash_key(rt)), tk); emit_hash_key(c, argv[0], ty_hash_key(rt), b);
        buf_printf(b, "; sp_%sHash_has_key(_t%d, _t%d) ? sp_%sHash_get(_t%d, _t%d) : ", hn, th, tk, hn, th, tk);
        if (vt == TY_POLY && comp_ntype(c, argv[1]) != TY_POLY) emit_boxed(c, argv[1], b);
        else emit_expr(c, argv[1], b);
        buf_puts(b, "; })");
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
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_hash_key(c, argv[0], ty_hash_key(rt), b); buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "value?") || !strcmp(name, "has_value?")) && argc == 1) {
        int poly = (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH);
        buf_printf(b, "sp_%sHash_has_value(", hn);
        emit_expr(c, recv, b); buf_puts(b, ", ");
        if (poly) emit_boxed(c, argv[0], b); else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "replace") && argc == 1 && comp_ntype(c, argv[0]) == rt) {
        buf_printf(b, "sp_%sHash_replace(", hn);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "default") && argc == 0) {
        if (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH) {
          int t = ++g_tmp;
          buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t); emit_expr(c, recv, b);
          buf_printf(b, "; _t%d ? _t%d->default_v : sp_box_nil(); })", t, t);
        } else {
          buf_puts(b, "sp_box_nil()");
          (void)recv;
        }
        return;
      }
      if (!strcmp(name, "keys") && argc == 0 && rt == TY_SYM_POLY_HASH) {
        /* runtime returns sym ids as an IntArray; box into a poly (sym) array */
        int ki = ++g_tmp, kp = ++g_tmp, ii = ++g_tmp;
        buf_printf(b, "({ sp_IntArray *_t%d = sp_SymPolyHash_keys(", ki); emit_expr(c, recv, b);
        buf_printf(b, "); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", kp, kp);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++)"
                      " sp_PolyArray_push(_t%d, sp_box_sym((sp_sym)sp_IntArray_get(_t%d, _t%d)));",
                   ii, ii, ki, ii, kp, ki, ii);
        buf_printf(b, " _t%d; })", kp);
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
      if (!strcmp(name, "merge") && argc == 1 && nt_ref(nt, id, "block") >= 0) {
        /* merge(other) { |k, v1, v2| } -- conflict-resolution block. The
           result starts as a copy of the receiver, then each key of `other`
           is inserted; on a collision the block picks the value. */
        int blk = nt_ref(nt, id, "block");
        const char *bp0 = block_param_name(c, blk, 0);
        const char *bp1 = block_param_name(c, blk, 1);
        const char *bp2 = block_param_name(c, blk, 2);
        TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
        int tr = ++g_tmp, to = ++g_tmp, ti = ++g_tmp, tk = ++g_tmp, tc = ++g_tmp, tj = ++g_tmp;
        buf_printf(b, "({ %s _t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);", c_type_name(rt), tr, hn, tr);
        /* copy the receiver into the fresh result */
        buf_printf(b, " %s _t%d = ", c_type_name(rt), tc); emit_expr(c, recv, b); buf_puts(b, ";");
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                      " sp_%sHash_set(_t%d, _t%d->order[_t%d], sp_%sHash_get(_t%d, _t%d->order[_t%d]));",
                   tj, tj, tc, tj, hn, tr, tc, tj, hn, tc, tc, tj);
        buf_printf(b, " %s _t%d = ", c_type_name(rt), to); emit_expr(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, to, ti);
        buf_printf(b, " %s _t%d = _t%d->order[_t%d];", c_type_name(kt), tk, to, ti);
        buf_printf(b, " if (sp_%sHash_has_key(_t%d, _t%d)) {", hn, tr, tk);
        if (bp0) buf_printf(b, " lv_%s = _t%d;", rename_local(bp0), tk);
        if (bp1) buf_printf(b, " lv_%s = sp_%sHash_get(_t%d, _t%d);", rename_local(bp1), hn, tr, tk);
        if (bp2) buf_printf(b, " lv_%s = sp_%sHash_get(_t%d, _t%d);", rename_local(bp2), hn, to, tk);
        buf_printf(b, " sp_%sHash_set(_t%d, _t%d, ", hn, tr, tk);
        {
          int bbody = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
          int bval = bn > 0 ? bb[bn - 1] : -1;
          buf_puts(b, "({ ");
          for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], b, 0);
          if (bval >= 0) {
            if (vt == TY_POLY && comp_ntype(c, bval) != TY_POLY) emit_boxed(c, bval, b);
            else emit_expr(c, bval, b);
          }
          else buf_puts(b, vt == TY_POLY ? "sp_box_nil()" : default_value(vt));
          buf_puts(b, "; })");
        }
        buf_printf(b, "); } else { sp_%sHash_set(_t%d, _t%d, sp_%sHash_get(_t%d, _t%d)); } }", hn, tr, tk, hn, to, tk);
        buf_printf(b, " _t%d; })", tr);
        return;
      }
      if (!strcmp(name, "merge") && argc == 1 &&
          (rt == TY_STR_INT_HASH || rt == TY_STR_POLY_HASH || rt == TY_SYM_POLY_HASH ||
           rt == TY_STR_STR_HASH)) {
        TyKind at = comp_ntype(c, argv[0]);
        /* cross-variant str merge: promote both sides to str_poly_hash */
        if ((rt == TY_STR_INT_HASH || rt == TY_STR_STR_HASH) &&
            ty_is_hash(at) && ty_hash_key(at) == TY_STRING && at != rt) {
          buf_puts(b, "sp_StrPolyHash_merge(");
          const char *rfn = rt == TY_STR_INT_HASH ? "sp_StrPolyHash_from_str_int_hash("
                                                   : "sp_StrPolyHash_from_str_str_hash(";
          buf_puts(b, rfn); emit_expr(c, recv, b); buf_puts(b, "), ");
          const char *afn = at == TY_STR_INT_HASH ? "sp_StrPolyHash_from_str_int_hash("
                          : at == TY_STR_STR_HASH  ? "sp_StrPolyHash_from_str_str_hash("
                                                   : NULL;
          if (afn) { buf_puts(b, afn); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
          else { emit_expr(c, argv[0], b); }
          buf_puts(b, ")");
          return;
        }
        buf_printf(b, "sp_%sHash_merge(", hn); emit_expr(c, recv, b); buf_puts(b, ", ");
        /* a str_poly receiver may be merged with a concrete str-keyed hash;
           coerce the argument to the receiver's variant first */
        if (rt == TY_STR_POLY_HASH && (at == TY_STR_STR_HASH || at == TY_STR_INT_HASH)) {
          buf_printf(b, "sp_StrPolyHash_from_%s(", at == TY_STR_STR_HASH ? "str_str_hash" : "str_int_hash");
          emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else if (at == TY_POLY) {
          /* poly arg: unbox to the receiver's hash type */
          int t = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", t); emit_expr(c, argv[0], b);
          buf_printf(b, "; (sp_%sHash*)_t%d.v.p; })", hn, t);
        }
        else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "delete") && argc == 1 &&
          (rt == TY_STR_INT_HASH || rt == TY_STR_STR_HASH || rt == TY_SYM_POLY_HASH)) {
        /* returns the deleted value (or nil on a miss), then removes the key */
        TyKind vt = ty_hash_val(rt);
        int th = ++g_tmp, tk = ++g_tmp, tv = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
        buf_printf(b, "; %s _t%d = ", c_type_name(ty_hash_key(rt)), tk); emit_hash_key(c, argv[0], ty_hash_key(rt), b);
        buf_printf(b, "; %s _t%d = sp_%sHash_has_key(_t%d, _t%d) ? sp_%sHash_get(_t%d, _t%d) : %s;",
                   c_type_name(vt), tv, hn, th, tk, hn, th, tk, vt == TY_POLY ? "sp_box_nil()" : default_value(vt));
        buf_printf(b, " sp_%sHash_delete(_t%d, _t%d); _t%d; })", hn, th, tk, tv);
        return;
      }
    }
  }

  /* `arr[i] = v` in expression position: do the store, evaluate to the rhs
     (Ruby []= returns the assigned value). The statement form is emitted
     elsewhere; this covers rvalue chains like `b = arr[i] = v`. */
  /* a[i, n] = src  —  slice assignment (same-length only) */
  if (recv >= 0 && ty_is_array(rt) && !strcmp(name, "[]=") && argc == 3) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (k) {
      int ta = ++g_tmp, ti = ++g_tmp, ts = ++g_tmp, tj = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
      buf_printf(b, "; mrb_int _t%d = ", ti); emit_expr(c, argv[0], b); buf_puts(b, "; ");
      buf_printf(b, "sp_%sArray *_t%d = ", k, ts); emit_expr(c, argv[2], b); buf_puts(b, "; ");
      buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++)", tj, tj, k, ts, tj);
      if (rt == TY_POLY_ARRAY)
        buf_printf(b, " sp_%sArray_set(_t%d, _t%d + _t%d, sp_%sArray_get(_t%d, _t%d));", k, ta, ti, tj, k, ts, tj);
      else
        buf_printf(b, " sp_%sArray_set(_t%d, _t%d + _t%d, sp_%sArray_get(_t%d, _t%d));", k, ta, ti, tj, k, ts, tj);
      buf_printf(b, " _t%d; })", ts);
      return;
    }
  }
  if (recv >= 0 && ty_is_array(rt) && !strcmp(name, "[]=") && argc == 2) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (k) {
      int t = ++g_tmp, ti = ++g_tmp, tv = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
      buf_printf(b, "; mrb_int _t%d = ", ti); emit_expr(c, argv[0], b); buf_puts(b, "; ");
      if (rt == TY_POLY_ARRAY) {
        buf_printf(b, "sp_RbVal _t%d = ", tv); emit_boxed(c, argv[1], b);
      }
      else { emit_ctype(c, ty_array_elem(rt), b); buf_printf(b, " _t%d = ", tv); emit_expr(c, argv[1], b); }
      buf_printf(b, "; sp_%sArray_set(_t%d, _t%d, _t%d); _t%d; })", k, t, ti, tv, tv);
      return;
    }
  }

  /* array value methods */
  /* empty array literal [] has TY_UNKNOWN; sum returns init or 0 */
  if (recv >= 0 && rt == TY_UNKNOWN && !strcmp(name, "sum") &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ArrayNode")) {
    int en = 0; nt_arr(nt, recv, "elements", &en);
    if (en == 0) {
      TyKind call_t = comp_ntype(c, id);
      if (argc == 1) {
        if (call_t == TY_POLY) emit_boxed(c, argv[0], b);
        else emit_expr(c, argv[0], b);
      }
      else {
        if (call_t == TY_POLY) buf_puts(b, "sp_box_int(0)");
        else buf_puts(b, "0");
      }
      return;
    }
  }
  if (recv >= 0 && ty_is_array(rt)) {
    if (!strcmp(name, "pack") && argc == 1 && (rt == TY_INT_ARRAY || rt == TY_POLY_ARRAY)) {
      buf_printf(b, "sp_%sArray_pack(", rt == TY_POLY_ARRAY ? "Poly" : "Int");
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    /* values_at(i, j, ...) -> fresh same-kind array of the picked elements
       (works for typed and poly arrays alike) */
    if (!strcmp(name, "values_at") && argc >= 1) {
      const char *an = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
      if (an) {
        int tr = ++g_tmp, to = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", an, tr); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray *_t%d = sp_%sArray_new(); ", an, to, an);
        for (int a = 0; a < argc; a++) {
          buf_printf(b, "sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, ", an, to, an, tr);
          emit_expr(c, argv[a], b); buf_puts(b, ")); ");
        }
        buf_printf(b, "_t%d; })", to);
        return;
      }
    }
    const char *k = array_kind(rt);
    /* fill(val[, start[, len]]): fill a range with val, evaluate to self. */
    if (!strcmp(name, "fill") && argc >= 1 && argc <= 3) {
      const char *fk = (rt == TY_POLY_ARRAY) ? "Poly" : k;
      if (fk) {
        int t = ++g_tmp, ti = ++g_tmp, tv = ++g_tmp, tn = ++g_tmp, ts = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", fk, t); emit_expr(c, recv, b); buf_puts(b, "; ");
        emit_ctype(c, ty_array_elem(rt), b); buf_printf(b, " _t%d = ", tv);
        if (rt == TY_POLY_ARRAY) emit_boxed(c, argv[0], b); else emit_expr(c, argv[0], b);
        buf_printf(b, "; mrb_int _t%d = sp_%sArray_length(_t%d);", tn, fk, t);
        if (argc >= 2) {
          buf_printf(b, " mrb_int _t%d = ", ts); emit_expr(c, argv[1], b);
          buf_printf(b, "; if (_t%d < 0) _t%d += _t%d; if (_t%d < 0) _t%d = 0;", ts, ts, tn, ts, ts);
          if (argc == 3) {
            int tl = ++g_tmp;
            buf_printf(b, " mrb_int _t%d = ", tl); emit_expr(c, argv[2], b);
            /* end = start+len; negative len = no-op (empty range) */
            buf_printf(b, "; if (_t%d < 0) _t%d = 0; _t%d = _t%d + _t%d;",
                       tl, tl, tn, ts, tl);
          }
          buf_printf(b, " for (mrb_int _t%d = _t%d; _t%d < _t%d; _t%d++)"
                        " sp_%sArray_set(_t%d, _t%d, _t%d); _t%d; })",
                     ti, ts, ti, tn, ti, fk, t, ti, tv, t);
        }
        else {
          buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)"
                        " sp_%sArray_set(_t%d, _t%d, _t%d); _t%d; })",
                     ti, ti, tn, ti, fk, t, ti, tv, t);
        }
        return;
      }
    }
    if (rt == TY_POLY_ARRAY && !strcmp(name, "sum") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      buf_puts(b, "sp_PolyArray_sum_int("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (rt == TY_POLY_ARRAY && !strcmp(name, "sum") && argc == 1 && nt_ref(nt, id, "block") < 0) {
      TyKind init_t = comp_ntype(c, argv[0]);
      buf_puts(b, "(");
      if (init_t == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else { emit_expr(c, argv[0], b); }
      buf_puts(b, " + sp_PolyArray_sum_int("); emit_expr(c, recv, b); buf_puts(b, "))");
      return;
    }
    if (rt == TY_POLY_ARRAY && (!strcmp(name, "shift") || !strcmp(name, "pop")) && argc == 0) {
      buf_printf(b, "sp_PolyArray_%s(", name); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (rt == TY_POLY_ARRAY && !strcmp(name, "dig") && argc >= 1) {
      if (argc == 1) {
        buf_puts(b, "sp_PolyArray_get("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else {
        for (int di = argc - 1; di >= 1; di--) buf_printf(b, "sp_poly_arr_get(");
        buf_puts(b, "sp_PolyArray_get("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        for (int di = 1; di < argc; di++) { buf_puts(b, ", "); emit_expr(c, argv[di], b); buf_puts(b, ")"); }
      }
      return;
    }
    if (k) {
      if ((!strcmp(name, "to_a") || !strcmp(name, "to_ary") || !strcmp(name, "entries") ||
           !strcmp(name, "flatten") || !strcmp(name, "compact")) && argc == 0) {
        /* a scalar-element array can't nest or hold nil: these are identity */
        emit_expr(c, recv, b); return;
      }
      if (!strcmp(name, "[]") && argc == 1 && nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "RangeNode")) {
        /* arr[a..b] / arr[a...b] -> subarray */
        int rn = argv[0];
        int excl = (int)(nt_int(nt, rn, "flags", 0) & 4) ? 1 : 0;
        int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
        buf_printf(b, "sp_%sArray_slice_range(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (lo >= 0) emit_expr(c, lo, b); else buf_puts(b, "0");
        buf_puts(b, ", ");
        if (hi >= 0) emit_expr(c, hi, b); else buf_puts(b, "-1");
        buf_printf(b, ", %d)", hi >= 0 ? excl : 0);
        return;
      }
      if (!strcmp(name, "[]") && argc == 2) {
        /* arr[start, len] -> subarray */
        buf_printf(b, "sp_%sArray_slice(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "[]") || !strcmp(name, "at")) && argc == 1) {
        buf_printf(b, "sp_%sArray_get(", k);
        emit_expr(c, recv, b); buf_puts(b, ", ");
        if (infer_type(c, argv[0]) == TY_POLY) {
          int t = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", t);
          emit_expr(c, argv[0], b);
          buf_printf(b, "; _t%d.v.i; })", t);
        } else {
          emit_expr(c, argv[0], b);
        }
        buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "dig") && argc >= 1) {
        if (argc == 1) {
          /* single-step: same as arr[i] */
          buf_printf(b, "sp_%sArray_get(", k); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else {
          /* multi-step: box the array as sp_RbVal, then chain sp_poly_arr_get */
          buf_printf(b, "sp_poly_arr_get(");
          /* first step: box the typed array as obj, then get element i */
          int is_int = (rt == TY_INT_ARRAY);
          (void)is_int;
          /* build chain from innermost outward */
          for (int di = argc - 1; di >= 1; di--) {
            buf_printf(b, "sp_poly_arr_get(");
          }
          /* first access: typed get then box */
          buf_printf(b, "sp_box_obj(");
          emit_expr(c, recv, b);
          buf_printf(b, ", SP_BUILTIN_%s_ARRAY)", rt == TY_INT_ARRAY ? "INT" : rt == TY_FLOAT_ARRAY ? "FLT" : "STR");
          buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
          for (int di = 1; di < argc; di++) {
            buf_puts(b, ", "); emit_expr(c, argv[di], b); buf_puts(b, ")");
          }
        }
        return;
      }
      if (!strcmp(name, "+") && argc == 1 && a0 == rt) {
        /* array + array of the same kind -> a fresh concatenation */
        buf_printf(b, "sp_%sArray_concat(", k);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "clear") && argc == 0) {
        /* empty the array in place, evaluate to it (Ruby returns self) */
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; if (_t%d) _t%d->len = 0; _t%d; })", t, t, t);
        return;
      }
      if ((!strcmp(name, "shift") || !strcmp(name, "pop")) && argc == 0) {
        /* remove and return first/last element (nil sentinel when empty) */
        buf_printf(b, "sp_%sArray_%s(", k, name); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      /* non-mutating copy-then-operate methods */
      if (!strcmp(name, "shuffle") && argc == 0) {
        buf_printf(b, "sp_%sArray_shuffle(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      /* in-place mutators that return self (raise FrozenError when frozen) */
      {
        const char *base = NULL;
        if      (!strcmp(name, "reverse!")) base = "reverse_bang";
        else if (!strcmp(name, "sort!"))    base = "sort_bang";
        else if (!strcmp(name, "shuffle!")) base = "shuffle_bang";
        else if (!strcmp(name, "uniq!"))    base = "uniq_bang";
        if (base && argc == 0) {
          int t = ++g_tmp;
          buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
          buf_printf(b, "; sp_%sArray_%s(_t%d); _t%d; })", k, base, t, t);
          return;
        }
      }
      if (!strcmp(name, "rotate!") && argc <= 1) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray_rotate_bang(_t%d, ", k, t);
        if (argc == 1) emit_expr(c, argv[0], b); else buf_puts(b, "1");
        buf_printf(b, "); _t%d; })", t);
        return;
      }
      if (!strcmp(name, "replace") && argc == 1 && a0 == rt) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray_replace(_t%d, ", k, t); emit_expr(c, argv[0], b);
        buf_printf(b, "); _t%d; })", t);
        return;
      }
      if (!strcmp(name, "insert") && argc == 2 && (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY)) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray_insert(_t%d, ", k, t); emit_expr(c, argv[0], b);
        buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_printf(b, "); _t%d; })", t);
        return;
      }
      if (!strcmp(name, "delete_at") && argc == 1) {
        buf_printf(b, "sp_%sArray_delete_at(", k); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "delete") && argc == 1 && (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY)) {
        buf_printf(b, "sp_%sArray_delete(", k); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "tally") && argc == 0) {
        if (rt == TY_INT_ARRAY) { buf_printf(b, "sp_IntArray_tally_int("); emit_expr(c, recv, b); buf_puts(b, ")"); return; }
        if (rt == TY_STR_ARRAY) { buf_printf(b, "sp_StrArray_tally("); emit_expr(c, recv, b); buf_puts(b, ")"); return; }
      }
      if (!strcmp(name, "slice!") && argc == 2) {
        /* slice!(start, len): remove and return the subarray (raises
           FrozenError inside the runtime helper when the array is frozen) */
        buf_printf(b, "sp_%sArray_slice_bang(", k); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
        return;
      }
      int block = nt_ref(nt, id, "block");
      /* bsearch { |x| cond } on typed arrays - find-minimum mode */
      if (!strcmp(name, "bsearch") && block >= 0) {
        const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          TyKind et = ty_array_elem(rt);
          int trecv = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp, tres = ++g_tmp, tmid = ++g_tmp;
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = ", trecv); emit_expr(c, recv, g_pre); buf_puts(g_pre, ";\n");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = 0, _t%d = sp_%sArray_length(_t%d) - 1;\n", tlo, thi, k, trecv);
          emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", tres, et == TY_INT ? "SP_INT_NIL" : "NULL");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "while (_t%d <= _t%d) {\n", tlo, thi);
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "mrb_int _t%d = _t%d + (_t%d - _t%d) / 2;\n", tmid, tlo, thi, tlo);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, tmid); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf cb; memset(&cb, 0, sizeof cb); emit_expr(c, bb[bn - 1], &cb); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "if (%s) { _t%d = sp_%sArray_get(_t%d, _t%d); _t%d = _t%d - 1; }\n",
                     cb.p ? cb.p : "0", tres, k, trecv, tmid, thi, tmid);
          free(cb.p);
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d = _t%d + 1; }\n", tlo, tmid);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tres); return;
        }
      }
      /* find_index { |x| cond } on typed arrays - returns index or SP_INT_NIL */
      if (!strcmp(name, "find_index") && block >= 0) {
        const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp;
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = ", trecv); emit_expr(c, recv, g_pre); buf_puts(g_pre, ";\n");
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = SP_INT_NIL;\n", tres);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, k, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf cb; memset(&cb, 0, sizeof cb); emit_expr(c, bb[bn - 1], &cb); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "if (%s) { _t%d = _t%d; break; }\n", cb.p ? cb.p : "0", tres, ti);
          free(cb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tres); return;
        }
      }
      /* map! / collect! { |x| body } - in-place transform, returns receiver */
      if ((!strcmp(name, "map!") || !strcmp(name, "collect!")) && block >= 0) {
        const char *bp0 = block_param_name(c, block, 0);
        const char *bp = bp0 ? rename_local(bp0) : NULL;
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          TyKind et = ty_array_elem(rt);
          Scope *ms = comp_scope_of(c, block);
          LocalVar *mlv = (ms && bp0) ? scope_local(ms, bp0) : NULL;
          TyKind msaved = mlv ? mlv->type : TY_UNKNOWN;
          if (mlv) { mlv->type = et; for (int j = 0; j < bn; j++) infer_type(c, bb[j]); }
          int trecv = ++g_tmp, ti = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, k, trecv, ti);
          if (bp) {
            emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre);
            buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti);
          }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "sp_%sArray_set(_t%d, _t%d, %s);\n", k, trecv, ti, vb.p ? vb.p : "0");
          free(vb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          if (mlv) mlv->type = msaved;
          buf_printf(b, "_t%d", trecv); return;
        }
      }
      /* select! / filter! / keep_if / reject! / delete_if { |x| cond } - in-place filter */
      if ((!strcmp(name, "select!") || !strcmp(name, "filter!") || !strcmp(name, "keep_if") ||
           !strcmp(name, "reject!") || !strcmp(name, "delete_if")) && block >= 0) {
        int is_rej = !strcmp(name, "reject!") || !strcmp(name, "delete_if");
        const char *bp0 = block_param_name(c, block, 0);
        const char *bp = bp0 ? rename_local(bp0) : NULL;
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          TyKind et = ty_array_elem(rt);
          Scope *fs = comp_scope_of(c, block);
          LocalVar *flv = (fs && bp0) ? scope_local(fs, bp0) : NULL;
          TyKind fsaved = flv ? flv->type : TY_UNKNOWN;
          if (flv) { flv->type = et; for (int j = 0; j < bn; j++) infer_type(c, bb[j]); }
          int trecv = ++g_tmp, ti = ++g_tmp, twp = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = 0;\n", twp);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, k, trecv, ti);
          emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre);
          buf_printf(g_pre, " _telt%d = sp_%sArray_get(_t%d, _t%d);\n", ti, k, trecv, ti);
          if (bp) {
            emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre);
            buf_printf(g_pre, " lv_%s = _telt%d;\n", bp, ti);
          }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf cb; memset(&cb, 0, sizeof cb); emit_expr(c, bb[bn - 1], &cb); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          if (is_rej)
            buf_printf(g_pre, "if (!(%s)) { sp_%sArray_set(_t%d, _t%d, _telt%d); _t%d++; }\n",
                       cb.p ? cb.p : "0", k, trecv, twp, ti, twp);
          else
            buf_printf(g_pre, "if (%s) { sp_%sArray_set(_t%d, _t%d, _telt%d); _t%d++; }\n",
                       cb.p ? cb.p : "0", k, trecv, twp, ti, twp);
          free(cb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "if (_t%d) _t%d->len = _t%d;\n", trecv, trecv, twp);
          if (flv) flv->type = fsaved;
          buf_printf(b, "_t%d", trecv); return;
        }
      }
      /* each_index { |i| ... } - iterate with index */
      if (!strcmp(name, "each_index") && block >= 0) {
        /* statement-mode: just emit the loop and return the receiver */
        const char *ip = block_param_name(c, block, 0); if (ip) ip = rename_local(ip);
        int body = nt_ref(nt, block, "body");
        int trecv = ++g_tmp, ti = ++g_tmp;
        Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
        emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
        buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                   ti, ti, k, trecv, ti);
        if (ip) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = _t%d;\n", ip, ti); }
        emit_stmts(c, body, g_pre, g_indent + 1);
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        buf_printf(b, "_t%d", trecv); return;
      }
      if ((!strcmp(name, "all?") || !strcmp(name, "any?") ||
           !strcmp(name, "none?") || !strcmp(name, "one?")) &&
          argc == 0 && nt_ref(nt, id, "block") < 0) {
        /* scalar-element arrays never hold nil/false: predicate is length-based */
        const char *op = !strcmp(name, "all?") ? ">= 0" : !strcmp(name, "any?") ? "> 0"
                       : !strcmp(name, "none?") ? "== 0" : "== 1";
        buf_printf(b, "(sp_%sArray_length(", k); emit_expr(c, recv, b); buf_printf(b, ") %s)", op);
        return;
      }
      if ((!strcmp(name, "length") || !strcmp(name, "size") || !strcmp(name, "count")) &&
          argc == 0 && nt_ref(nt, id, "block") < 0) {
        buf_printf(b, "sp_%sArray_length(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "count") && argc == 0 && nt_ref(nt, id, "block") >= 0) {
        /* count { |x| cond } -- loop and count truthy block results */
        int blk = nt_ref(nt, id, "block");
        const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
        int body2 = nt_ref(nt, blk, "body");
        int bn2 = 0; const int *bb2 = body2 >= 0 ? nt_arr(nt, body2, "body", &bn2) : NULL;
        if (bn2 > 0) {
          int trecv = ++g_tmp, tcnt = ++g_tmp, ti = ++g_tmp;
          Buf rb2; memset(&rb2, 0, sizeof rb2); emit_expr(c, recv, &rb2);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rb2.p ? rb2.p : ""); free(rb2.p);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = 0;\n", tcnt);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, k, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti); }
          for (int j = 0; j < bn2 - 1; j++) emit_stmt(c, bb2[j], g_pre, g_indent + 1);
          int saveI = g_indent; g_indent = g_indent + 1;
          Buf vb2; memset(&vb2, 0, sizeof vb2); emit_expr(c, bb2[bn2 - 1], &vb2);
          g_indent = saveI;
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "if (%s) _t%d++;\n", vb2.p ? vb2.p : "0", tcnt);
          free(vb2.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tcnt);
          return;
        }
      }
      if (!strcmp(name, "empty?") && argc == 0) {
        buf_printf(b, "(sp_%sArray_length(", k); emit_expr(c, recv, b); buf_puts(b, ") == 0)");
        return;
      }
      if (!strcmp(name, "sum") && argc == 0) {
        buf_printf(b, "sp_%sArray_sum(", k); emit_expr(c, recv, b); buf_puts(b, ", 0)");
        return;
      }
      if (!strcmp(name, "sum") && argc == 1 && nt_ref(nt, id, "block") < 0) {
        TyKind init_t = comp_ntype(c, argv[0]);
        buf_printf(b, "sp_%sArray_sum(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (rt == TY_FLOAT_ARRAY && init_t == TY_INT) {
          buf_puts(b, "(mrb_float)("); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else if (rt == TY_FLOAT_ARRAY && init_t == TY_POLY) {
          buf_puts(b, "sp_poly_to_f("); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else if (rt == TY_INT_ARRAY && init_t == TY_POLY) {
          buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else {
          emit_expr(c, argv[0], b);
        }
        buf_puts(b, ")");
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
      if (!strcmp(name, "first") && argc == 1) {
        buf_printf(b, "sp_%sArray_slice(", k); emit_expr(c, recv, b); buf_puts(b, ", 0, ");
        emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "last") && argc == 1) {
        /* slice's negative start counts from the end -> the last n elements */
        int tn = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = ", tn); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_%sArray_slice(", k); emit_expr(c, recv, b);
        buf_printf(b, ", -_t%d, _t%d); })", tn, tn);
        return;
      }
      if (!strcmp(name, "pop") && argc == 0) {
        buf_printf(b, "sp_%sArray_pop(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "min") || !strcmp(name, "max")) && argc == 0 && rt != TY_STR_ARRAY) {
        buf_printf(b, "sp_%sArray_%s(", k, name); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "minmax") && argc == 0 && rt != TY_STR_ARRAY && block < 0) {
        int t = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray *_t%d = sp_%sArray_new(); sp_%sArray_push(_t%d, sp_%sArray_min(_t%d));"
                      " sp_%sArray_push(_t%d, sp_%sArray_max(_t%d)); _t%d; })",
                   k, o, k, k, o, k, t, k, o, k, t, o);
        return;
      }
      if ((!strcmp(name, "index") || !strcmp(name, "find_index") || !strcmp(name, "rindex")) && argc == 1 && (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY)) {
        /* nil-on-miss -> poly */
        const char *fn = !strcmp(name, "rindex") ? "rindex_poly" : "index_poly";
        buf_printf(b, "sp_%sArray_%s(", k, fn);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "include?") && argc == 1) {
        /* a typed array can never contain an element of an incompatible
           type (numeric vs string), so the answer is statically false;
           still evaluate both operands for any side effects. */
        int mismatch = 0;
        if (rt == TY_STR_ARRAY && a0 != TY_STRING && a0 != TY_UNKNOWN && a0 != TY_POLY) mismatch = 1;
        if ((rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY) &&
            a0 != TY_INT && a0 != TY_FLOAT && a0 != TY_UNKNOWN && a0 != TY_POLY) mismatch = 1;
        if (mismatch) {
          buf_puts(b, "((void)("); emit_expr(c, recv, b);
          buf_puts(b, "), (void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)");
          return;
        }
      }
      if ((!strcmp(name, "include?") || !strcmp(name, "index") || !strcmp(name, "find_index")) && argc == 1 && rt != TY_FLOAT_ARRAY) {
        const char *fn = !strcmp(name, "include?") ? "include" : "index";
        buf_printf(b, "sp_%sArray_%s(", k, fn);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "sort") && argc == 0 &&
          (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY || rt == TY_STR_ARRAY)) {
        buf_printf(b, "sp_%sArray_sort(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "uniq") && argc == 0 && rt == TY_INT_ARRAY) {
        buf_puts(b, "sp_IntArray_uniq("); emit_expr(c, recv, b); buf_puts(b, ")");
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
      if ((!strcmp(name, "&") || !strcmp(name, "|") || !strcmp(name, "-") || !strcmp(name, "difference")) && argc == 1 && a0 == rt) {
        const char *fn = !strcmp(name, "&") ? "intersect" : ((!strcmp(name, "|")) ? "union" : "difference");
        buf_printf(b, "sp_%sArray_%s(", k, fn);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "union") && argc == 0) {
        buf_printf(b, "sp_%sArray_union(", k); emit_expr(c, recv, b); buf_puts(b, ", NULL)");
        return;
      }
      if (!strcmp(name, "sample") && argc == 0) {
        buf_printf(b, "sp_%sArray_sample(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
    }
    /* poly (mixed-element) array methods: elements are boxed sp_RbVal */
    if (rt == TY_POLY_ARRAY) {
      if (!strcmp(name, "[]") && argc == 1) {
        buf_puts(b, "sp_PolyArray_get("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "clear") && argc == 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; if (_t%d) _t%d->len = 0; _t%d; })", t, t, t);
        return;
      }
      if (!strcmp(name, "+") && argc == 1 && a0 == TY_POLY_ARRAY) {
        buf_puts(b, "sp_PolyArray_concat("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "&") || !strcmp(name, "|") || !strcmp(name, "-")) && argc == 1 && a0 == TY_POLY_ARRAY) {
        const char *fn = !strcmp(name, "&") ? "intersect" : (!strcmp(name, "|") ? "union" : "difference");
        buf_printf(b, "sp_PolyArray_%s(", fn);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "union") && argc == 0) {
        buf_puts(b, "sp_PolyArray_union("); emit_expr(c, recv, b); buf_puts(b, ", NULL)");
        return;
      }
      if (!strcmp(name, "sample") && argc == 0) {
        buf_puts(b, "sp_PolyArray_sample("); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "all?") || !strcmp(name, "any?") ||
           !strcmp(name, "none?") || !strcmp(name, "one?")) &&
          argc == 0 && nt_ref(nt, id, "block") < 0) {
        /* count truthy elements; a poly element may be nil/false */
        int t = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; mrb_int _t%d = 0; for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)"
                      " if (sp_poly_truthy(sp_PolyArray_get(_t%d, _t%d))) _t%d++;",
                   tn, ti, ti, t, ti, t, ti, tn);
        const char *expr = !strcmp(name, "all?") ? "_t%d == sp_PolyArray_length(_t%d)"
                         : !strcmp(name, "any?") ? "_t%d > 0"
                         : !strcmp(name, "none?") ? "_t%d == 0" : "_t%d == 1";
        buf_puts(b, " (");
        if (!strcmp(name, "all?")) buf_printf(b, expr, tn, t);
        else buf_printf(b, expr, tn);
        buf_puts(b, "); })");
        return;
      }
      if ((!strcmp(name, "length") || !strcmp(name, "size") || !strcmp(name, "count")) && argc == 0
          && nt_ref(nt, id, "block") < 0) {
        buf_puts(b, "sp_PolyArray_length("); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "count") && argc == 0 && nt_ref(nt, id, "block") >= 0) {
        /* count { |x| cond } on PolyArray */
        int blk = nt_ref(nt, id, "block");
        const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
        int body2 = nt_ref(nt, blk, "body");
        int bn2 = 0; const int *bb2 = body2 >= 0 ? nt_arr(nt, body2, "body", &bn2) : NULL;
        if (bn2 > 0) {
          int trecv = ++g_tmp, tcnt = ++g_tmp, ti = ++g_tmp;
          Buf rb2; memset(&rb2, 0, sizeof rb2); emit_expr(c, recv, &rb2);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_PolyArray *_t%d = %s;\n", trecv, rb2.p ? rb2.p : ""); free(rb2.p);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = 0;\n", tcnt);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n",
                     ti, ti, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
          for (int j = 0; j < bn2 - 1; j++) emit_stmt(c, bb2[j], g_pre, g_indent + 1);
          int saveI = g_indent; g_indent = g_indent + 1;
          Buf vb2; memset(&vb2, 0, sizeof vb2); emit_expr(c, bb2[bn2 - 1], &vb2);
          g_indent = saveI;
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "if (%s) _t%d++;\n", vb2.p ? vb2.p : "0", tcnt);
          free(vb2.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tcnt);
          return;
        }
      }
      if (!strcmp(name, "empty?") && argc == 0) {
        buf_puts(b, "(sp_PolyArray_length("); emit_expr(c, recv, b); buf_puts(b, ") == 0)");
        return;
      }
      if ((!strcmp(name, "push") || !strcmp(name, "<<") || !strcmp(name, "append")) && argc == 1) {
        buf_puts(b, "sp_PolyArray_push("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "first") && argc == 0) {
        buf_puts(b, "sp_PolyArray_get("); emit_expr(c, recv, b); buf_puts(b, ", 0)");
        return;
      }
      if (!strcmp(name, "to_a") && argc == 0) { emit_expr(c, recv, b); return; }
      if (!strcmp(name, "last") && argc == 0) {
        int t = ++g_tmp;
        Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_PolyArray *_t%d = ", t);
        buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
        buf_printf(b, "sp_PolyArray_get(_t%d, sp_PolyArray_length(_t%d) - 1)", t, t);
        return;
      }
      if (!strcmp(name, "include?") && argc == 1) {
        buf_puts(b, "sp_PolyArray_include("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "dup") && argc == 0) {
        buf_puts(b, "sp_PolyArray_dup("); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "compact") && argc == 0) {
        buf_puts(b, "sp_PolyArray_compact("); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "compact!") && argc == 0) {
        buf_puts(b, "sp_PolyArray_compact_bang("); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "flatten") && argc <= 1) {
        if (argc == 1) { buf_puts(b, "sp_PolyArray_flatten_n("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else { buf_puts(b, "sp_PolyArray_flatten("); emit_expr(c, recv, b); buf_puts(b, ")"); }
        return;
      }
      if (!strcmp(name, "transpose") && argc == 0) {
        buf_puts(b, "sp_int_array_transpose("); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "assoc") || !strcmp(name, "rassoc")) && argc == 1) {
        buf_printf(b, "sp_PolyArray_%s(", name); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "join") && argc == 1) {
        buf_puts(b, "sp_PolyArray_join("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "inspect") || !strcmp(name, "to_s")) && argc == 0) {
        buf_puts(b, "sp_PolyArray_inspect("); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "slice!") && argc == 2) {
        buf_puts(b, "sp_PolyArray_slice_bang("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "replace") && argc == 1 && a0 == TY_POLY_ARRAY) {
        buf_puts(b, "sp_PolyArray_replace("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "shuffle") && argc == 0) {
        buf_puts(b, "sp_PolyArray_shuffle("); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "sort") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        buf_puts(b, "sp_PolyArray_sort("); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      {
        const char *base = NULL;
        if      (!strcmp(name, "reverse!")) base = "reverse_bang";
        else if (!strcmp(name, "shuffle!")) base = "shuffle_bang";
        else if (!strcmp(name, "sort!"))    base = "sort_bang";
        else if (!strcmp(name, "uniq!"))    base = "uniq_bang";
        if (base && argc == 0) {
          int t = ++g_tmp;
          buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
          buf_printf(b, "; sp_PolyArray_%s(_t%d); _t%d; })", base, t, t);
          return;
        }
      }
      if (!strcmp(name, "rotate!") && argc <= 1) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_PolyArray_rotate_bang(_t%d, ", t);
        if (argc == 1) emit_expr(c, argv[0], b); else buf_puts(b, "1");
        buf_printf(b, "); _t%d; })", t);
        return;
      }
      if ((!strcmp(name, "map!") || !strcmp(name, "collect!")) && nt_ref(nt, id, "block") >= 0) {
        int blk = nt_ref(nt, id, "block");
        const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          int trecv = ++g_tmp, ti = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n", ti, ti, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "sp_PolyArray_set(_t%d, _t%d, %s);\n", trecv, ti, vb.p ? vb.p : "sp_box_nil()");
          free(vb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", trecv); return;
        }
      }
      if ((!strcmp(name, "select!") || !strcmp(name, "filter!") || !strcmp(name, "keep_if") ||
           !strcmp(name, "reject!") || !strcmp(name, "delete_if")) && nt_ref(nt, id, "block") >= 0) {
        int is_rej = !strcmp(name, "reject!") || !strcmp(name, "delete_if");
        int blk = nt_ref(nt, id, "block");
        const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          int trecv = ++g_tmp, ti = ++g_tmp, twp = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = 0;\n", twp);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n", ti, ti, trecv, ti);
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal _telt%d = sp_PolyArray_get(_t%d, _t%d);\n", ti, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = _telt%d;\n", bp, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          emit_indent(g_pre, g_indent);
          buf_puts(g_pre, "if (");
          if (is_rej) buf_puts(g_pre, "!");
          emit_cond(c, bb[bn - 1], g_pre);
          g_indent = sv;
          buf_printf(g_pre, ") { sp_PolyArray_set(_t%d, _t%d, _telt%d); _t%d++; }\n",
                     trecv, twp, ti, twp);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "if (_t%d) _t%d->len = _t%d;\n", trecv, trecv, twp);
          buf_printf(b, "_t%d", trecv); return;
        }
      }
      if (!strcmp(name, "to_h") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        TyKind res = comp_ntype(c, id);
        const char *hn = ty_hash_cname(res);
        if (!hn) hn = "SymPoly";
        TyKind kty = ty_hash_key(res), vty = ty_hash_val(res);
        int tr = ++g_tmp, th = ++g_tmp, ti = ++g_tmp, tp = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", tr); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);", hn, th, hn, th);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ti, ti, tr, ti);
        buf_printf(b, " sp_PolyArray *_t%d = (sp_PolyArray *)sp_PolyArray_get(_t%d, _t%d).v.p;", tp, tr, ti);
        /* key extraction */
        buf_printf(b, " sp_%sHash_set(_t%d, ", hn, th);
        char kexpr[128];
        if (kty == TY_SYMBOL)      snprintf(kexpr, sizeof kexpr, "(sp_sym)sp_PolyArray_get(_t%d, 0).v.i", tp);
        else if (kty == TY_STRING) snprintf(kexpr, sizeof kexpr, "sp_PolyArray_get(_t%d, 0).v.s", tp);
        else                       snprintf(kexpr, sizeof kexpr, "sp_PolyArray_get(_t%d, 0).v.i", tp);
        buf_puts(b, kexpr); buf_puts(b, ", ");
        /* value extraction */
        if (vty == TY_POLY)        buf_printf(b, "sp_PolyArray_get(_t%d, 1)", tp);
        else if (vty == TY_INT)    buf_printf(b, "sp_PolyArray_get(_t%d, 1).v.i", tp);
        else if (vty == TY_STRING) buf_printf(b, "sp_PolyArray_get(_t%d, 1).v.s", tp);
        else if (vty == TY_FLOAT)  buf_printf(b, "sp_PolyArray_get(_t%d, 1).v.f", tp);
        else                       buf_printf(b, "sp_PolyArray_get(_t%d, 1)", tp);
        buf_printf(b, "); } _t%d; })", th);
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
    if (!strcmp(name, "to_sym") || !strcmp(name, "itself")) { emit_expr(c, recv, b); return; }
    /* case-folding methods return a (re-interned) symbol */
    if (!strcmp(name, "upcase") || !strcmp(name, "downcase") ||
        !strcmp(name, "capitalize") || !strcmp(name, "swapcase")) {
      buf_printf(b, "sp_sym_intern(sp_str_%s(sp_sym_to_s(", name); emit_expr(c, recv, b); buf_puts(b, ")))");
      return;
    }
    if (!strcmp(name, "length") || !strcmp(name, "size")) {
      buf_puts(b, "((mrb_int)strlen(sp_sym_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))");
      return;
    }
    if (!strcmp(name, "empty?")) {
      buf_puts(b, "(strlen(sp_sym_to_s("); emit_expr(c, recv, b); buf_puts(b, ")) == 0)");
      return;
    }
    if (!strcmp(name, "==") || !strcmp(name, "!=")) {
      buf_puts(b, name[0] == '=' ? "(" : "(!(");
      emit_expr(c, recv, b); buf_puts(b, " == "); emit_expr(c, argv[0], b);
      buf_puts(b, name[0] == '=' ? ")" : "))");
      return;
    }
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

  /* str.each_char / each_line / chars / lines / bytes / codepoints { |x| ... } -> iterate, return self. */
  if (recv >= 0 && rt == TY_STRING && nt_ref(nt, id, "block") >= 0 &&
      (!strcmp(name, "each_char") || !strcmp(name, "each_line") || !strcmp(name, "each_byte") ||
       !strcmp(name, "chars") || !strcmp(name, "lines") || !strcmp(name, "bytes") || !strcmp(name, "codepoints"))) {
    int block = nt_ref(nt, id, "block");
    int body = nt_ref(nt, block, "body");
    const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
    int ts = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    int is_line = !strcmp(name, "each_line") || !strcmp(name, "lines");
    int is_byte = !strcmp(name, "each_byte") || !strcmp(name, "bytes") || !strcmp(name, "codepoints");
    Scope *cs_ech = p0 ? comp_scope_of(c, id) : NULL;
    LocalVar *clv_ech = (p0 && cs_ech) ? scope_local(cs_ech, p0) : NULL;
    int p0_box_poly_ech = clv_ech && clv_ech->type == TY_POLY;
    buf_printf(b, "({ const char *_t%d = %s; ", ts, rb.p ? rb.p : ""); free(rb.p);
    /* Save outer variable before loop to restore it afterward */
    int tsv_ech = 0;
    if (p0 && clv_ech) {
      tsv_ech = ++g_tmp;
      Buf sv_ech; memset(&sv_ech, 0, sizeof sv_ech); emit_ctype(c, clv_ech->type, &sv_ech);
      buf_printf(b, "%s _t%d = lv_%s; ", sv_ech.p ? sv_ech.p : "sp_RbVal", tsv_ech, p0); free(sv_ech.p);
    }
    if (is_line) {
      int tl = ++g_tmp;
      buf_printf(b, "sp_StrArray *_t%d = sp_str_lines(_t%d); for (mrb_int _t%d = 0; _t%d < sp_StrArray_length(_t%d); _t%d++) { ", tl, ts, ti, ti, tl, ti);
      if (p0) {
        if (p0_box_poly_ech) buf_printf(b, "lv_%s = sp_box_str(sp_StrArray_get(_t%d, _t%d)); ", p0, tl, ti);
        else buf_printf(b, "lv_%s = sp_StrArray_get(_t%d, _t%d); ", p0, tl, ti);
      }
    }
    else if (is_byte) {
      buf_printf(b, "for (mrb_int _t%d = 0; _t%d < (mrb_int)sp_str_byte_len(_t%d); _t%d++) { ", ti, ti, ts, ti);
      if (p0) {
        if (p0_box_poly_ech) buf_printf(b, "lv_%s = sp_box_int((unsigned char)_t%d[_t%d]); ", p0, ts, ti);
        else buf_printf(b, "lv_%s = (unsigned char)_t%d[_t%d]; ", p0, ts, ti);
      }
    }
    else {
      buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_str_length(_t%d); _t%d++) { ", ti, ti, ts, ti);
      if (p0) {
        if (p0_box_poly_ech) buf_printf(b, "lv_%s = sp_box_str(sp_str_char_at_or_nil(_t%d, _t%d)); ", p0, ts, ti);
        else buf_printf(b, "lv_%s = sp_str_char_at_or_nil(_t%d, _t%d); ", p0, ts, ti);
      }
    }
    int sv = g_nren; g_nren = 0;
    emit_stmts(c, body, b, 0);
    g_nren = sv;
    if (p0 && tsv_ech > 0) buf_printf(b, " lv_%s = _t%d;", p0, tsv_ech);
    buf_printf(b, " } _t%d; })", ts);
    return;
  }

  /* scalar receiver methods: evaluate the receiver once into rs, then
     splice its text (so a literal/complex receiver isn't rebuilt). */
  if (recv >= 0 && (rt == TY_STRING || rt == TY_INT || rt == TY_FLOAT)) {
    Buf rs; memset(&rs, 0, sizeof rs);
    emit_expr(c, recv, &rs);
    const char *r = rs.p ? rs.p : "";
    int handled = 1;

    if (rt == TY_STRING) {
      /* blockless "a".upto("c") materializes the succ-sequence as an array */
      if (!strcmp(name, "upto") && argc == 1 && nt_ref(nt, id, "block") < 0) {
        buf_printf(b, "sp_StrArray_from_string_range(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", 0)");
      }
      /* string methods taking a regex-literal argument route to the engine */
      else if ((!strcmp(name, "gsub") || !strcmp(name, "sub")) && argc == 2 && re_lit_index(c, argv[0]) >= 0) {
        const char *suf = comp_ntype(c, argv[1]) == TY_STR_STR_HASH ? "_str_str_hash" : "";
        buf_printf(b, "sp_re_%s%s(sp_re_pat_%d, %s, ", name, suf, re_lit_index(c, argv[0]), r);
        emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if ((!strcmp(name, "gsub") || !strcmp(name, "sub")) && argc == 2 &&
               nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "InterpolatedRegularExpressionNode")) {
        Buf rp; memset(&rp, 0, sizeof rp);
        emit_regex_pat_to_buf(c, argv[0], &rp);
        buf_printf(b, "sp_re_%s(%s, %s, ", name, rp.p ? rp.p : "NULL", r);
        emit_expr(c, argv[1], b); buf_puts(b, ")");
        free(rp.p);
      }
      else if (!strcmp(name, "split") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_split(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (!strcmp(name, "scan") && argc == 1 && re_lit_index(c, argv[0]) >= 0 &&
               !re_has_captures(re_lit_src(c, argv[0]))) {
        buf_printf(b, "sp_re_scan(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (!strcmp(name, "to_sym") || !strcmp(name, "intern")) buf_printf(b, "sp_sym_intern(%s)", r);
      else if (!strcmp(name, "length") || !strcmp(name, "size")) buf_printf(b, "sp_str_length(%s)", r);
      else if (!strcmp(name, "bytesize")) buf_printf(b, "(mrb_int)sp_str_byte_len(%s)", r);
      else if (!strcmp(name, "upcase"))     buf_printf(b, "sp_str_upcase(%s)", r);
      else if (!strcmp(name, "downcase"))   buf_printf(b, "sp_str_downcase(%s)", r);
      else if (!strcmp(name, "capitalize")) buf_printf(b, "sp_str_capitalize(%s)", r);
      else if (!strcmp(name, "reverse"))    buf_printf(b, "sp_str_reverse(%s)", r);
      else if (!strcmp(name, "strip"))      buf_printf(b, "sp_str_strip(%s)", r);
      else if (!strcmp(name, "lstrip"))     buf_printf(b, "sp_str_lstrip(%s)", r);
      else if (!strcmp(name, "rstrip"))     buf_printf(b, "sp_str_rstrip(%s)", r);
      else if (!strcmp(name, "chomp") && argc == 1) {
        buf_printf(b, "sp_str_chomp_sep(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "chomp"))      buf_printf(b, "sp_str_chomp(%s)", r);
      else if (!strcmp(name, "chop"))       buf_printf(b, "sp_str_chop(%s)", r);
      else if (!strcmp(name, "to_s") || !strcmp(name, "to_str") || !strcmp(name, "dup") || !strcmp(name, "clone")) buf_puts(b, r);
      else if (!strcmp(name, "inspect"))    { int tv = ++g_tmp; buf_printf(b, "({ const char *_t%d = %s; _t%d ? sp_str_inspect(_t%d) : SPL(\"nil\"); })", tv, r, tv, tv); }
      else if (!strcmp(name, "empty?"))     buf_printf(b, "(sp_str_length(%s) == 0)", r);
      else if (!strcmp(name, "include?") && argc == 1) {
        buf_printf(b, "sp_str_include(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "start_with?") && argc == 1) {
        buf_printf(b, "sp_str_start_with(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "end_with?") && argc == 1) {
        buf_printf(b, "sp_str_end_with(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "index") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_index_poly(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (!strcmp(name, "index") && argc == 1) {
        /* nil-on-miss carried as the SP_INT_NIL sentinel (a nullable int) */
        buf_printf(b, "sp_str_index_opt(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if ((!strcmp(name, "partition") || !strcmp(name, "rpartition")) && argc == 1 &&
               re_lit_index(c, argv[0]) < 0) {
        buf_printf(b, "sp_str_%s(%s, ", name, r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "partition") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        /* [before, match, after] from the first regex match, else [s, "", ""] */
        int tr = ++g_tmp;
        buf_printf(b, "({ sp_StrArray *_t%d = sp_StrArray_new();"
                      " if (sp_re_match(sp_re_pat_%d, %s) >= 0) {"
                      " sp_StrArray_push(_t%d, sp_re_match_pre); sp_StrArray_push(_t%d, sp_re_match_str);"
                      " sp_StrArray_push(_t%d, sp_re_match_post); } else {"
                      " sp_StrArray_push(_t%d, %s); sp_StrArray_push(_t%d, SPL(\"\")); sp_StrArray_push(_t%d, SPL(\"\")); }"
                      " _t%d; })",
                   tr, re_lit_index(c, argv[0]), r, tr, tr, tr, tr, r, tr, tr, tr);
      }
      else if (!strcmp(name, "rpartition") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_rpartition(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (!strcmp(name, "rindex") && argc == 1) { buf_printf(b, "sp_str_rindex(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "rindex") && argc == 2) { buf_printf(b, "sp_str_rindex_from(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "crypt") && argc == 1) { buf_printf(b, "sp_str_crypt(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "scrub") && argc == 0) buf_printf(b, "sp_str_scrub(%s, 0)", r);
      else if (!strcmp(name, "scrub") && argc == 1) { buf_printf(b, "sp_str_scrub(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if ((!strcmp(name, "[]") || !strcmp(name, "slice")) && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        /* s[/re/] -> the matched substring, or nil (NULL) on no match */
        buf_printf(b, "(sp_re_match(sp_re_pat_%d, %s) >= 0 ? sp_re_match_str : NULL)", re_lit_index(c, argv[0]), r);
      }
      else if ((!strcmp(name, "[]") || !strcmp(name, "slice")) && argc == 1 && nt_type(c->nt, argv[0]) &&
               !strcmp(nt_type(c->nt, argv[0]), "RangeNode")) {
        /* s[a..b] / s[a...b]; beginless/endless ranges use 0 / length */
        int rn = argv[0];
        int excl = (int)(nt_int(c->nt, rn, "flags", 0) & 4) ? 1 : 0;
        int lo = nt_ref(c->nt, rn, "left"), hi = nt_ref(c->nt, rn, "right");
        buf_printf(b, "sp_str_sub_range_r(%s, ", r);
        if (lo >= 0) emit_expr(c, lo, b); else buf_puts(b, "0");
        buf_puts(b, ", ");
        if (hi >= 0) { emit_expr(c, hi, b); buf_printf(b, ", %d)", excl); }
        else buf_printf(b, "(mrb_int)sp_str_length(%s), 0)", r);  /* endless: to the end */
      }
      else if ((!strcmp(name, "[]") || !strcmp(name, "slice")) && argc == 2) {
        /* s[start, len] */
        buf_printf(b, "sp_str_sub_range(%s, ", r);
        emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if ((!strcmp(name, "[]") || !strcmp(name, "slice")) && argc == 1) {
        buf_printf(b, "sp_str_char_at_or_nil(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "split") && argc == 0) buf_printf(b, "sp_str_split_ws(%s)", r);
      else if (!strcmp(name, "split") && argc == 1) {
        /* split(nil) and split(" ") are whitespace-mode; split(sep) drops trailing empties */
        const char *aty = nt_type(c->nt, argv[0]);
        int nil_arg = aty && !strcmp(aty, "NilNode");
        int ws = nil_arg || (aty && !strcmp(aty, "StringNode") && nt_str(c->nt, argv[0], "content") &&
                 !strcmp(nt_str(c->nt, argv[0], "content"), " "));
        if (ws) buf_printf(b, "sp_str_split_ws(%s)", r);
        else { buf_printf(b, "sp_str_split_drop_trailing(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      }
      else if (!strcmp(name, "split") && argc == 2) {
        buf_printf(b, "sp_str_split_limit(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "clamp") && (argc == 2 ||
               (argc == 1 && nt_type(c->nt, argv[0]) && !strcmp(nt_type(c->nt, argv[0]), "RangeNode")))) {
        int lo_n, hi_n;
        if (argc == 2) { lo_n = argv[0]; hi_n = argv[1]; }
        else { int rn = argv[0]; lo_n = nt_ref(c->nt, rn, "left"); hi_n = nt_ref(c->nt, rn, "right"); }
        int tc = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = %s; const char *_t%d = ", tc, r, tlo); emit_expr(c, lo_n, b);
        buf_printf(b, "; const char *_t%d = ", thi); emit_expr(c, hi_n, b);
        buf_printf(b, "; strcmp(_t%d, _t%d) < 0 ? _t%d : (strcmp(_t%d, _t%d) > 0 ? _t%d : _t%d); })",
                   tc, tlo, tlo, tc, thi, thi, tc);
      }
      else if (!strcmp(name, "oct") && argc == 0) buf_printf(b, "sp_str_oct(%s)", r);
      else if (!strcmp(name, "ord") && argc == 0) buf_printf(b, "sp_str_ord(%s)", r);
      else if ((!strcmp(name, "force_encoding") || !strcmp(name, "b") || !strcmp(name, "encode")) && argc <= 1) buf_printf(b, "(%s)", r);
      else if (!strcmp(name, "encoding") && argc == 0) buf_printf(b, "((void)(%s), sp_box_encoding(sp_encoding_utf8()))", r);
      else if (!strcmp(name, "dump") && argc == 0) buf_printf(b, "sp_str_dump(%s)", r);
      else if (!strcmp(name, "undump") && argc == 0) buf_printf(b, "sp_str_undump(%s)", r);
      else if (!strcmp(name, "casecmp") && argc == 1) { buf_printf(b, "sp_str_casecmp(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "casecmp?") && argc == 1) { buf_printf(b, "(sp_str_casecmp(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ") == 0)"); }
      else if (!strcmp(name, "byteslice") && argc == 2) { buf_printf(b, "sp_str_byteslice(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "byteslice") && argc == 1) { buf_printf(b, "sp_str_byteslice(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", 1)"); }
      else if (!strcmp(name, "squeeze") && argc == 0) buf_printf(b, "sp_str_squeeze(%s)", r);
      else if (!strcmp(name, "squeeze") && argc == 1) { buf_printf(b, "sp_str_squeeze_chars(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if ((!strcmp(name, "tr") || !strcmp(name, "tr_s")) && argc == 2) {
        buf_printf(b, "sp_str_%s(%s, ", name, r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "delete") && argc == 1) { buf_printf(b, "sp_str_delete(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "count") && argc == 1) { buf_printf(b, "sp_str_count(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "count") && argc >= 2) {
        buf_printf(b, "sp_str_count_n(%s, (const char *[]){", r);
        for (int a = 0; a < argc; a++) { if (a) buf_puts(b, ", "); emit_expr(c, argv[a], b); }
        buf_printf(b, "}, %d)", argc);
      }
      else if (!strcmp(name, "lines") && argc == 0) buf_printf(b, "sp_str_lines(%s)", r);
      else if (!strcmp(name, "bytes") && argc == 0)   buf_printf(b, "sp_str_bytes(%s)", r);
      else if (!strcmp(name, "unpack") && argc == 1)  { buf_printf(b, "sp_str_unpack(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "chars") && argc == 0)   buf_printf(b, "sp_str_chars(%s)", r);
      else if ((!strcmp(name, "succ") || !strcmp(name, "next")) && argc == 0) buf_printf(b, "sp_str_succ(%s)", r);
      else if (!strcmp(name, "to_i") && argc == 0)    buf_printf(b, "sp_str_to_i_cruby(%s)", r);
      else if (!strcmp(name, "to_i") && argc == 1)    { buf_printf(b, "sp_str_to_i_base(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "to_f") && argc == 0)    buf_printf(b, "atof(%s)", r);
      else if (!strcmp(name, "gsub") && argc == 2) {
        buf_printf(b, "sp_str_gsub(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "sub") && argc == 2 && comp_ntype(c, argv[1]) == TY_STR_STR_HASH) {
        buf_printf(b, "sp_str_sub_str_str_hash(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "sub") && argc == 2) {
        buf_printf(b, "sp_str_sub(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "tr") && argc == 2) {
        buf_printf(b, "sp_str_tr(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "center") && argc == 1) {
        buf_printf(b, "sp_str_center(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "center") && argc == 2) {
        buf_printf(b, "sp_str_center2(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "ljust") && argc == 1) {
        buf_printf(b, "sp_str_ljust(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "ljust") && argc == 2) {
        buf_printf(b, "sp_str_ljust2(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "rjust") && argc == 1) {
        buf_printf(b, "sp_str_rjust(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "rjust") && argc == 2) {
        buf_printf(b, "sp_str_rjust2(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else handled = 0;
    }
    else if (rt == TY_INT) {
      if      (!strcmp(name, "to_s") && argc == 0) buf_printf(b, "((%s) == SP_INT_NIL ? SPL(\"\") : sp_int_to_s(%s))", r, r);
      else if (!strcmp(name, "inspect")) buf_printf(b, "((%s) == SP_INT_NIL ? SPL(\"nil\") : sp_int_to_s(%s))", r, r);
      else if (!strcmp(name, "to_f"))   buf_printf(b, "((mrb_float)(%s))", r);
      else if ((!strcmp(name, "to_i") || !strcmp(name, "to_int") || !strcmp(name, "floor") ||
                !strcmp(name, "ceil") || !strcmp(name, "round") || !strcmp(name, "truncate")) &&
               argc == 0) buf_printf(b, "(%s)", r);
      else if ((!strcmp(name, "floor") || !strcmp(name, "ceil") ||
                !strcmp(name, "round") || !strcmp(name, "truncate")) && argc == 1) {
        buf_printf(b, "sp_int_%s(%s, ", name, r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "abs"))    buf_printf(b, "((%s) < 0 ? -(%s) : (%s))", r, r, r);
      else if (!strcmp(name, "chr"))    buf_printf(b, "sp_int_chr(%s)", r);
      else if (!strcmp(name, "[]") && argc == 1) { buf_printf(b, "(((%s) >> (", r); emit_expr(c, argv[0], b); buf_puts(b, ")) & 1)"); }
      else if (!strcmp(name, "even?"))  buf_printf(b, "((%s) %% 2 == 0)", r);
      else if (!strcmp(name, "odd?"))   buf_printf(b, "((%s) %% 2 != 0)", r);
      else if (!strcmp(name, "zero?"))  buf_printf(b, "((%s) == 0)", r);
      else if (!strcmp(name, "nonzero?")) buf_printf(b, "((%s) == 0 ? SP_INT_NIL : (%s))", r, r);
      else if (!strcmp(name, "positive?")) buf_printf(b, "((%s) > 0)", r);
      else if (!strcmp(name, "negative?")) buf_printf(b, "((%s) < 0)", r);
      else if (!strcmp(name, "divmod") && argc == 1) {
        int tb = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = ", tb); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_IntArray *_t%d = sp_IntArray_new(); sp_IntArray_push(_t%d, sp_idiv(%s, _t%d));"
                      " sp_IntArray_push(_t%d, sp_imod(%s, _t%d)); _t%d; })", o, o, r, tb, o, r, tb, o);
      }
      else if (!strcmp(name, "div") && argc == 1) { buf_printf(b, "sp_idiv(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "gcd") && argc == 1) { buf_printf(b, "sp_gcd(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "lcm") && argc == 1) { buf_printf(b, "sp_lcm(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "magnitude") && argc == 0) buf_printf(b, "((%s) < 0 ? -(%s) : (%s))", r, r, r);
      else if (!strcmp(name, "modulo") && argc == 1) { buf_printf(b, "sp_imod(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "remainder") && argc == 1) { buf_printf(b, "((%s) %% (", r); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
      else if (!strcmp(name, "size") && argc == 0) buf_puts(b, "((mrb_int)sizeof(mrb_int))");
      else if (!strcmp(name, "gcdlcm") && argc == 1) {
        int ta = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = ", ta); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_IntArray *_t%d = sp_IntArray_new(); sp_IntArray_push(_t%d, sp_gcd(%s, _t%d));"
                      " sp_IntArray_push(_t%d, sp_lcm(%s, _t%d)); _t%d; })", o, o, r, ta, o, r, ta, o);
      }
      else if (!strcmp(name, "clamp") && argc == 2) { buf_printf(b, "sp_int_clamp(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "clamp") && argc == 1 && nt_type(c->nt, argv[0]) && !strcmp(nt_type(c->nt, argv[0]), "RangeNode")) {
        int rn = argv[0]; int tcr = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", tcr); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_int_clamp(%s, _t%d.first, _t%d.last - _t%d.excl); })", r, tcr, tcr, tcr);
        (void)rn;
      }
      else if (!strcmp(name, "digits") && argc == 0) buf_printf(b, "sp_int_digits(%s, 10)", r);
      else if (!strcmp(name, "allbits?") && argc == 1) { buf_printf(b, "(((%s) & (", r); emit_expr(c, argv[0], b); buf_printf(b, ")) == ("); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
      else if (!strcmp(name, "anybits?") && argc == 1) { buf_printf(b, "(((%s) & (", r); emit_expr(c, argv[0], b); buf_puts(b, ")) != 0)"); }
      else if (!strcmp(name, "nobits?") && argc == 1) { buf_printf(b, "(((%s) & (", r); emit_expr(c, argv[0], b); buf_puts(b, ")) == 0)"); }
      else if (!strcmp(name, "ceildiv") && argc == 1) { buf_printf(b, "sp_ceildiv(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "pow") && argc == 2) { buf_printf(b, "sp_powmod(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "pow") && argc == 1) { buf_printf(b, "sp_int_pow(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "pred") && argc == 0) buf_printf(b, "((%s) - 1)", r);
      else if ((!strcmp(name, "succ") || !strcmp(name, "next")) && argc == 0) buf_printf(b, "((%s) + 1)", r);
      else if (!strcmp(name, "to_s") && argc == 1) { buf_printf(b, "sp_int_to_s_base(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else handled = 0;
    }
    else { /* TY_FLOAT */
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
        else if (ndig < 0)  /* round to a power of ten left of the decimal -> Integer */
          buf_printf(b, "({ double _f = pow(10, %d); (mrb_int)(%s((%s) / _f) * _f); })", -ndig, cfn, r);
        else
          buf_printf(b, "((mrb_int)%s(%s))", cfn, r);
      }
      else if (!strcmp(name, "to_i"))  buf_printf(b, "((mrb_int)(%s))", r);
      else if (!strcmp(name, "to_f"))  buf_printf(b, "(%s)", r);
      else if (!strcmp(name, "divmod") && argc == 1) {
        /* Float#divmod(n) -> [floor(x/n) (Integer), x - q*n (Float)] */
        int tx = ++g_tmp, tn = ++g_tmp, tq = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ mrb_float _t%d = (%s); mrb_float _t%d = ", tx, r, tn); emit_expr(c, argv[0], b);
        buf_printf(b, "; if (isnan(_t%d) || isnan(_t%d)) sp_raise_cls(\"FloatDomainError\", \"NaN\");"
                      " if (_t%d == 0.0) sp_raise_cls(\"ZeroDivisionError\", \"divided by 0\");"
                      " mrb_int _t%d = (mrb_int)floor(_t%d / _t%d); sp_PolyArray *_t%d = sp_PolyArray_new();"
                      " sp_PolyArray_push(_t%d, sp_box_int(_t%d));"
                      " sp_PolyArray_push(_t%d, sp_box_float(_t%d - (mrb_float)_t%d * _t%d)); _t%d; })",
                   tx, tn, tn, tq, tx, tn, o, o, tq, o, tx, tq, tn, o);
      }
      else if (!strcmp(name, "to_s"))    buf_printf(b, "sp_float_opt_to_s(%s)", r);
      else if (!strcmp(name, "inspect")) buf_printf(b, "sp_float_opt_inspect(%s)", r);
      else if (!strcmp(name, "abs"))   buf_printf(b, "((%s) < 0 ? -(%s) : (%s))", r, r, r);
      else if (!strcmp(name, "zero?")) buf_printf(b, "((%s) == 0.0)", r);
      else if (!strcmp(name, "nan?"))  buf_printf(b, "(isnan(%s) != 0)", r);
      else if (!strcmp(name, "finite?")) buf_printf(b, "(isfinite(%s) != 0)", r);
      else if (!strcmp(name, "infinite?")) buf_printf(b, "(isinf(%s) ? ((%s) > 0 ? 1LL : -1LL) : SP_INT_NIL)", r, r);
      else if (!strcmp(name, "positive?")) buf_printf(b, "((%s) > 0)", r);
      else if (!strcmp(name, "negative?")) buf_printf(b, "((%s) < 0)", r);
      else handled = 0;
    }
    free(rs.p);
    if (handled) return;
  }

  /* `[]=` in expression position: mutate and return the assigned value.
     Ruby's `(h[k] = v)` and `(a[i] = v)` evaluate to v. */
  if (!strcmp(name, "[]=") && argc == 2 && recv >= 0) {
    TyKind vt = comp_ntype(c, argv[1]);
    if (ty_is_hash(rt)) {
      const char *hn = ty_hash_cname(rt);
      if (hn) {
        int tv = ++g_tmp;
        int is_poly_hash = (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH);
        buf_puts(b, "({ ");
        /* For poly hashes with scalar values, store the scalar and box it for the hash call. */
        TyKind decl_type = (is_poly_hash && vt != TY_UNKNOWN && vt != TY_POLY) ? vt : (vt != TY_UNKNOWN ? vt : TY_POLY);
        emit_ctype(c, decl_type, b);
        buf_printf(b, " _t%d = ", tv);
        emit_expr(c, argv[1], b);
        buf_printf(b, "; if (sp_gc_is_frozen("); emit_expr(c, recv, b); buf_puts(b, ")) sp_raise_frozen_hash(); ");
        buf_printf(b, "sp_%sHash_set(", hn); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (rt == TY_POLY_POLY_HASH) emit_boxed(c, argv[0], b); else emit_expr(c, argv[0], b);
        buf_puts(b, ", ");
        if (is_poly_hash && vt != TY_POLY) {
          char tvn[32]; snprintf(tvn, sizeof tvn, "_t%d", tv);
          emit_boxed_text(c, decl_type, tvn, b);
        } else {
          buf_printf(b, "_t%d", tv);
        }
        buf_printf(b, "); _t%d; })", tv);
        return;
      }
    }
    if (ty_is_array(rt) || rt == TY_POLY_ARRAY) {
      const char *k = rt == TY_POLY_ARRAY ? "Poly" : array_kind(rt);
      if (k) {
        int tv = ++g_tmp;
        buf_puts(b, "({ ");
        emit_ctype(c, vt != TY_UNKNOWN ? vt : TY_POLY, b);
        buf_printf(b, " _t%d = ", tv);
        if (rt == TY_POLY_ARRAY && vt != TY_POLY) emit_boxed(c, argv[1], b);
        else emit_expr(c, argv[1], b);
        buf_printf(b, "; sp_%sArray_set(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_expr(c, argv[0], b); buf_printf(b, ", _t%d); _t%d; })", tv, tv);
        return;
      }
    }
  }

  /* Last-resort fallbacks for inspect/to_s on unresolved receivers.
     The test array_unresolved_inspect_no_segv expects "[]" when an
     unsupported method chains into inspect. Emit a safe nil-degrade
     rather than aborting the compiler. */
  if (recv >= 0 && argc == 0 && !strcmp(name, "inspect")) {
    buf_puts(b, "\"[]\""); return;
  }
  if (recv >= 0 && argc == 0 && !strcmp(name, "to_s")) {
    buf_puts(b, "\"\""); return;
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

  /* string append: s << x  ->  s = sp_str_concat(s, x) (value semantics).
     recv must be an assignable lvalue (local or ivar). A chained append
     `s << a << b << c` bottoms out at the same lvalue, so unroll it into
     one reassignment per argument in left-to-right order. */
  if (rt == TY_STRING && !strcmp(name, "<<") && argc == 1) {
    /* walk down the receiver chain, collecting each `<<` argument */
    int chain[64]; int nchain = 0;
    int cur = id;
    while (nchain < 64) {
      /* unwrap ParenthesesNode wrappers (e.g. `(s << a) << b`) */
      while (nt_type(nt, cur) && !strcmp(nt_type(nt, cur), "ParenthesesNode")) {
        int pb = nt_ref(nt, cur, "body");
        if (pb < 0) break;
        int bn = 0; const int *bb = nt_arr(nt, pb, "body", &bn);
        if (bn != 1) break;
        cur = bb[0];
      }
      const char *cty = nt_type(nt, cur);
      if (!cty || strcmp(cty, "CallNode")) break;
      const char *cnm = nt_str(nt, cur, "name");
      int crecv = nt_ref(nt, cur, "receiver");
      if (!cnm || strcmp(cnm, "<<") || crecv < 0 || comp_ntype(c, crecv) != TY_STRING) break;
      int cargs = nt_ref(nt, cur, "arguments");
      int cac = 0; const int *cav = cargs >= 0 ? nt_arr(nt, cargs, "arguments", &cac) : NULL;
      if (cac != 1) break;
      chain[nchain++] = cav[0];
      cur = crecv;
    }
    const char *rty = nt_type(nt, cur);
    if (nchain > 0 && rty &&
        (!strcmp(rty, "LocalVariableReadNode") || !strcmp(rty, "InstanceVariableReadNode") || !strcmp(rty, "SelfNode"))) {
      /* chain was collected outermost-first; emit left-to-right */
      for (int j = nchain - 1; j >= 0; j--) {
        int arg = chain[j];
        TyKind at = comp_ntype(c, arg);
        emit_indent(b, indent);
        emit_expr(c, cur, b); buf_puts(b, " = sp_str_concat(");
        emit_expr(c, cur, b); buf_puts(b, ", ");
        if (at == TY_INT) { buf_puts(b, "sp_int_codepoint_to_str("); emit_expr(c, arg, b); buf_puts(b, ")"); }
        else if (at == TY_POLY) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, arg, b); buf_puts(b, ")"); }
        else emit_expr(c, arg, b);
        buf_puts(b, ");\n");
      }
      return 1;
    }
    /* `<<` onto a frozen string literal raises FrozenError */
    if (rty && !strcmp(rty, "StringNode")) {
      emit_indent(b, indent);
      buf_puts(b, "sp_raise_cls(\"FrozenError\", \"can't modify frozen String\");\n");
      return 1;
    }
    return 0;
  }

  /* in-place string bang methods on an assignable receiver: reassign the
     receiver to the transformed value (value-semantics mutation, like <<). */
  if (rt == TY_STRING && argc == 0) {
    const char *base = NULL;
    if      (!strcmp(name, "chomp!"))      base = "chomp";
    else if (!strcmp(name, "chop!"))       base = "chop";
    else if (!strcmp(name, "upcase!"))     base = "upcase";
    else if (!strcmp(name, "downcase!"))   base = "downcase";
    else if (!strcmp(name, "capitalize!")) base = "capitalize";
    else if (!strcmp(name, "swapcase!"))   base = "swapcase";
    else if (!strcmp(name, "strip!"))      base = "strip";
    else if (!strcmp(name, "lstrip!"))     base = "lstrip";
    else if (!strcmp(name, "rstrip!"))     base = "rstrip";
    else if (!strcmp(name, "reverse!"))    base = "reverse";
    else if (!strcmp(name, "squeeze!"))    base = "squeeze";
    if (base) {
      const char *rty = nt_type(nt, recv);
      if (rty && (!strcmp(rty, "LocalVariableReadNode") || !strcmp(rty, "InstanceVariableReadNode") || !strcmp(rty, "SelfNode"))) {
        emit_indent(b, indent);
        emit_expr(c, recv, b); buf_printf(b, " = sp_str_%s(", base); emit_expr(c, recv, b); buf_puts(b, ");\n");
        return 1;
      }
    }
  }
  /* replace / prepend / clear / delete_prefix!/suffix! via reassignment */
  if (rt == TY_STRING) {
    const char *rty = nt_type(nt, recv);
    int assignable = rty && (!strcmp(rty, "LocalVariableReadNode") || !strcmp(rty, "InstanceVariableReadNode") || !strcmp(rty, "SelfNode"));
    /* an in-place mutator on a frozen string literal raises FrozenError */
    if (rty && !strcmp(rty, "StringNode") &&
        (!strcmp(name, "insert") || !strcmp(name, "prepend") || !strcmp(name, "<<") ||
         !strcmp(name, "concat") || !strcmp(name, "replace") || !strcmp(name, "clear") ||
         !strcmp(name, "delete_prefix!") || !strcmp(name, "delete_suffix!"))) {
      emit_indent(b, indent);
      buf_puts(b, "sp_raise_cls(\"FrozenError\", \"can't modify frozen String\");\n");
      return 1;
    }
    if (assignable && !strcmp(name, "replace") && argc == 1) {
      emit_indent(b, indent); emit_expr(c, recv, b); buf_puts(b, " = "); emit_expr(c, argv[0], b); buf_puts(b, ";\n");
      return 1;
    }
    if (assignable && !strcmp(name, "prepend") && argc == 1) {
      emit_indent(b, indent); emit_expr(c, recv, b); buf_puts(b, " = sp_str_concat("); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, recv, b); buf_puts(b, ");\n");
      return 1;
    }
    if (assignable && !strcmp(name, "clear") && argc == 0) {
      emit_indent(b, indent); emit_expr(c, recv, b); buf_puts(b, " = (&(\"\\xff\")[1]);\n");
      return 1;
    }
    if (assignable && !strcmp(name, "insert") && argc == 2) {
      /* insert(i, x): s[0,i] + x + s[i..]. A negative i counts from the end
         and inserts after that character (i += len + 1). */
      int ti = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "{ mrb_int _t%d = ", ti); emit_expr(c, argv[0], b);
      buf_printf(b, "; if (_t%d < 0) _t%d += (mrb_int)sp_str_length(", ti, ti); emit_expr(c, recv, b); buf_printf(b, ") + 1; ");
      emit_expr(c, recv, b); buf_puts(b, " = sp_str_concat(sp_str_concat(sp_str_sub_range(");
      emit_expr(c, recv, b); buf_printf(b, ", 0, _t%d), ", ti); emit_expr(c, argv[1], b);
      buf_puts(b, "), sp_str_sub_range("); emit_expr(c, recv, b);
      buf_printf(b, ", _t%d, (mrb_int)sp_str_length(", ti); emit_expr(c, recv, b); buf_printf(b, "))); }\n");
      return 1;
    }
    if (assignable && (!strcmp(name, "delete_prefix!") || !strcmp(name, "delete_suffix!")) && argc == 1) {
      const char *base = !strcmp(name, "delete_prefix!") ? "delete_prefix" : "delete_suffix";
      emit_indent(b, indent); emit_expr(c, recv, b); buf_printf(b, " = sp_str_%s(", base); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ");\n");
      return 1;
    }
  }

  if (ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    if (hn && !strcmp(name, "[]=") && argc == 2) {
      emit_indent(b, indent);
      buf_puts(b, "if (sp_gc_is_frozen("); emit_expr(c, recv, b); buf_puts(b, ")) sp_raise_frozen_hash();\n");
      emit_indent(b, indent);
      buf_printf(b, "sp_%sHash_set(", hn);
      emit_expr(c, recv, b); buf_puts(b, ", ");
      if (rt == TY_POLY_POLY_HASH) emit_boxed(c, argv[0], b); else emit_expr(c, argv[0], b);
      buf_puts(b, ", ");
      if (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH) emit_boxed(c, argv[1], b);
      else emit_expr(c, argv[1], b);
      buf_puts(b, ");\n");
      return 1;
    }
    return 0;
  }

  if (rt == TY_POLY_ARRAY) {
    if (!strcmp(name, "[]=") && argc == 2) {
      emit_indent(b, indent);
      buf_puts(b, "sp_PolyArray_set("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ");\n");
      return 1;
    }
    if ((!strcmp(name, "push") || !strcmp(name, "<<") || !strcmp(name, "append")) && argc >= 1) {
      for (int a = 0; a < argc; a++) {
        emit_indent(b, indent);
        buf_puts(b, "sp_PolyArray_push("); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_boxed(c, argv[a], b); buf_puts(b, ");\n");
      }
      return 1;
    }
    if (!strcmp(name, "clear") && argc == 0) {
      emit_indent(b, indent);
      buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, ")->len = 0;\n");
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
  if ((!strcmp(name, "push") || !strcmp(name, "<<") || !strcmp(name, "append")) && argc >= 1) {
    for (int a = 0; a < argc; a++) {
      emit_indent(b, indent);
      buf_printf(b, "sp_%sArray_push(", k);
      emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_expr(c, argv[a], b); buf_puts(b, ");\n");
    }
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
    buf_printf(b, "; %s _t%d = ", c_type_name(ty_hash_key(rt)), tb); emit_hash_key(c, argv[0], ty_hash_key(rt), b);
    buf_puts(b, "; ");
    buf_printf(b, "sp_%sHash_set(_t%d, _t%d, ", hn, ta, tb);
    const char *pf = vt == TY_POLY ?
        (!strcmp(op, "+") ? "sp_poly_add" : !strcmp(op, "-") ? "sp_poly_sub" :
         !strcmp(op, "*") ? "sp_poly_mul" : !strcmp(op, "/") ? "sp_poly_div" :
         !strcmp(op, "%") ? "sp_poly_mod" : !strcmp(op, "**") ? "sp_poly_pow" : NULL) : NULL;
    if (vt == TY_STRING && !strcmp(op, "+")) {
      buf_printf(b, "sp_str_concat(sp_%sHash_get(_t%d, _t%d), ", hn, ta, tb);
      emit_expr(c, v, b); buf_puts(b, ")");
    }
    else if (pf) {
      /* a poly-valued slot folds via the dynamic operator on boxed operands */
      buf_printf(b, "%s(sp_%sHash_get(_t%d, _t%d), ", pf, hn, ta, tb);
      emit_boxed(c, v, b); buf_puts(b, ")");
    }
    else {
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

/* h[k] &&= v  /  h[k] ||= v  /  a[i] &&= v  /  a[i] ||= v.
   IndexAndWriteNode / IndexOrWriteNode. Receiver and key evaluated once. */
static void emit_index_and_or_write(Compiler *c, int id, Buf *b, int indent, int is_or) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int v = nt_ref(nt, id, "value");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  if (argc != 1) { unsupported(c, id, is_or ? "index-or-write" : "index-and-write"); return; }
  TyKind rt = comp_ntype(c, recv);
  int ta = ++g_tmp, tb = ++g_tmp;

  if (ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    if (!hn) { unsupported(c, id, "index and/or write (unknown hash)"); return; }
    TyKind kt = ty_hash_key(rt);
    TyKind vt = ty_hash_val(rt);
    emit_indent(b, indent);
    buf_printf(b, "{ %s _t%d = ", c_type_name(rt), ta); emit_expr(c, recv, b);
    buf_printf(b, "; %s _t%d = ", c_type_name(kt), tb); emit_hash_key(c, argv[0], kt, b);
    buf_puts(b, "; ");
    if (vt == TY_POLY) {
      buf_printf(b, "if (%ssp_poly_truthy(sp_%sHash_get(_t%d, _t%d))) sp_%sHash_set(_t%d, _t%d, ",
                 is_or ? "!" : "", hn, ta, tb, hn, ta, tb);
      emit_boxed(c, v, b);
      buf_puts(b, ")");
    }
    else {
      buf_printf(b, "if (%ssp_%sHash_has_key(_t%d, _t%d)) sp_%sHash_set(_t%d, _t%d, ",
                 is_or ? "!" : "", hn, ta, tb, hn, ta, tb);
      emit_expr(c, v, b);
      buf_puts(b, ")");
    }
    buf_puts(b, "; }\n");
    return;
  }

  if (ty_is_array(rt)) {
    const char *k = array_kind(rt);
    if (!k) { unsupported(c, id, "index and/or write (array kind)"); return; }
    emit_indent(b, indent);
    buf_printf(b, "{ %s _t%d = ", c_type_name(rt), ta); emit_expr(c, recv, b);
    buf_printf(b, "; mrb_int _t%d = ", tb); emit_expr(c, argv[0], b);
    buf_puts(b, "; ");
    if (rt == TY_INT_ARRAY) {
      buf_printf(b, "if (%ssp_IntArray_get(_t%d, _t%d) != SP_INT_NIL) sp_IntArray_set(_t%d, _t%d, ",
                 is_or ? "!" : "", ta, tb, ta, tb);
      emit_expr(c, v, b);
      buf_puts(b, ")");
    }
    else if (rt == TY_STR_ARRAY) {
      buf_printf(b, "if (%ssp_StrArray_get(_t%d, _t%d)) sp_StrArray_set(_t%d, _t%d, ",
                 is_or ? "!" : "", ta, tb, ta, tb);
      emit_expr(c, v, b);
      buf_puts(b, ")");
    }
    else if (rt == TY_POLY_ARRAY) {
      buf_printf(b, "if (%ssp_poly_truthy(sp_PolyArray_get(_t%d, _t%d))) sp_PolyArray_set(_t%d, _t%d, ",
                 is_or ? "!" : "", ta, tb, ta, tb);
      emit_boxed(c, v, b);
      buf_puts(b, ")");
    }
    else {
      unsupported(c, id, "index and/or write (array type)"); return;
    }
    buf_puts(b, "; }\n");
    return;
  }

  unsupported(c, id, is_or ? "index-or-write" : "index-and-write");
}

static int scope_has_return(Compiler *c, int scope_idx) {
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (ty && !strcmp(ty, "ReturnNode") && c->nscope[id] == scope_idx) return 1;
  }
  return 0;
}

/* Inline a call to a free-function yielding method `foo(args) { |bp| ... }`:
   declare the method's locals (renamed to avoid clashing with the call
   site), bind params to args, then emit the method body with yield
   expanding to the block. Returns 1 if handled. */
static int emit_inline_call_x(Compiler *c, int id, Buf *b, int indent, int as_expr) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name) return 0;
  int mi, recv_class = -1;
  int implicit_self = 0;
  if (recv < 0) {
    mi = comp_method_index(c, name);     /* free function */
    if (mi < 0) {                        /* implicit-self instance method */
      Scope *encl = comp_scope_of(c, id);
      if (encl->class_id >= 0) { mi = comp_method_in_chain(c, encl->class_id, name, NULL); implicit_self = 1; }
      else return 0;
    }
  }
  else {
    TyKind rt = comp_ntype(c, recv);
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ConstantReadNode")) {
      /* Cls.method with a yield block: look up as a class method */
      const char *cname = nt_str(nt, recv, "name");
      int ci = cname ? comp_class_index(c, cname) : -1;
      if (ci < 0) return 0;
      mi = comp_cmethod_in_chain(c, ci, name, NULL);
    }
    else if (ty_is_object(rt)) {
      recv_class = ty_object_class(rt);
      mi = comp_method_in_chain(c, recv_class, name, NULL);
    }
    else return 0;
  }
  (void)implicit_self;
  if (mi < 0) return 0;
  Scope *m = &c->scopes[mi];
  if (!m->yields || scope_has_return(c, mi)) return 0;
  int block = nt_ref(nt, id, "block");   /* may be -1: no block passed */
  /* `inner(&)` / `inner(&block)`: a BlockArgumentNode forwards the block
     active at this (already-inlined) site, not a fresh literal. */
  if (block >= 0 && nt_type(nt, block) && !strcmp(nt_type(nt, block), "BlockArgumentNode"))
    block = g_block_id;
  if (g_nren + m->nlocals >= MAX_RENAME) return 0;
  /* Pre-check: every body local must have an emittable type. Bail BEFORE
     writing anything (a mid-emit bail would leave an unbalanced `{`). */
  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && !strcmp(lv->name, m->blk_param)) continue;
    if (!is_scalar_ret(lv->type)) return 0;
  }

  int tag = ++g_tmp;
  int saved_nren = g_nren, saved_block = g_block_id;
  const char *saved_self = g_self;
  const char *saved_bpn = g_block_param_name;
  int saved_yfb = g_yield_block_fallback;
  static char selfbuf[64];
  /* Nested `yield` inside the block body should chain to the block that was
     active before this inline, not to the inner block. */
  g_yield_block_fallback = saved_block;
  g_block_id = block;
  g_block_param_name = m->blk_param;

  if (as_expr) buf_puts(b, "({\n");
  else { emit_indent(b, indent); buf_puts(b, "{\n"); }
  /* instance method: bind self to the receiver (a pointer) */
  if (recv >= 0 && recv_class >= 0) {
    int st = ++g_tmp;
    emit_indent(b, indent + 1);
    buf_printf(b, "sp_%s *_t%d = ", c->classes[recv_class].name, st);
    emit_expr(c, recv, b);
    buf_puts(b, ";\n");
    snprintf(selfbuf, sizeof selfbuf, "_t%d", st);
    g_self = selfbuf;
  }
  int din = indent + 1;

  /* declare method locals under renamed names */
  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && !strcmp(lv->name, m->blk_param)) continue;  /* virtual &block slot */
    snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", lv->name);
    snprintf(g_ren_to[g_nren], sizeof g_ren_to[0], "_y%d_%s", tag, lv->name);
    const char *rn = g_ren_to[g_nren];
    g_nren++;
    emit_indent(b, din);
    emit_ctype(c, lv->type, b);
    buf_printf(b, " lv_%s = %s;\n", rn, lv->type == TY_RANGE ? "(sp_Range){0}" : default_value(lv->type));
    if (needs_root(lv->type)) { emit_indent(b, din); buf_printf(b, "SP_GC_ROOT(lv_%s);\n", rn); }
  }

  /* bind params to call args (args are in the call-site scope: renames off) */
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  for (int i = 0; i < m->nparams; i++) {
    emit_indent(b, din);
    buf_printf(b, "lv__y%d_%s = ", tag, m->pnames[i]);
    int sv = g_nren; g_nren = 0;
    emit_arg_or_default(c, m, i, i < argc ? argv[i] : -1, b);
    g_nren = sv;
    buf_puts(b, ";\n");
  }

  if (as_expr) {
    /* Use a result var so the tail uses assignment, not `return`, in the
       GCC statement-expression ({ ... result_var; }) context. */
    TyKind rt = comp_ntype(c, id);
    int rtag = ++g_tmp;
    char rvbuf[32]; snprintf(rvbuf, sizeof rvbuf, "_t%d", rtag);
    emit_indent(b, din); emit_ctype(c, rt, b);
    buf_printf(b, " _t%d = %s;\n", rtag, default_value(rt));
    const char *sv_rv = g_result_var; g_result_var = rvbuf;
    int sp = g_result_poly; g_result_poly = (rt == TY_POLY);
    emit_stmts_tail(c, m->body, b, din);
    g_result_var = sv_rv; g_result_poly = sp;
    emit_indent(b, din); buf_printf(b, "_t%d;\n", rtag);
  }
  else emit_stmts(c, m->body, b, din);
  if (as_expr) { emit_indent(b, indent); buf_puts(b, "})"); }
  else { emit_indent(b, indent); buf_puts(b, "}\n"); }

  g_nren = saved_nren;
  g_block_id = saved_block;
  g_self = saved_self;
  g_block_param_name = saved_bpn;
  g_yield_block_fallback = saved_yfb;
  return 1;
}

static int emit_inline_call(Compiler *c, int id, Buf *b, int indent) {
  return emit_inline_call_x(c, id, b, indent, 0);
}

/* Is `id` a `<&block-param>.call(...)` invocation of the active block? */
static int is_block_call(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (!g_block_param_name || !g_block_param_name[0] || g_block_id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty || strcmp(ty, "CallNode")) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || (strcmp(nm, "call") && strcmp(nm, "()") && strcmp(nm, "[]") && strcmp(nm, "yield"))) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || strcmp(nt_type(nt, recv), "LocalVariableReadNode")) return 0;
  const char *rn = nt_str(nt, recv, "name");
  return rn && !strcmp(rn, g_block_param_name);
}

/* Expand the active block's body, binding its params to the given call
   args. Shared by YieldNode and `block.call`. `as_expr` wraps in ({...}). */
static void emit_block_invoke(Compiler *c, int args_node, Buf *b, int indent, int as_expr) {
  const NodeTable *nt = c->nt;
  int blk = g_block_id;
  int bbody = nt_ref(nt, blk, "body");
  int yc = 0;
  const int *yargs = args_node >= 0 ? nt_arr(nt, args_node, "arguments", &yc) : NULL;
  Scope *bsc = comp_scope_of(c, blk);
  if (as_expr) buf_puts(b, "({ ");
  for (int k = 0; ; k++) {
    const char *bp = block_param_name(c, blk, k);
    if (!bp) break;
    /* When inside an inlined method, block params may be renamed (e.g. x →
       _y3_x); apply the rename table so we write the right C variable. */
    const char *bpr = rename_local(bp);
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "lv_%s = ", bpr);
    if (k < yc) emit_expr(c, yargs[k], b);
    else {
      LocalVar *bl = scope_local(bsc, bp);
      TyKind bt = bl ? bl->type : TY_INT;
      buf_puts(b, bt == TY_RANGE ? "(sp_Range){0}" : default_value(bt));
    }
    buf_puts(b, as_expr ? "; " : ";\n");
  }
  /* Keep the rename table active for the block body: the block's variable
     references are in the same lexical scope as the surrounding inlined
     method, so renames like x → _y3_x must stay visible. Nested inlines
     inside the block body append at the current g_nren and self-restore.
     Set g_block_id to the fallback (the block active before the enclosing
     inline started) so that a nested `yield` inside the block chains to
     the outermost caller's block rather than going dead. */
  int svb = g_block_id; g_block_id = g_yield_block_fallback;
  const char *svbpn = g_block_param_name; g_block_param_name = NULL;
  emit_stmts(c, bbody, b, as_expr ? 0 : indent);
  g_block_id = svb; g_block_param_name = svbpn;
  if (as_expr) buf_puts(b, "})");
}

/* Inline a yielding method call in expression position: ({ ...; value; }).
   The method must return a usable value (its body's last statement). */
static int emit_inline_expr(Compiler *c, int id, Buf *b) {
  /* only when a value is actually produced (scalar return) */
  TyKind rt = comp_ntype(c, id);
  if (!is_scalar_ret(rt)) return 0;
  return emit_inline_call_x(c, id, b, g_indent + 1, 1);
}

/* Block iteration lowered to an inline C for-loop. Handles n.times,
   array.each, range.each, n.upto/downto. Returns 1 if handled. */
/* Emit `lv_<p0> = <expr_src>` boxing if p0 is poly and src is concrete. */
static void emit_iter_param_assign(Compiler *c, int block, const char *p0_orig,
                                   const char *p0_ren, TyKind src_type,
                                   const char *src_expr, Buf *b, int indent) {
  Scope *sc = comp_scope_of(c, block);
  LocalVar *lv = sc ? scope_local(sc, p0_orig) : NULL;
  TyKind pt = lv ? lv->type : src_type;
  emit_indent(b, indent);
  if (pt == TY_POLY && src_type != TY_POLY) {
    Buf bx; memset(&bx, 0, sizeof bx);
    emit_boxed_text(c, src_type, src_expr, &bx);
    buf_printf(b, "lv_%s = %s;\n", p0_ren, bx.p ? bx.p : src_expr);
    free(bx.p);
  }
  else {
    buf_printf(b, "lv_%s = %s;\n", p0_ren, src_expr);
  }
}

static int emit_iteration_stmt(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name) return 0;

  /* loop { ... } -- infinite loop, exited by break */
  if (recv < 0 && !strcmp(name, "loop")) {
    int lbody = nt_ref(nt, block, "body");
    emit_indent(b, indent); buf_puts(b, "for (;;) {\n");
    emit_stmts(c, lbody, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  if (recv < 0) return 0;
  int body = nt_ref(nt, block, "body");
  const char *p0_orig = block_param_name(c, block, 0);
  const char *p0 = p0_orig ? rename_local(p0_orig) : NULL;
  TyKind rt = comp_ntype(c, recv);

  /* n.times { |i| ... } */
  if (!strcmp(name, "times") && rt == TY_INT) {
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb);
    emit_expr(c, recv, &rb);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < ", t, t);
    buf_puts(b, rb.p); buf_printf(b, "; _t%d++) {\n", t);
    if (p0) { char ts[32]; snprintf(ts, sizeof ts, "_t%d", t); emit_iter_param_assign(c, block, p0_orig, p0, TY_INT, ts, b, indent + 1); }
    emit_stmts(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    free(rb.p);
    return 1;
  }

  /* num.step(limit[, step]) { [|i|] ... } -- stepping loop. A float receiver
     or a float limit/step makes it a float walk (yielding floats), computed
     by iteration count to avoid floating-point drift (CRuby semantics). */
  if (!strcmp(name, "step") && (rt == TY_INT || rt == TY_FLOAT)) {
    int args = nt_ref(nt, id, "arguments");
    int sargc = 0;
    const int *sargv = args >= 0 ? nt_arr(nt, args, "arguments", &sargc) : NULL;
    if (sargc < 1) return 0;
    int is_float = (rt == TY_FLOAT) || comp_ntype(c, sargv[0]) == TY_FLOAT ||
                   (sargc >= 2 && comp_ntype(c, sargv[1]) == TY_FLOAT);
    if (!is_float) {
      int t = ++g_tmp, tl = ++g_tmp, ts = ++g_tmp;
      emit_indent(b, indent); buf_printf(b, "mrb_int _t%d = ", tl); emit_expr(c, sargv[0], b); buf_puts(b, ";\n");
      emit_indent(b, indent); buf_printf(b, "mrb_int _t%d = ", ts);
      if (sargc >= 2) emit_expr(c, sargv[1], b); else buf_puts(b, "1");
      buf_puts(b, ";\n");
      emit_indent(b, indent);
      buf_printf(b, "for (mrb_int _t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; _t%d >= 0 ? _t%d <= _t%d : _t%d >= _t%d; _t%d += _t%d) {\n",
                 ts, t, tl, t, tl, t, ts);
      if (p0) { char ts2[32]; snprintf(ts2, sizeof ts2, "_t%d", t); emit_iter_param_assign(c, block, p0_orig, p0, TY_INT, ts2, b, indent + 1); }
      emit_stmts(c, body, b, indent + 1);
      emit_indent(b, indent); buf_puts(b, "}\n");
      return 1;
    }
    int tb = ++g_tmp, tl = ++g_tmp, ts = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp;
    emit_indent(b, indent); buf_printf(b, "mrb_float _t%d = ", tb); emit_expr(c, recv, b); buf_puts(b, ";\n");
    emit_indent(b, indent); buf_printf(b, "mrb_float _t%d = ", tl); emit_expr(c, sargv[0], b); buf_puts(b, ";\n");
    emit_indent(b, indent); buf_printf(b, "mrb_float _t%d = ", ts);
    if (sargc >= 2) emit_expr(c, sargv[1], b); else buf_puts(b, "1.0");
    buf_puts(b, ";\n");
    /* n = floor((limit-begin)/step + err); err bounds fp drift (CRuby) */
    emit_indent(b, indent);
    buf_printf(b, "mrb_float _t%d_e = (fabs(_t%d)+fabs(_t%d)+fabs(_t%d-_t%d))/fabs(_t%d)*DBL_EPSILON;\n",
               tn, tb, tl, tl, tb, ts);
    emit_indent(b, indent);
    buf_printf(b, "if (_t%d_e > 0.5) _t%d_e = 0.5;\n", tn, tn);
    emit_indent(b, indent);
    buf_printf(b, "mrb_int _t%d = (mrb_int)floor((_t%d-_t%d)/_t%d + _t%d_e);\n", tn, tl, tb, ts, tn);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d <= _t%d; _t%d++) {\n", ti, ti, tn, ti);
    if (p0) { char fp_expr[64]; snprintf(fp_expr, sizeof fp_expr, "_t%d + _t%d * _t%d", tb, ti, ts); emit_iter_param_assign(c, block, p0_orig, p0, TY_FLOAT, fp_expr, b, indent + 1); }
    emit_stmts(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* hash.each / each_pair { |k, v| ... } */
  if ((!strcmp(name, "each") || !strcmp(name, "each_pair")) && ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    if (!hn) return 0;
    const char *p1 = block_param_name(c, block, 1); if (p1) p1 = rename_local(p1);
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

  /* hash.each_value { |v| ... } / each_key { |k| ... } -- single param */
  if ((!strcmp(name, "each_value") || !strcmp(name, "each_key")) && ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    if (!hn) return 0;
    int is_val = !strcmp(name, "each_value");
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb);
    emit_expr(c, recv, &rb);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < ", t, t);
    buf_puts(b, rb.p); buf_printf(b, "->len; _t%d++) {\n", t);
    if (p0) {
      /* The param may be poly (shared name across hashes of differing
         element types); box a concrete element into the poly slot. */
      const char *raw = block_param_name(c, block, 0);
      LocalVar *pv = raw ? scope_local(comp_scope_of(c, block), raw) : NULL;
      TyKind want = is_val ? ty_hash_val(rt) : ty_hash_key(rt);
      int box = pv && pv->type == TY_POLY && want != TY_POLY;
      char src[256];
      if (rt == TY_POLY_POLY_HASH) {
        /* PolyPolyHash: ->order[i] is an index; keys/vals hold sp_RbVal */
        if (is_val)
          snprintf(src, sizeof src, "%s->vals[%s->order[_t%d]]", rb.p, rb.p, t);
        else
          snprintf(src, sizeof src, "%s->keys[%s->order[_t%d]]", rb.p, rb.p, t);
      }
      else if (is_val)
        snprintf(src, sizeof src, "sp_%sHash_get(%s, %s->order[_t%d])", hn, rb.p, rb.p, t);
      else
        snprintf(src, sizeof src, "%s->order[_t%d]", rb.p, t);
      emit_indent(b, indent + 1);
      buf_printf(b, "lv_%s = ", p0);
      if (box) emit_boxed_text(c, want, src, b);
      else buf_puts(b, src);
      buf_puts(b, ";\n");
    }
    emit_stmts(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    free(rb.p);
    return 1;
  }

  /* array.each_with_index { |x, i| ... } */
  if (!strcmp(name, "each_with_index") && ty_is_array(rt)) {
    const char *k = array_kind(rt);
    if (!k) return 0;
    const char *p1 = block_param_name(c, block, 1); if (p1) p1 = rename_local(p1);
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    Scope *cs_ewi = comp_scope_of(c, id);
    LocalVar *clv_ewi_p1 = (p1 && cs_ewi) ? scope_local(cs_ewi, p1) : NULL;
    LocalVar *clv_ewi_p0 = (p0 && cs_ewi) ? scope_local(cs_ewi, p0) : NULL;
    int p1_box_poly = clv_ewi_p1 && clv_ewi_p1->type == TY_POLY;
    /* Save outer variables before loop */
    int ts_p0 = 0, ts_p1 = 0;
    if (p0 && clv_ewi_p0) {
      ts_p0 = ++g_tmp; Buf ot; memset(&ot, 0, sizeof ot); emit_ctype(c, clv_ewi_p0->type, &ot);
      emit_indent(b, indent); buf_printf(b, "%s _t%d = lv_%s;\n", ot.p ? ot.p : "sp_RbVal", ts_p0, p0); free(ot.p);
    }
    if (p1 && clv_ewi_p1) {
      ts_p1 = ++g_tmp; Buf ot; memset(&ot, 0, sizeof ot); emit_ctype(c, clv_ewi_p1->type, &ot);
      emit_indent(b, indent); buf_printf(b, "%s _t%d = lv_%s;\n", ot.p ? ot.p : "sp_RbVal", ts_p1, p1); free(ot.p);
    }
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(", t, t, k);
    buf_puts(b, rb.p); buf_printf(b, "); _t%d++) {\n", t);
    if (p0) {
      emit_indent(b, indent + 1);
      buf_printf(b, "lv_%s = sp_%sArray_get(", p0, k);
      buf_puts(b, rb.p); buf_printf(b, ", _t%d);\n", t);
    }
    if (p1) {
      emit_indent(b, indent + 1);
      if (p1_box_poly) buf_printf(b, "lv_%s = sp_box_int(_t%d);\n", p1, t);
      else buf_printf(b, "lv_%s = _t%d;\n", p1, t);
    }
    emit_stmts(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    /* Restore outer variables */
    if (p0 && ts_p0 > 0) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p0, ts_p0); }
    if (p1 && ts_p1 > 0) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p1, ts_p1); }
    free(rb.p);
    return 1;
  }

  /* poly_val.each { |v| ... }: runtime-dispatch over a boxed array */
  if (!strcmp(name, "each") && rt == TY_POLY && block >= 0 && p0) {
    int ta = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); buf_printf(b, "sp_RbVal _t%d = %s;\n", ta, rb.p ? rb.p : "sp_box_nil()"); free(rb.p);
    emit_indent(b, indent); buf_printf(b, "mrb_int _t%d = sp_poly_arr_len(_t%d);\n", tn, ta);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) {\n", ti, ti, tn, ti);
    emit_indent(b, indent + 1);
    buf_printf(b, "lv_%s = sp_poly_arr_get(_t%d, _t%d);\n", p0, ta, ti);
    emit_stmts(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* array.each { |x| ... } */
  if (!strcmp(name, "each") && rt == TY_POLY_ARRAY) {
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    int ta = ++g_tmp;
    /* Detect block param shadowing an outer variable; save/restore to preserve outer value */
    Scope *cs_pa = p0 ? comp_scope_of(c, id) : NULL;
    LocalVar *outer_pa = (p0 && cs_pa) ? scope_local(cs_pa, p0) : NULL;
    int ts_pa = 0;
    if (outer_pa) {
      ts_pa = ++g_tmp; Buf ot_pa; memset(&ot_pa, 0, sizeof ot_pa); emit_ctype(c, outer_pa->type, &ot_pa);
      emit_indent(b, indent); buf_printf(b, "%s _t%d = lv_%s;\n", ot_pa.p ? ot_pa.p : "sp_RbVal", ts_pa, p0); free(ot_pa.p);
    }
    emit_indent(b, indent);
    buf_printf(b, "sp_PolyArray *_t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n", t, t, ta, t);
    if (p0) {
      /* Destructuring: 2+ params over poly_array where params are scalar-typed */
      const char *orig_p0n = block_param_name(c, block, 0);
      Scope *blk_sp = comp_scope_of(c, block);
      LocalVar *bp0p = orig_p0n ? scope_local(blk_sp, orig_p0n) : NULL;
      TyKind bp0_tp = bp0p ? bp0p->type : TY_UNKNOWN;
      int npp = 0; while (block_param_name(c, block, npp)) npp++;
      int did_destruct = 0;
      if (npp >= 2 && bp0_tp != TY_POLY && bp0_tp != TY_UNKNOWN) {
        const char *inner_kk = array_kind(ty_array_of(bp0_tp));
        if (inner_kk) {
          int tsub = ++g_tmp;
          emit_indent(b, indent + 1);
          buf_printf(b, "sp_%sArray *_t%d = (sp_%sArray *)sp_PolyArray_get(_t%d, _t%d).v.p;\n",
                     inner_kk, tsub, inner_kk, ta, t);
          for (int pj = 0; pj < npp; pj++) {
            const char *pnj = block_param_name(c, block, pj);
            if (!pnj) continue;
            emit_indent(b, indent + 1);
            buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, %d);\n",
                       rename_local(pnj), inner_kk, tsub, pj);
          }
          did_destruct = 1;
        }
      }
      if (!did_destruct) {
        emit_indent(b, indent + 1);
        buf_printf(b, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", p0, ta, t);
      }
    }
    emit_stmts(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    if (outer_pa) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p0, ts_pa); }
    return 1;
  }
  if ((!strcmp(name, "each") || !strcmp(name, "each_entry") || !strcmp(name, "reverse_each")) &&
      ty_is_array(rt)) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) return 0;
    int rev = !strcmp(name, "reverse_each");
    int t = ++g_tmp, tn = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb);
    emit_expr(c, recv, &rb);
    /* Detect block param shadowing an outer variable; save/restore to preserve outer value */
    TyKind et = p0 ? ty_array_elem(rt) : TY_UNKNOWN;
    Scope *cs = p0 ? comp_scope_of(c, id) : NULL;
    LocalVar *outer = (p0 && cs) ? scope_local(cs, p0) : NULL;
    int box_to_poly = outer && outer->type == TY_POLY && et != TY_POLY;
    int ts = 0;
    if (outer) {
      /* Block params shadow outer variables in Ruby; save and restore */
      ts = ++g_tmp;
      Buf ot_ea; memset(&ot_ea, 0, sizeof ot_ea); emit_ctype(c, outer->type, &ot_ea);
      emit_indent(b, indent);
      buf_printf(b, "%s _t%d = lv_%s;\n", ot_ea.p ? ot_ea.p : "sp_RbVal", ts, p0); free(ot_ea.p);
    }
    if (rev) { emit_indent(b, indent); buf_printf(b, "mrb_int _t%d = sp_%sArray_length(%s);\n", tn, k, rb.p); }
    emit_indent(b, indent);
    if (rev) buf_printf(b, "for (mrb_int _t%d = _t%d - 1; _t%d >= 0; _t%d--) {\n", t, tn, t, t);
    else {
      buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(", t, t, k);
      buf_puts(b, rb.p); buf_printf(b, "); _t%d++) {\n", t);
    }
    if (p0) {
      /* Destructuring: 2+ params over poly_array where params are scalar-typed
         (e.g. `[[1,2],[3,4]].each { |a,b| }` or numbered `{ _1; _2 }`).
         The poly element is an inner typed array; unbox and destructure. */
      Scope *blk_s = comp_scope_of(c, block);
      /* Use original (unrenameD) name for scope lookup; p0 is already renamed */
      const char *orig_p0_name = block_param_name(c, block, 0);
      LocalVar *bp0 = orig_p0_name ? scope_local(blk_s, orig_p0_name) : NULL;
      TyKind bp0_type = bp0 ? bp0->type : TY_UNKNOWN;
      int np = 0; while (block_param_name(c, block, np)) np++;
      if (np >= 2 && !strcmp(k, "Poly") && bp0_type != TY_POLY && bp0_type != TY_UNKNOWN) {
        /* Get the inner array kind from the first param's element type */
        const char *inner_k = array_kind(ty_array_of(bp0_type));
        if (inner_k) {
          int tsub = ++g_tmp;
          emit_indent(b, indent + 1);
          buf_printf(b, "sp_%sArray *_t%d = (sp_%sArray *)sp_PolyArray_get(", inner_k, tsub, inner_k);
          buf_puts(b, rb.p); buf_printf(b, ", _t%d).v.p;\n", t);
          for (int pj = 0; pj < np; pj++) {
            const char *pname2 = block_param_name(c, block, pj);
            if (!pname2) continue;
            emit_indent(b, indent + 1);
            buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, %d);\n",
                       rename_local(pname2), inner_k, tsub, pj);
          }
          goto each_body;
        }
      }
      emit_indent(b, indent + 1);
      if (box_to_poly) {
        if (et == TY_INT) buf_printf(b, "lv_%s = sp_box_int(sp_%sArray_get(", p0, k);
        else if (et == TY_STRING) buf_printf(b, "lv_%s = sp_box_str(sp_%sArray_get(", p0, k);
        else if (et == TY_FLOAT) buf_printf(b, "lv_%s = sp_box_float(sp_%sArray_get(", p0, k);
        else if (et == TY_BOOL) buf_printf(b, "lv_%s = sp_box_bool(sp_%sArray_get(", p0, k);
        else buf_printf(b, "lv_%s = sp_%sArray_get(", p0, k);
        buf_puts(b, rb.p); buf_printf(b, ", _t%d)", t);
        if (et == TY_INT || et == TY_STRING || et == TY_FLOAT || et == TY_BOOL) buf_puts(b, ")");
        buf_puts(b, ";\n");
      }
      else {
        buf_printf(b, "lv_%s = sp_%sArray_get(", p0, k);
        buf_puts(b, rb.p); buf_printf(b, ", _t%d);\n", t);
      }
    }
    each_body:
    emit_stmts(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    if (outer) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p0, ts); }
    free(rb.p);
    return 1;
  }

  /* int_array.combination(k) { |c| ... } -- yield each k-combination as a
     fresh int_array */
  if (!strcmp(name, "combination") && rt == TY_INT_ARRAY) {
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (ac != 1) return 0;
    int ta = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); buf_printf(b, "{ sp_IntArray *_t%d = ", ta); buf_puts(b, rb.p ? rb.p : ""); buf_puts(b, ";\n"); free(rb.p);
    emit_indent(b, indent + 1); buf_printf(b, "sp_PtrArray *_t%d = sp_IntArray_combination(_t%d, ", tc, ta); emit_expr(c, av[0], b); buf_puts(b, "); SP_GC_ROOT(_t"); buf_printf(b, "%d);\n", tc);
    emit_indent(b, indent + 1); buf_printf(b, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, tc, ti);
    if (p0) { emit_indent(b, indent + 2); buf_printf(b, "lv_%s = (sp_IntArray *)_t%d->data[_t%d];\n", p0, tc, ti); }
    emit_stmts(c, body, b, indent + 2);
    emit_indent(b, indent + 1); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* array.each_cons(n) { |a, b, ...| } -- sliding window of n consecutive
     elements; a single param binds the n-element sub-array, multiple params
     destructure the window */
  if (!strcmp(name, "each_cons") && ty_is_array(rt)) {
    int args = nt_ref(nt, id, "arguments");
    int ec = 0; const int *eav = args >= 0 ? nt_arr(nt, args, "arguments", &ec) : NULL;
    if (ec != 1) return 0;
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) return 0;
    int np = 0; while (block_param_name(c, block, np)) np++;
    int ta = ++g_tmp, tnn = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); emit_ctype(c, rt, b); buf_printf(b, " _t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
    emit_indent(b, indent); buf_printf(b, "mrb_int _t%d = ", tnn); emit_expr(c, eav[0], b); buf_puts(b, ";\n");
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d + _t%d - 1 < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, tnn, k, ta, ti);
    if (np == 1) {
      const char *pn = block_param_name(c, block, 0);
      const char *rpn = rename_local(pn);
      Scope *csc_ec = comp_scope_of(c, block);
      LocalVar *clv_ec = csc_ec ? scope_local(csc_ec, pn) : NULL;
      TyKind csaved_ec = clv_ec ? clv_ec->type : TY_UNKNOWN;
      int use_shadow_ec = clv_ec && clv_ec->type != rt && rt != TY_UNKNOWN;
      if (use_shadow_ec) {
        int bodyBn = 0; const int *bodyBb = body >= 0 ? nt_arr(nt, body, "body", &bodyBn) : NULL;
        clv_ec->type = rt;
        for (int j = 0; j < bodyBn; j++) infer_type(c, bodyBb[j]);
        emit_indent(b, indent + 1); buf_puts(b, "{\n");
        emit_indent(b, indent + 2); emit_ctype(c, rt, b);
        buf_printf(b, " lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n", rpn, k, ta, ti, tnn);
        emit_stmts(c, body, b, indent + 2);
        emit_indent(b, indent + 1); buf_puts(b, "}\n");
        clv_ec->type = csaved_ec;
      }
      else {
        emit_indent(b, indent + 1);
        buf_printf(b, "lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n", rpn, k, ta, ti, tnn);
        emit_stmts(c, body, b, indent + 1);
      }
    }
    else {
      for (int pj = 0; pj < np; pj++) {
        const char *pn = block_param_name(c, block, pj);
        emit_indent(b, indent + 1);
        buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d + %d);\n", rename_local(pn), k, ta, ti, pj);
      }
      emit_stmts(c, body, b, indent + 1);
    }
    emit_indent(b, indent); buf_puts(b, "}\n");
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

  /* "a".upto("e") { |c| ... } -- string succ-sequence loop, mirrors
     sp_StrArray_from_string_range semantics (inclusive, 4096-cap) */
  if (!strcmp(name, "upto") && rt == TY_STRING && p0) {
    int args = nt_ref(nt, id, "arguments");
    int argc = 0;
    const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
    if (argc != 1) return 0;
    int te = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp, tcmp = ++g_tmp;
    emit_indent(b, indent); buf_printf(b, "const char *_t%d = ", te); emit_expr(c, argv[0], b); buf_puts(b, ";\n");
    emit_indent(b, indent); buf_printf(b, "const char *_t%d = ", tc); emit_expr(c, recv, b); buf_puts(b, ";\n");
    emit_indent(b, indent); buf_printf(b, "for (int _t%d = 0; _t%d < 4096; _t%d++) {\n", ti, ti, ti);
    emit_indent(b, indent + 1); buf_printf(b, "int _t%d = strcmp(_t%d, _t%d);\n", tcmp, tc, te);
    emit_indent(b, indent + 1); buf_printf(b, "if (_t%d > 0) break;\n", tcmp);
    emit_indent(b, indent + 1); buf_printf(b, "lv_%s = _t%d;\n", p0, tc);
    emit_stmts(c, body, b, indent + 1);
    emit_indent(b, indent + 1); buf_printf(b, "if (_t%d == 0) break;\n", tcmp);
    emit_indent(b, indent + 1); buf_printf(b, "_t%d = sp_str_succ(_t%d);\n", tc, tc);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* recv.tap { |p| body } -- run block for side effects, preserve outer var */
  if (!strcmp(name, "tap") && recv >= 0) {
    TyKind et = infer_type(c, recv);
    Scope *tsc = p0 ? comp_scope_of(c, block) : NULL;
    LocalVar *tlv0 = (tsc && p0) ? scope_local(tsc, p0) : NULL;
    TyKind tsaved0 = tlv0 ? tlv0->type : TY_UNKNOWN;
    int use_shadow_t = tlv0 && tlv0->type != et && et != TY_UNKNOWN;
    int tr = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); emit_ctype(c, et, b);
    buf_printf(b, " _t%d = %s;\n", tr, rb.p ? rb.p : ""); free(rb.p);
    if (use_shadow_t) {
      int tbody_bn = 0; const int *tbody_bb = body >= 0 ? nt_arr(nt, body, "body", &tbody_bn) : NULL;
      tlv0->type = et;
      for (int j = 0; j < tbody_bn; j++) infer_type(c, tbody_bb[j]);
      emit_indent(b, indent); buf_puts(b, "{\n");
      emit_indent(b, indent + 1); emit_ctype(c, et, b);
      buf_printf(b, " lv_%s = _t%d;\n", p0, tr);
      emit_stmts(c, body, b, indent + 1);
      emit_indent(b, indent); buf_puts(b, "}\n");
      tlv0->type = tsaved0;
    }
    else {
      if (p0) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p0, tr); }
      emit_stmts(c, body, b, indent);
    }
    return 1;
  }

  /* array.cycle(n) { |p| body } -- repeat n times over the array */
  if (!strcmp(name, "cycle") && ty_is_array(rt)) {
    int args = nt_ref(nt, id, "arguments");
    int cyc_argc = 0; const int *cyc_argv = args >= 0 ? nt_arr(nt, args, "arguments", &cyc_argc) : NULL;
    if (cyc_argc != 1) return 0;
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) return 0;
    TyKind et = ty_array_elem(rt);
    Scope *csc = p0 ? comp_scope_of(c, block) : NULL;
    LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
    TyKind csaved0 = clv0 ? clv0->type : TY_UNKNOWN;
    int use_shadow_cy = clv0 && clv0->type != et && et != TY_UNKNOWN;
    int ta = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); emit_ctype(c, rt, b);
    buf_printf(b, " _t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
    emit_indent(b, indent); buf_printf(b, "mrb_int _t%d = ", tn);
    emit_expr(c, cyc_argv[0], b); buf_puts(b, ";\n");
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) {\n", ti, ti, tn, ti);
    emit_indent(b, indent + 1);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", tj, tj, k, ta, tj);
    int innerIndent = indent + 2;
    if (use_shadow_cy) {
      int cyb_bn = 0; const int *cyb_bb = body >= 0 ? nt_arr(nt, body, "body", &cyb_bn) : NULL;
      clv0->type = et;
      for (int j = 0; j < cyb_bn; j++) infer_type(c, cyb_bb[j]);
      emit_indent(b, innerIndent); buf_puts(b, "{\n"); innerIndent++;
      emit_indent(b, innerIndent); emit_ctype(c, et, b);
      buf_printf(b, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, ta, tj);
      emit_stmts(c, body, b, innerIndent);
      innerIndent--;
      emit_indent(b, innerIndent); buf_puts(b, "}\n");
      clv0->type = csaved0;
    }
    else {
      if (p0) { emit_indent(b, innerIndent); buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, ta, tj); }
      emit_stmts(c, body, b, innerIndent);
    }
    emit_indent(b, indent + 1); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* array.each_slice(n) { |p| body } -- yield subarrays of size n */
  if (!strcmp(name, "each_slice") && ty_is_array(rt)) {
    int args = nt_ref(nt, id, "arguments");
    int es_argc = 0; const int *es_argv = args >= 0 ? nt_arr(nt, args, "arguments", &es_argc) : NULL;
    if (es_argc != 1) return 0;
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) return 0;
    Scope *csc = p0 ? comp_scope_of(c, block) : NULL;
    LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
    TyKind csaved0 = clv0 ? clv0->type : TY_UNKNOWN;
    int use_shadow_es = clv0 && clv0->type != rt && rt != TY_UNKNOWN;
    int ta = ++g_tmp, ts = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); emit_ctype(c, rt, b);
    buf_printf(b, " _t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
    emit_indent(b, indent); buf_printf(b, "mrb_int _t%d = ", ts);
    emit_expr(c, es_argv[0], b); buf_puts(b, ";\n");
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d += _t%d) {\n",
               ti, ti, k, ta, ti, ts);
    int bodyIndent = indent + 1;
    if (use_shadow_es) {
      int esb_bn = 0; const int *esb_bb = body >= 0 ? nt_arr(nt, body, "body", &esb_bn) : NULL;
      clv0->type = rt;
      for (int j = 0; j < esb_bn; j++) infer_type(c, esb_bb[j]);
      emit_indent(b, bodyIndent); buf_puts(b, "{\n"); bodyIndent++;
      emit_indent(b, bodyIndent); emit_ctype(c, rt, b);
      buf_printf(b, " lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n", p0, k, ta, ti, ts);
      emit_stmts(c, body, b, bodyIndent);
      bodyIndent--;
      emit_indent(b, bodyIndent); buf_puts(b, "}\n");
      clv0->type = csaved0;
    }
    else {
      if (p0) { emit_indent(b, bodyIndent); buf_printf(b, "lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n", p0, k, ta, ti, ts); }
      emit_stmts(c, body, b, bodyIndent);
    }
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* str.scan(/re/) { |m| body } -- iterate over regex matches */
  if (!strcmp(name, "scan") && rt == TY_STRING) {
    int args = nt_ref(nt, id, "arguments");
    int sc_argc = 0; const int *sc_argv = args >= 0 ? nt_arr(nt, args, "arguments", &sc_argc) : NULL;
    if (sc_argc != 1 || re_lit_index(c, sc_argv[0]) < 0) return 0;
    TyKind et = TY_STRING;
    Scope *csc = p0 ? comp_scope_of(c, block) : NULL;
    LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
    TyKind csaved0 = clv0 ? clv0->type : TY_UNKNOWN;
    int use_shadow_sc = clv0 && clv0->type != et && et != TY_UNKNOWN;
    int tm = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent);
    buf_printf(b, "sp_StrArray *_t%d = sp_re_scan(sp_re_pat_%d, %s);\n",
               tm, re_lit_index(c, sc_argv[0]), rb.p ? rb.p : ""); free(rb.p);
    emit_indent(b, indent); buf_printf(b, "SP_GC_ROOT(_t%d);\n", tm);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_StrArray_length(_t%d); _t%d++) {\n",
               ti, ti, tm, ti);
    int bodyIndent = indent + 1;
    if (use_shadow_sc) {
      int scb_bn = 0; const int *scb_bb = body >= 0 ? nt_arr(nt, body, "body", &scb_bn) : NULL;
      clv0->type = et;
      for (int j = 0; j < scb_bn; j++) infer_type(c, scb_bb[j]);
      emit_indent(b, bodyIndent); buf_puts(b, "{\n"); bodyIndent++;
      emit_indent(b, bodyIndent); buf_printf(b, "const char *lv_%s = sp_StrArray_get(_t%d, _t%d);\n", p0, tm, ti);
      emit_stmts(c, body, b, bodyIndent);
      bodyIndent--;
      emit_indent(b, bodyIndent); buf_puts(b, "}\n");
      clv0->type = csaved0;
    }
    else {
      if (p0) { emit_indent(b, bodyIndent); buf_printf(b, "lv_%s = sp_StrArray_get(_t%d, _t%d);\n", p0, tm, ti); }
      emit_stmts(c, body, b, bodyIndent);
    }
    emit_indent(b, indent); buf_puts(b, "}\n");
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
        unsigned char ch = (unsigned char)*p;
        if (ch == '%') buf_puts(&fmt, "%%");
        else if (ch == '\\') buf_puts(&fmt, "\\\\");
        else if (ch == '"') buf_puts(&fmt, "\\\"");
        else if (ch == '\n') buf_puts(&fmt, "\\n");
        else if (ch == '\t') buf_puts(&fmt, "\\t");
        else if (ch == '\r') buf_puts(&fmt, "\\r");
        else if (ch >= 0x20 && ch < 0x7f) buf_printf(&fmt, "%c", ch);
        else buf_printf(&fmt, "\\%03o", ch);
      }
    }
    else if (pty && !strcmp(pty, "EmbeddedStatementsNode")) {
      int s = nt_ref(nt, pid, "statements");
      int bn = 0;
      const int *body = s >= 0 ? nt_arr(nt, s, "body", &bn) : NULL;
      int expr = bn > 0 ? body[bn - 1] : -1;
      /* `#{ s1; s2; ...; sN }` evaluates every statement in order and uses sN's
         value. Run the leading statements for side effects; if sN is itself an
         assignment, perform it and read the assigned variable back as the value. */
      char vexpr[48]; vexpr[0] = 0;
      for (int si = 0; si + 1 < bn; si++) emit_stmt(c, body[si], g_pre, g_indent);
      const char *ety = expr >= 0 ? nt_type(nt, expr) : NULL;
      TyKind t = comp_ntype(c, expr);
      if (ety && (!strcmp(ety, "LocalVariableWriteNode") || !strcmp(ety, "LocalVariableOperatorWriteNode") ||
                  !strcmp(ety, "LocalVariableOrWriteNode") || !strcmp(ety, "LocalVariableAndWriteNode"))) {
        emit_stmt(c, expr, g_pre, g_indent);
        const char *vn = nt_str(nt, expr, "name");
        LocalVar *lvp = vn ? scope_local(comp_scope_of(c, expr), vn) : NULL;
        if (lvp) t = lvp->type;
        int tv = ++g_tmp;
        emit_indent(g_pre, g_indent); emit_ctype(c, t, g_pre);
        buf_printf(g_pre, " _t%d = ", tv); emit_local_ref(c, expr, vn, g_pre); buf_puts(g_pre, ";\n");
        snprintf(vexpr, sizeof vexpr, "_t%d", tv);
      }
      #define EMIT_IV() do { if (vexpr[0]) buf_puts(&argbuf, vexpr); else emit_expr(c, expr, &argbuf); } while (0)
      buf_puts(&argbuf, ", ");
      if (t == TY_INT) {
        buf_puts(&fmt, "%lld"); buf_puts(&argbuf, "(long long)");
        EMIT_IV();
      }
      else if (t == TY_STRING) {
        /* a nullable string (NULL) interpolates as the empty string */
        buf_puts(&fmt, "%s"); buf_puts(&argbuf, "("); EMIT_IV(); buf_puts(&argbuf, " ?: \"\")");
      }
      else if (t == TY_FLOAT) {
        buf_puts(&fmt, "%s"); buf_puts(&argbuf, "sp_float_to_s(");
        EMIT_IV(); buf_puts(&argbuf, ")");
      }
      else if (t == TY_BOOL) {
        buf_puts(&fmt, "%s"); buf_puts(&argbuf, "(");
        EMIT_IV(); buf_puts(&argbuf, " ? \"true\" : \"false\")");
      }
      else if (t == TY_SYMBOL) {
        buf_puts(&fmt, "%s"); buf_puts(&argbuf, "sp_sym_to_s(");
        EMIT_IV(); buf_puts(&argbuf, ")");
      }
      else if (t == TY_POLY) {
        buf_puts(&fmt, "%s"); buf_puts(&argbuf, "sp_poly_to_s(");
        EMIT_IV(); buf_puts(&argbuf, ")");
      }
      else if (t == TY_EXCEPTION) {
        buf_puts(&fmt, "%s"); buf_puts(&argbuf, "sp_exc_message(");
        EMIT_IV(); buf_puts(&argbuf, ")");
      }
      else if (t == TY_NIL) {
        buf_puts(&fmt, "%s"); buf_puts(&argbuf, "((void)(");
        EMIT_IV(); buf_puts(&argbuf, "), \"\")");
      }
      else if (t == TY_POLY_ARRAY) {
        buf_puts(&fmt, "%s"); buf_puts(&argbuf, "sp_PolyArray_inspect(");
        EMIT_IV(); buf_puts(&argbuf, ")");
      }
      else if (ty_is_array(t) && array_kind(t)) {
        buf_puts(&fmt, "%s"); buf_printf(&argbuf, "sp_%sArray_inspect(", array_kind(t));
        EMIT_IV(); buf_puts(&argbuf, ")");
      }
      else if (ty_is_object(t) && comp_method_in_chain(c, ty_object_class(t), "to_s", NULL) >= 0) {
        buf_puts(&fmt, "%s"); buf_printf(&argbuf, "sp_%s_to_s(", c->classes[ty_object_class(t)].name);
        EMIT_IV(); buf_puts(&argbuf, ")");
      }
      else if (ty_is_hash(t) && ty_hash_cname(t)) {
        buf_puts(&fmt, "%s"); buf_printf(&argbuf, "sp_%sHash_inspect(", ty_hash_cname(t));
        EMIT_IV(); buf_puts(&argbuf, ")");
      }
      else if (t == TY_UNKNOWN && ety && !strcmp(ety, "ArrayNode") &&
               (nt_arr(nt, expr, "elements", (int[]){0}), 1)) {
        /* a bare empty array literal interpolates as "[]" */
        int en = 0; nt_arr(nt, expr, "elements", &en);
        if (en == 0) { buf_puts(&fmt, "%s"); buf_puts(&argbuf, "\"[]\""); }
        else { free(fmt.p); free(argbuf.p); unsupported(c, pid, "interpolation value"); }
      }
      else {
        free(fmt.p); free(argbuf.p);
        unsupported(c, pid, "interpolation value");
      }
      #undef EMIT_IV
      nargs++;
    }
    else {
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
  }
  else {
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
  if (!strcmp(ty, "StringNode")) {
    const char *sc = nt_str(nt, id, "content");
    emit_str_literal_n(b, sc ? sc : "", sc ? nt_str_len(nt, id, "content") : 0);
    return;
  }
  if (!strcmp(ty, "SourceFileNode")) {
    const char *sc = nt_str(nt, id, "content");
    emit_str_literal_n(b, sc ? sc : "", sc ? strlen(sc) : 0);
    return;
  }
  if (!strcmp(ty, "SourceLineNode")) {
    buf_printf(b, "%lld", (long long)nt_int(nt, id, "start_line", 0));
    return;
  }
  if (!strcmp(ty, "RegularExpressionNode")) {
    int ri = re_lit_index(c, id);
    if (ri >= 0) buf_printf(b, "sp_re_pat_%d", ri);
    else buf_puts(b, "NULL");
    return;
  }
  if (!strcmp(ty, "InterpolatedStringNode")) { emit_interp(c, id, b); return; }
  if (!strcmp(ty, "InterpolatedSymbolNode")) {
    buf_puts(b, "sp_sym_intern("); emit_interp(c, id, b); buf_puts(b, ")");
    return;
  }
  if (!strcmp(ty, "TrueNode"))  { buf_puts(b, "1"); return; }
  if (!strcmp(ty, "FalseNode")) { buf_puts(b, "0"); return; }
  if (!strcmp(ty, "NilNode"))   { buf_puts(b, "0"); return; }  /* default in numeric/bool context */
  if (!strcmp(ty, "SymbolNode")) {
    int sid = comp_sym_intern(c, nt_str(nt, id, "value"));
    buf_printf(b, "((sp_sym)%d)", sid);
    return;
  }
  if (!strcmp(ty, "LambdaNode")) { emit_proc_literal(c, id, b); return; }
  if (!strcmp(ty, "CaseNode")) { emit_case_expr(c, id, b); return; }

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
  if (!strcmp(ty, "LocalVariableReadNode")) { emit_local_ref(c, id, nt_str(nt, id, "name"), b); return; }
  if (!strcmp(ty, "LocalVariableWriteNode")) {
    /* assignment used as expression: ({ lv = rhs; lv; }) */
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
    buf_puts(b, "({ ");
    emit_local_ref(c, id, nm, b); buf_puts(b, " = ");
    if (lv && lv->type == TY_POLY && comp_ntype(c, v) != TY_POLY) emit_boxed(c, v, b);
    else emit_expr(c, v, b);
    buf_puts(b, "; "); emit_local_ref(c, id, nm, b); buf_puts(b, "; })");
    return;
  }
  if (!strcmp(ty, "LocalVariableOperatorWriteNode")) {
    /* c += 3 used as expression: ({ lv_c += 3; lv_c; }) */
    const char *nm = nt_str(nt, id, "name");
    const char *en = rename_local(nm);
    buf_puts(b, "({ ");
    emit_op_assign(c, id, b, 0);
    buf_printf(b, "lv_%s; })", en);
    return;
  }
  if (!strcmp(ty, "InstanceVariableWriteNode")) {
    /* @ivar = rhs used as expression: ({ self->iv_x = rhs; self->iv_x; }) */
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    Scope *cws = comp_scope_of(c, id);
    int cid2 = cws ? cws->class_id : -1;
    if (cid2 < 0 && g_class_body_id >= 0) cid2 = g_class_body_id;
    if (!nm || v < 0) { buf_puts(b, "0"); return; }
    TyKind ivt2 = TY_UNKNOWN;
    if (cid2 >= 0) {
      int iv2 = comp_ivar_index(&c->classes[cid2], nm);
      if (iv2 >= 0) ivt2 = c->classes[cid2].ivar_types[iv2];
    }
    const char *vty2 = nt_type(nt, v);
    int ven2 = 0;
    int v_empty_array2 = vty2 && !strcmp(vty2, "ArrayNode") && (nt_arr(nt, v, "elements", &ven2), ven2 == 0);
    int v_empty_hash2 = 0;
    if (!v_empty_array2 && vty2) {
      int hen2 = 0;
      if (!strcmp(vty2, "HashNode") || !strcmp(vty2, "KeywordHashNode"))
        v_empty_hash2 = (nt_arr(nt, v, "elements", &hen2), hen2 == 0);
    }
    char ref2e[300];
    if (cws && cws->is_cmethod && cid2 >= 0)
      snprintf(ref2e, sizeof ref2e, "civ_%s_%s", c->classes[cid2].name, nm + 1);
    else
      snprintf(ref2e, sizeof ref2e, "%s->iv_%s", g_self, nm + 1);
    buf_puts(b, "({ ");
    buf_printf(b, "%s = ", ref2e);
    if (v_empty_array2 && ivt2 == TY_POLY_ARRAY) buf_puts(b, "sp_PolyArray_new()");
    else if (v_empty_array2 && array_kind(ivt2)) buf_printf(b, "sp_%sArray_new()", array_kind(ivt2));
    else if (v_empty_hash2 && ty_is_hash(ivt2)) {
      const char *hcn = ty_hash_cname(ivt2);
      if (hcn) buf_printf(b, "sp_%sHash_new()", hcn);
      else emit_expr(c, v, b);
    }
    else if (ivt2 == TY_POLY && comp_ntype(c, v) != TY_POLY) emit_boxed(c, v, b);
    else emit_expr(c, v, b);
    buf_printf(b, "; %s; })", ref2e);
    return;
  }
  if (!strcmp(ty, "InstanceVariableOrWriteNode") || !strcmp(ty, "InstanceVariableAndWriteNode")) {
    int is_or = !strcmp(ty, "InstanceVariableOrWriteNode");
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    Scope *cws3 = comp_scope_of(c, id);
    int cid3 = cws3 ? cws3->class_id : -1;
    if (cid3 < 0 && g_class_body_id >= 0) cid3 = g_class_body_id;
    TyKind ivt3 = TY_UNKNOWN;
    if (cid3 >= 0) { int iv3 = comp_ivar_index(&c->classes[cid3], nm); if (iv3 >= 0) ivt3 = c->classes[cid3].ivar_types[iv3]; }
    char ref3[300];
    if (cws3 && cws3->is_cmethod && cid3 >= 0)
      snprintf(ref3, sizeof ref3, "civ_%s_%s", c->classes[cid3].name, nm + 1);
    else
      snprintf(ref3, sizeof ref3, "%s->iv_%s", g_self, nm + 1);
    if (ivt3 == TY_POLY) {
      buf_printf(b, "({ if (%ssp_poly_truthy(%s)) %s = ", is_or ? "!" : "", ref3, ref3);
      emit_boxed(c, v, b);
      buf_printf(b, "; %s; })", ref3);
    }
    else if (ivt3 == TY_BOOL) {
      buf_printf(b, "({ if (%s%s) %s = ", is_or ? "!" : "", ref3, ref3);
      emit_expr(c, v, b);
      buf_printf(b, "; %s; })", ref3);
    }
    else if (!is_or) {
      buf_printf(b, "({ %s = ", ref3);
      emit_expr(c, v, b);
      buf_printf(b, "; %s; })", ref3);
    }
    else buf_puts(b, ref3);
    return;
  }
  if (!strcmp(ty, "LocalVariableOrWriteNode") || !strcmp(ty, "LocalVariableAndWriteNode")) {
    int is_or = !strcmp(ty, "LocalVariableOrWriteNode");
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
    TyKind t = lv ? lv->type : TY_UNKNOWN;
    const char *en = rename_local(nm);
    if (t == TY_POLY) {
      buf_printf(b, "({ if (%ssp_poly_truthy(lv_%s)) lv_%s = ", is_or ? "!" : "", en, en);
      emit_boxed(c, v, b);
      buf_printf(b, "; lv_%s; })", en);
    }
    else if (t == TY_BOOL) {
      buf_printf(b, "({ if (%slv_%s) lv_%s = ", is_or ? "!" : "", en, en);
      emit_expr(c, v, b);
      buf_printf(b, "; lv_%s; })", en);
    }
    else if (!is_or) {
      buf_printf(b, "({ lv_%s = ", en);
      emit_expr(c, v, b);
      buf_printf(b, "; lv_%s; })", en);
    }
    else {
      buf_printf(b, "lv_%s", en);
    }
    return;
  }
  if (!strcmp(ty, "YieldNode")) {
    if (g_block_id < 0) { buf_puts(b, "SP_INT_NIL"); return; }  /* no block: yield is nil */
    emit_block_invoke(c, nt_ref(nt, id, "arguments"), b, 0, 1);
    return;
  }
  if (is_block_call(c, id)) {           /* block.call used for its value */
    emit_block_invoke(c, nt_ref(nt, id, "arguments"), b, 0, 1);
    return;
  }
  if (!strcmp(ty, "SelfNode")) { buf_puts(b, g_self); return; }  /* self is the object reference (pointer) */
  if (!strcmp(ty, "InstanceVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");  /* "@x" */
    Scope *cs = comp_scope_of(c, id);
    if (cs && cs->is_cmethod && cs->class_id >= 0)
      buf_printf(b, "civ_%s_%s", c->classes[cs->class_id].name, nm + 1);  /* module/class-level ivar */
    else
      buf_printf(b, "%s->iv_%s", g_self, nm + 1);
    return;
  }
  if (!strcmp(ty, "ClassVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");  /* "@@x" */
    Scope *s = comp_scope_of(c, id);
    if (s->class_id >= 0) { buf_printf(b, "cvar_%s_%s", c->classes[s->class_id].name, nm + 2); return; }
    unsupported(c, id, "class variable read (no class scope)");
  }
  if (!strcmp(ty, "ClassVariableWriteNode")) {  /* in value position: yields the assigned value */
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    Scope *s = comp_scope_of(c, id);
    if (s->class_id < 0) unsupported(c, id, "class variable write (no class scope)");
    TyKind ct = TY_INT;
    int idx = comp_cvar_index(&c->classes[s->class_id], nm);
    if (idx >= 0) ct = c->classes[s->class_id].cvar_types[idx];
    buf_printf(b, "(cvar_%s_%s = ", c->classes[s->class_id].name, nm + 2);
    if (ct == TY_POLY) emit_boxed(c, v, b); else emit_expr(c, v, b);
    buf_puts(b, ")");
    return;
  }
  if (!strcmp(ty, "ClassVariableOperatorWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    const char *op = nt_str(nt, id, "binary_operator");
    int v = nt_ref(nt, id, "value");
    Scope *s = comp_scope_of(c, id);
    if (s->class_id < 0) { unsupported(c, id, "class variable op-write (no class scope)"); return; }
    TyKind ct = TY_INT;
    int idx = comp_cvar_index(&c->classes[s->class_id], nm);
    if (idx >= 0) ct = c->classes[s->class_id].cvar_types[idx];
    char ref[300]; snprintf(ref, sizeof ref, "cvar_%s_%s", c->classes[s->class_id].name, nm + 2);
    if (ct == TY_STRING && op && !strcmp(op, "+")) {
      buf_printf(b, "(%s = sp_str_concat(%s, ", ref, ref);
      emit_expr(c, v, b); buf_puts(b, "))");
    }
    else {
      buf_printf(b, "(%s %s= ", ref, op ? op : "+");
      emit_expr(c, v, b); buf_puts(b, ")");
    }
    return;
  }
  if (!strcmp(ty, "ClassVariableOrWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    Scope *s = comp_scope_of(c, id);
    if (s->class_id < 0) { unsupported(c, id, "class variable or-write (no class scope)"); return; }
    char ref[300]; snprintf(ref, sizeof ref, "cvar_%s_%s", c->classes[s->class_id].name, nm + 2);
    buf_printf(b, "(%s ? %s : (%s = ", ref, ref, ref);
    emit_expr(c, v, b); buf_puts(b, "))");
    return;
  }
  if (!strcmp(ty, "ClassVariableAndWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    Scope *s = comp_scope_of(c, id);
    if (s->class_id < 0) { unsupported(c, id, "class variable and-write (no class scope)"); return; }
    char ref[300]; snprintf(ref, sizeof ref, "cvar_%s_%s", c->classes[s->class_id].name, nm + 2);
    buf_printf(b, "(%s ? (%s = ", ref, ref);
    emit_expr(c, v, b); buf_puts(b, ") : 0)");
    return;
  }
  if (!strcmp(ty, "GlobalVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    /* predefined punctuation globals: $/ is the record separator "\n"; $! / $; /
       $, read nil (spinel doesn't honor the split/print-sep defaults) */
    if (nm && !strcmp(nm, "$/")) { emit_str_literal(b, "\n"); return; }
    if (nm && !strcmp(nm, "$?")) { buf_puts(b, "sp_last_status"); return; }
    if (nm && (!strcmp(nm, "$!") || !strcmp(nm, "$;") || !strcmp(nm, "$,"))) { buf_puts(b, "0"); return; }
    if (nm && nm[0] == '$') {
      const char *rn = comp_resolve_gvar(c, nm + 1);
      if (comp_gvar(c, rn)) { buf_printf(b, "gv_%s", rn); return; }
    }
    unsupported(c, id, "global variable read");
  }
  if (!strcmp(ty, "NumberedReferenceReadNode")) {
    /* $1..$9 -> the n-th capture of the last match (NULL when absent) */
    long long n = nt_int(nt, id, "number", 0);
    if (n >= 1 && n <= 9) buf_printf(b, "sp_re_captures[%lld]", n);
    else buf_puts(b, "NULL");
    return;
  }
  if (!strcmp(ty, "BackReferenceReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm) { buf_puts(b, "NULL"); return; }
    if (!strcmp(nm, "$&") || !strcmp(nm, "$~")) buf_puts(b, "sp_re_match_str");
    else if (!strcmp(nm, "$`"))                 buf_puts(b, "sp_re_match_pre");
    else if (!strcmp(nm, "$'"))                 buf_puts(b, "sp_re_match_post");
    else if (!strcmp(nm, "$+")) {
      /* last group that participated: scan captures[] backwards */
      buf_puts(b, "({ int _bri = 9; while (_bri > 0 && !sp_re_captures[_bri-1]) _bri--; _bri > 0 ? sp_re_captures[_bri-1] : NULL; })");
    }
    else buf_puts(b, "NULL");
    return;
  }
  if (!strcmp(ty, "ConstantReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    LocalVar *cv = nm ? comp_const(c, nm) : NULL;
    if (cv) {
      if (cv->init_guarded) {
        /* a read during the const's own Class.new init raises NameError */
        buf_printf(b, "(sp_init_in_progress_%s ? (sp_raise_cls(\"NameError\","
                      " \"uninitialized constant %s\"), cst_%s) : cst_%s)", nm, nm, nm, nm);
      }
      else buf_printf(b, "cst_%s", nm);
      return;
    }
    if (nm && !strcmp(nm, "RUBY_DESCRIPTION")) { buf_puts(b, "SPL(\"spinel\")"); return; }
    if (nm && !strcmp(nm, "ARGV")) { buf_puts(b, "sp_get_ARGV()"); return; }
    unsupported(c, id, "constant read");
  }
  if (!strcmp(ty, "ConstantPathNode")) {
    /* M::CONST -> the flat constant named by the final path component */
    const char *nm = nt_str(nt, id, "name");
    if (nm && comp_const(c, nm)) { buf_printf(b, "cst_%s", nm); return; }
    if (nm && !strcmp(nm, "ARGV")) { buf_puts(b, "sp_get_ARGV()"); return; }
    /* well-known module constants */
    int par_idc = nt_ref(nt, id, "parent");
    const char *par_tyc = par_idc >= 0 ? nt_type(nt, par_idc) : NULL;
    const char *par_nmc = (par_tyc && !strcmp(par_tyc, "ConstantReadNode")) ? nt_str(nt, par_idc, "name") : NULL;
    if (par_nmc && !strcmp(par_nmc, "Float") && nm) {
      if (!strcmp(nm, "MAX"))      { buf_puts(b, "DBL_MAX"); return; }
      if (!strcmp(nm, "MIN"))      { buf_puts(b, "DBL_MIN"); return; }
      if (!strcmp(nm, "EPSILON"))  { buf_puts(b, "DBL_EPSILON"); return; }
      if (!strcmp(nm, "INFINITY")) { buf_puts(b, "(1.0/0.0)"); return; }
      if (!strcmp(nm, "NAN"))      { buf_puts(b, "(0.0/0.0)"); return; }
      if (!strcmp(nm, "DIG"))      { buf_printf(b, "(double)DBL_DIG"); return; }
      if (!strcmp(nm, "MANT_DIG")) { buf_printf(b, "(double)DBL_MANT_DIG"); return; }
      if (!strcmp(nm, "RADIX"))    { buf_printf(b, "(double)FLT_RADIX"); return; }
    }
    if (par_nmc && !strcmp(par_nmc, "Math") && nm) {
      if (!strcmp(nm, "PI")) { buf_puts(b, "M_PI"); return; }
      if (!strcmp(nm, "E"))  { buf_puts(b, "M_E"); return; }
    }
    if (par_nmc && !strcmp(par_nmc, "File") && nm) {
      if (!strcmp(nm, "SEPARATOR"))      { buf_puts(b, "\"/\""); return; }
      if (!strcmp(nm, "PATH_SEPARATOR")) { buf_puts(b, "\":\""); return; }
      if (!strcmp(nm, "ALT_SEPARATOR"))  { buf_puts(b, "(&(\"\\xff\")[1])"); return; }
    }
    if (par_nmc && !strcmp(par_nmc, "Integer") && nm &&
        (!strcmp(nm, "MAX") || !strcmp(nm, "MIN"))) {
      /* Integer::MAX/MIN do not exist in Ruby — raise NameError at runtime */
      buf_printf(b, "(sp_raise_cls(\"NameError\", \"uninitialized constant Integer::%s\"), 0)", nm);
      return;
    }
    unsupported(c, id, "constant path read");
  }
  if (!strcmp(ty, "DefinedNode")) {
    /* compile-time defined? -> a label string, or nil (NULL) when undefined */
    int v = nt_ref(nt, id, "value");
    const char *vt = v >= 0 ? nt_type(nt, v) : NULL;
    const char *res = NULL;
    if (vt) {
      if (!strcmp(vt, "LocalVariableReadNode")) res = "local-variable";
      else if (!strcmp(vt, "InstanceVariableReadNode")) {
        /* Return "instance-variable" only when the ivar is known to be assigned. */
        const char *inm = nt_str(nt, v, "name");
        for (int kk = 0; kk < nt->count && !res; kk++) {
          const char *kt = nt_type(nt, kk);
          if (kt && !strcmp(kt, "InstanceVariableWriteNode") &&
              inm && nt_str(nt, kk, "name") && !strcmp(nt_str(nt, kk, "name"), inm))
            res = "instance-variable";
        }
      }
      else if (!strcmp(vt, "ClassVariableReadNode")) res = "class variable";
      else if (!strcmp(vt, "SelfNode")) res = "self";
      else if (!strcmp(vt, "NilNode")) res = "nil";
      else if (!strcmp(vt, "TrueNode")) res = "true";
      else if (!strcmp(vt, "FalseNode")) res = "false";
      else if (!strcmp(vt, "IntegerNode") || !strcmp(vt, "FloatNode") ||
               !strcmp(vt, "StringNode") || !strcmp(vt, "SymbolNode") || !strcmp(vt, "ArrayNode")) res = "expression";
      else if (!strcmp(vt, "GlobalVariableReadNode")) {
        const char *gn = nt_str(nt, v, "name");
        for (int kk = 0; kk < nt->count && !res; kk++) {
          const char *kt = nt_type(nt, kk);
          if (kt && (!strcmp(kt, "GlobalVariableWriteNode") || !strcmp(kt, "GlobalVariableOperatorWriteNode")) &&
              gn && nt_str(nt, kk, "name") && !strcmp(nt_str(nt, kk, "name"), gn))
            res = "global-variable";
        }
      }
      else if (!strcmp(vt, "ConstantReadNode")) {
        const char *cn = nt_str(nt, v, "name");
        static const char *const builtins[] = {
          "Object", "BasicObject", "Kernel", "Module", "Class", "Array", "Hash",
          "String", "Integer", "Float", "Symbol", "Regexp", "Range", "NilClass",
          "TrueClass", "FalseClass", "Numeric", "Comparable", "Enumerable",
          "IO", "File", "Dir", "Math", "GC", "Process", "ENV", "ARGV",
          "STDOUT", "STDERR", "STDIN", NULL
        };
        if (cn) {
          if (comp_const(c, cn) || comp_class_index(c, cn) >= 0) res = "constant";
          if (!res) {
            for (int bi = 0; builtins[bi]; bi++)
              if (!strcmp(cn, builtins[bi])) { res = "constant"; break; }
          }
        }
      }
      else if (!strcmp(vt, "CallNode") && nt_ref(nt, v, "receiver") < 0) {
        const char *cn = nt_str(nt, v, "name");
        if (cn && comp_method_index(c, cn) >= 0) res = "method";
      }
    }
    if (res) buf_printf(b, "SPL(\"%s\")", res);
    else buf_puts(b, "NULL");
    return;
  }
  if (!strcmp(ty, "ParenthesesNode")) {
    int body = nt_ref(nt, id, "body");
    int n = 0;
    const int *bd = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    if (n == 0) { buf_puts(b, "sp_box_nil()"); return; }
    if (n == 1) {
      buf_puts(b, "("); emit_expr(c, bd[0], b); buf_puts(b, ")");
      return;
    }
    /* Multi-stmt parens: `(s1; s2; expr)` — run leading stmts in prelude,
       return value of last expression via GNU statement expression. */
    buf_puts(b, "({ ");
    for (int j = 0; j < n - 1; j++) {
      emit_stmt(c, bd[j], b, 0);
    }
    emit_expr(c, bd[n - 1], b);
    buf_puts(b, "; })");
    return;
  }
  if (!strcmp(ty, "ArrayNode")) {
    int n = 0;
    const int *els = nt_arr(nt, id, "elements", &n);
    TyKind at = comp_ntype(c, id);
    /* an empty `[]` literal carries no element type of its own; it is
       emitted via the target's type in emit_assign. If we reach here for
       an empty literal, use g_ret_type context (e.g. tail position in a
       poly_array-returning method) before falling back to int array. */
    if (n == 0 && at == TY_UNKNOWN && ty_is_array(g_ret_type)) at = g_ret_type;
    const char *k = array_kind(at);
    if (n == 0 && !k && at != TY_POLY_ARRAY) { buf_puts(b, "sp_IntArray_new()"); return; }
    /* poly (mixed-element) array: build an sp_PolyArray of boxed elements */
    if (at == TY_POLY_ARRAY) {
      int t = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new();\n", t);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
      for (int j = 0; j < n; j++) {
        const char *ety = nt_type(nt, els[j]);
        if (ety && !strcmp(ety, "SplatNode")) {
          /* [*arr] or [*range] — expand into poly */
          int inner = nt_ref(nt, els[j], "expression");
          TyKind it = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
          Buf el; memset(&el, 0, sizeof el); emit_expr(c, inner, &el);
          const char *ep = el.p ? el.p : "NULL";
          emit_indent(g_pre, g_indent);
          if (it == TY_RANGE)
            buf_printf(g_pre, "{ sp_Range _sr = %s; mrb_int _e = _sr.last+(_sr.excl?0:1); for (mrb_int _si = _sr.first; _si < _e; _si++) sp_PolyArray_push(_t%d, sp_box_int(_si)); }\n", ep, t);
          else if (it == TY_INT_ARRAY)
            buf_printf(g_pre, "{ sp_IntArray *_sa = %s; if (_sa) for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_int(_sa->data[_sa->start+_si])); }\n", ep, t);
          else if (it == TY_STR_ARRAY)
            buf_printf(g_pre, "{ sp_StrArray *_sa = %s; if (_sa) for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_str(_sa->data[_si])); }\n", ep, t);
          else if (it == TY_FLOAT_ARRAY)
            buf_printf(g_pre, "{ sp_FloatArray *_sa = %s; if (_sa) for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_float(_sa->data[_si])); }\n", ep, t);
          else if (it == TY_POLY_ARRAY)
            buf_printf(g_pre, "{ sp_PolyArray *_sa = %s; if (_sa) for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, _sa->data[_si]); }\n", ep, t);
          else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed(c, inner, &bx); buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s);\n", t, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
          free(el.p);
        } else {
          Buf el; memset(&el, 0, sizeof el);
          emit_boxed(c, els[j], &el);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", t);
          buf_puts(g_pre, el.p ? el.p : "");
          buf_puts(g_pre, ");\n");
          free(el.p);
        }
      }
      buf_printf(b, "_t%d", t);
      return;
    }
    if (!k) unsupported(c, id, "array literal (element type)");
    int t = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", k, t, k);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
    for (int j = 0; j < n; j++) {
      const char *ety = nt_type(nt, els[j]);
      if (ety && !strcmp(ety, "SplatNode")) {
        /* [*range] or [*arr] inside a typed array literal */
        int inner = nt_ref(nt, els[j], "expression");
        TyKind it = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
        Buf el; memset(&el, 0, sizeof el); emit_expr(c, inner, &el);
        const char *ep = el.p ? el.p : "NULL";
        emit_indent(g_pre, g_indent);
        if (it == TY_RANGE)
          buf_printf(g_pre, "{ sp_Range _sr = %s; mrb_int _e = _sr.last+(_sr.excl?0:1); for (mrb_int _si = _sr.first; _si < _e; _si++) sp_%sArray_push(_t%d, _si); }\n", ep, k, t);
        else if (it == TY_INT_ARRAY && !strcmp(k, "Int"))
          buf_printf(g_pre, "{ sp_IntArray *_sa = %s; if (_sa) for (mrb_int _si = 0; _si < _sa->len; _si++) sp_%sArray_push(_t%d, _sa->data[_sa->start+_si]); }\n", ep, k, t);
        else if (it == TY_STR_ARRAY && !strcmp(k, "Str"))
          buf_printf(g_pre, "{ sp_StrArray *_sa = %s; if (_sa) for (mrb_int _si = 0; _si < _sa->len; _si++) sp_%sArray_push(_t%d, _sa->data[_si]); }\n", ep, k, t);
        else {
          /* Mismatched or unknown element type: emit_expr fallback */
          buf_printf(g_pre, "sp_%sArray_push(_t%d, %s);\n", k, t, ep);
        }
        free(el.p);
      } else {
        Buf el; memset(&el, 0, sizeof el);
        emit_expr(c, els[j], &el);   /* element preludes flow to g_pre first */
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_%sArray_push(_t%d, ", k, t);
        buf_puts(g_pre, el.p ? el.p : "");
        buf_puts(g_pre, ");\n");
        free(el.p);
      }
    }
    buf_printf(b, "_t%d", t);
    return;
  }
  if (!strcmp(ty, "HashNode") || !strcmp(ty, "KeywordHashNode")) {
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
    int sym_poly = (ht == TY_SYM_POLY_HASH || ht == TY_STR_POLY_HASH);
    int poly_poly = (ht == TY_POLY_POLY_HASH);
    for (int j = 0; j < n; j++) {
      int key = nt_ref(nt, els[j], "key");
      int val = nt_ref(nt, els[j], "value");
      Buf kb; memset(&kb, 0, sizeof kb);
      if (poly_poly) emit_boxed(c, key, &kb); else emit_expr(c, key, &kb);
      Buf vb; memset(&vb, 0, sizeof vb);
      if (sym_poly || poly_poly) emit_boxed(c, val, &vb); else emit_expr(c, val, &vb);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_%sHash_set(_t%d, ", hn, t);
      buf_puts(g_pre, kb.p ? kb.p : ""); buf_puts(g_pre, ", ");
      buf_puts(g_pre, vb.p ? vb.p : ""); buf_puts(g_pre, ");\n");
      free(kb.p); free(vb.p);
    }
    buf_printf(b, "_t%d", t);
    return;
  }
  if (!strcmp(ty, "IfNode") || !strcmp(ty, "UnlessNode")) {
    /* if/unless as a value: a ternary when both branches are single
       value-expressions */
    int pred = nt_ref(nt, id, "predicate");
    int then_b = nt_ref(nt, id, "statements");
    int is_unless = !strcmp(ty, "UnlessNode");
    int sub = nt_ref(nt, id, is_unless ? "else_clause" : "subsequent");
    int tn = 0;
    const int *tb = then_b >= 0 ? nt_arr(nt, then_b, "body", &tn) : NULL;
    int else_stmts = -1;
    if (sub >= 0 && nt_type(nt, sub) && !strcmp(nt_type(nt, sub), "ElseNode"))
      else_stmts = nt_ref(nt, sub, "statements");
    int en = 0;
    const int *eb = else_stmts >= 0 ? nt_arr(nt, else_stmts, "body", &en) : NULL;
    if (tn == 1 && en == 1) {
      TyKind res = comp_ntype(c, id);
      buf_puts(b, "(");
      if (is_unless) buf_puts(b, "!(");
      emit_cond(c, pred, b);
      if (is_unless) buf_puts(b, ")");
      buf_puts(b, " ? ");
      /* a poly result with concrete-typed branches boxes each branch;
         for typed-array results, an empty [] literal uses the result type */
      { int nd = tb[0]; const char *_bty = nt_type(nt, nd);
        if (res == TY_POLY && comp_ntype(c, nd) != TY_POLY) emit_boxed(c, nd, b);
        else if (ty_is_array(res) && _bty && !strcmp(_bty, "ArrayNode")) {
          int _bn = 0; nt_arr(nt, nd, "elements", &_bn);
          if (_bn == 0) { const char *_rk = (res == TY_POLY_ARRAY) ? "Poly" : array_kind(res);
            buf_printf(b, "sp_%sArray_new()", _rk ? _rk : "Int"); }
          else emit_expr(c, nd, b);
        } else emit_expr(c, nd, b); }
      buf_puts(b, " : ");
      { int nd = eb[0]; const char *_bty = nt_type(nt, nd);
        if (res == TY_POLY && comp_ntype(c, nd) != TY_POLY) emit_boxed(c, nd, b);
        else if (ty_is_array(res) && _bty && !strcmp(_bty, "ArrayNode")) {
          int _bn = 0; nt_arr(nt, nd, "elements", &_bn);
          if (_bn == 0) { const char *_rk = (res == TY_POLY_ARRAY) ? "Poly" : array_kind(res);
            buf_printf(b, "sp_%sArray_new()", _rk ? _rk : "Int"); }
          else emit_expr(c, nd, b);
        } else emit_expr(c, nd, b); }
      buf_puts(b, ")");
      return;
    }
    /* Multi-stmt branches or no-else: emit as if/else block with a result temp.
       Preludes and the if structure go into g_pre; `b` receives only _t<N>. */
    {
      TyKind res = comp_ntype(c, id);
      int tr = ++g_tmp;
      /* Declare the temp and default-initialize it. */
      emit_indent(g_pre, g_indent);
      emit_ctype(c, res, g_pre);
      buf_printf(g_pre, " _t%d = %s;\n", tr,
                 res == TY_RANGE ? "(sp_Range){0}" : default_value(res));
      /* Emit condition — any prolog from the condition expr also goes to g_pre. */
      emit_indent(g_pre, g_indent);
      buf_puts(g_pre, "if (");
      if (is_unless) buf_puts(g_pre, "!(");
      emit_cond(c, pred, g_pre);
      if (is_unless) buf_puts(g_pre, ")");
      buf_puts(g_pre, ") {\n");
      /* Then branch: side-effect stmts, then assign last expr to temp. */
      for (int i = 0; i < tn - 1; i++) emit_stmt(c, tb[i], g_pre, g_indent + 2);
      if (tn > 0) {
        int last_then = tb[tn - 1];
        TyKind lt = comp_ntype(c, last_then);
        if (lt == TY_NIL || lt == TY_UNKNOWN) {
          emit_stmt(c, last_then, g_pre, g_indent + 2);
        }
        else {
          int saved_gi = g_indent; g_indent = g_indent + 2;
          Buf le; memset(&le, 0, sizeof le);
          emit_expr(c, last_then, &le);
          g_indent = saved_gi;
          emit_indent(g_pre, g_indent + 2);
          buf_printf(g_pre, "_t%d = ", tr);
          if (res == TY_POLY && lt != TY_POLY) {
            Buf bx; memset(&bx, 0, sizeof bx);
            emit_boxed_text(c, lt, le.p ? le.p : default_value(lt), &bx);
            buf_puts(g_pre, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
          }
          else buf_puts(g_pre, le.p ? le.p : default_value(res));
          buf_puts(g_pre, ";\n");
          free(le.p);
        }
      }
      emit_indent(g_pre, g_indent);
      buf_puts(g_pre, "}\n");
      /* Else / elsif branch. */
      if (sub >= 0) {
        const char *sub_ty = nt_type(nt, sub);
        if (sub_ty && !strcmp(sub_ty, "ElseNode")) {
          emit_indent(g_pre, g_indent);
          buf_puts(g_pre, "else {\n");
          for (int i = 0; i < en - 1; i++) emit_stmt(c, eb[i], g_pre, g_indent + 2);
          if (en > 0) {
            int last_else = eb[en - 1];
            TyKind lt2 = comp_ntype(c, last_else);
            if (lt2 == TY_NIL || lt2 == TY_UNKNOWN) {
              emit_stmt(c, last_else, g_pre, g_indent + 2);
            }
            else {
              int saved_gi2 = g_indent; g_indent = g_indent + 2;
              Buf le2; memset(&le2, 0, sizeof le2);
              emit_expr(c, last_else, &le2);
              g_indent = saved_gi2;
              emit_indent(g_pre, g_indent + 2);
              buf_printf(g_pre, "_t%d = ", tr);
              if (res == TY_POLY && lt2 != TY_POLY) {
                Buf bx2; memset(&bx2, 0, sizeof bx2);
                emit_boxed_text(c, lt2, le2.p ? le2.p : default_value(lt2), &bx2);
                buf_puts(g_pre, bx2.p ? bx2.p : "sp_box_nil()"); free(bx2.p);
              }
              else buf_puts(g_pre, le2.p ? le2.p : default_value(res));
              buf_puts(g_pre, ";\n");
              free(le2.p);
            }
          }
          emit_indent(g_pre, g_indent);
          buf_puts(g_pre, "}\n");
        }
        else {
          /* elsif (sub is IfNode) or other subsequent: recurse via emit_expr */
          emit_indent(g_pre, g_indent);
          buf_puts(g_pre, "else {\n");
          int saved_gi3 = g_indent; g_indent = g_indent + 2;
          Buf sub_e; memset(&sub_e, 0, sizeof sub_e);
          emit_expr(c, sub, &sub_e);
          g_indent = saved_gi3;
          emit_indent(g_pre, g_indent + 2);
          buf_printf(g_pre, "_t%d = %s;\n", tr, sub_e.p ? sub_e.p : default_value(res));
          free(sub_e.p);
          emit_indent(g_pre, g_indent);
          buf_puts(g_pre, "}\n");
        }
      }
      buf_printf(b, "_t%d", tr);
      return;
    }
  }
  if (!strcmp(ty, "CallNode")) { emit_call(c, id, b); return; }
  if (!strcmp(ty, "SuperNode") || !strcmp(ty, "ForwardingSuperNode")) {
    if (!emit_super_inline(c, id, b, 0, 1)) emit_super(c, id, b);
    return;
  }
  if (!strcmp(ty, "AndNode") || !strcmp(ty, "OrNode")) {
    int is_and = !strcmp(ty, "AndNode");
    int left = nt_ref(nt, id, "left"), right = nt_ref(nt, id, "right");
    TyKind lt = comp_ntype(c, left), res = comp_ntype(c, id);
    if (lt == TY_BOOL && comp_ntype(c, right) == TY_BOOL) {
      buf_puts(b, "(");
      emit_expr(c, left, b);
      buf_puts(b, is_and ? " && " : " || ");
      emit_expr(c, right, b);
      buf_puts(b, ")");
      return;
    }
    /* value form: a || b  ->  truthy(a) ? a : b ;  a && b -> truthy(a) ? b : a.
       Evaluate the left once into a temp; results widen to the unified type. */
    int t = ++g_tmp;
    int lt_falsy_const = (lt == TY_NIL || lt == TY_VOID);  /* a nil/void left has no C-typed value */
    buf_puts(b, "({ ");
    emit_ctype(c, (lt == TY_UNKNOWN || lt_falsy_const) ? res : lt, b);
    buf_printf(b, " _t%d = ", t);
    if (lt_falsy_const) {
      buf_puts(b, "("); emit_expr(c, left, b); buf_puts(b, ", ");
      buf_puts(b, res == TY_POLY ? "sp_box_nil()" : default_value(res == TY_UNKNOWN ? TY_INT : res));
      buf_puts(b, ")");
    }
    else emit_expr(c, left, b);
    buf_puts(b, "; ");
    if (lt == TY_POLY)      buf_printf(b, "sp_poly_truthy(_t%d)", t);
    else if (lt == TY_BOOL) buf_printf(b, "_t%d", t);
    else if (lt_falsy_const) buf_puts(b, "0");
    else if (lt == TY_INT)  buf_printf(b, "(_t%d != SP_INT_NIL)", t);  /* a nullable int reads falsy at the sentinel; a plain int is always truthy */
    else if (lt == TY_FLOAT) buf_printf(b, "(!sp_float_is_nil(_t%d))", t);
    else if (lt == TY_STRING || ty_is_array(lt) || ty_is_hash(lt) || ty_is_object(lt) ||
             lt == TY_PROC || lt == TY_STRINGIO || lt == TY_STRINGSCANNER || lt == TY_MATCHDATA || lt == TY_EXCEPTION)
      buf_printf(b, "(_t%d != 0)", t);  /* nullable pointer: NULL reads falsy */
    else                    buf_puts(b, "1");  /* concrete value: always truthy */
    buf_puts(b, " ? ");
    /* the "kept-left" arm and the "right" arm, each widened to res */
    #define EMIT_ARM(IS_RIGHT) do { \
      if (IS_RIGHT) { if (res == TY_POLY && comp_ntype(c, right) != TY_POLY) emit_boxed(c, right, b); else emit_expr(c, right, b); } \
      else { if (res == TY_POLY && lt != TY_POLY) { /* box temp */ \
               Buf _vb; memset(&_vb,0,sizeof _vb); buf_printf(&_vb, "_t%d", t); \
               /* reuse emit_boxed by faking: just box by left type */ \
               if (lt==TY_INT) buf_printf(b, "sp_box_int(_t%d)", t); \
               else if (lt==TY_STRING) buf_printf(b, "sp_box_str(_t%d)", t); \
               else if (lt==TY_FLOAT) buf_printf(b, "sp_box_float(_t%d)", t); \
               else if (lt==TY_BOOL) buf_printf(b, "sp_box_bool(_t%d)", t); \
               else if (lt==TY_SYMBOL) buf_printf(b, "sp_box_sym(_t%d)", t); \
               else buf_printf(b, "_t%d", t); free(_vb.p); } \
             else buf_printf(b, "_t%d", t); } \
    } while (0)
    if (is_and) { EMIT_ARM(1); buf_puts(b, " : "); EMIT_ARM(0); }
    else        { EMIT_ARM(0); buf_puts(b, " : "); EMIT_ARM(1); }
    #undef EMIT_ARM
    buf_printf(b, "; })");
    return;
  }

  if (!strcmp(ty, "RescueModifierNode")) {
    /* `expr rescue fallback` as an rvalue: evaluate expr under setjmp;
       on exception, evaluate fallback instead. */
    int e  = nt_ref(nt, id, "expression");
    int r  = nt_ref(nt, id, "rescue_expression");
    TyKind rt = comp_ntype(c, id);
    int t = ++g_tmp;
    buf_puts(b, "({ ");
    emit_ctype(c, rt, b);
    buf_printf(b, " _t%d = %s; sp_exc_top++;\n", t, default_value(rt));
    buf_puts(b, "if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {\n");
    /* expression arm — assign result to temp (skip diverging exprs like raise) */
    TyKind et = e >= 0 ? comp_ntype(c, e) : TY_UNKNOWN;
    int e_diverges = (et == TY_UNKNOWN || et == TY_VOID);
    buf_puts(b, "  ");
    if (e >= 0 && !e_diverges) {
      buf_printf(b, "_t%d = ", t);
      if (rt == TY_POLY && et != TY_POLY) emit_boxed(c, e, b);
      else emit_expr(c, e, b);
      buf_puts(b, ";");
    }
    else if (e >= 0) {
      /* diverging expression like raise: emit as stmt (no assignment) */
      emit_expr(c, e, b); buf_puts(b, ";");
    }
    buf_puts(b, " sp_exc_top--;\n}\nelse {\n  sp_exc_top--;\n  ");
    /* rescue arm */
    if (r >= 0) {
      buf_printf(b, "_t%d = ", t);
      if (rt == TY_POLY && comp_ntype(c, r) != TY_POLY) emit_boxed(c, r, b);
      else emit_expr(c, r, b);
      buf_puts(b, ";");
    }
    buf_printf(b, "\n}\n_t%d; })", t);
    return;
  }

  if (!strcmp(ty, "BeginNode")) {
    /* begin/rescue as an rvalue: hoist the block into g_pre so the temp
       is assigned before the surrounding expression reads it. */
    TyKind rt = comp_ntype(c, id);
    int t = ++g_tmp;
    char rv[32]; snprintf(rv, sizeof rv, "_t%d", t);
    int sp = g_result_poly; g_result_poly = (rt == TY_POLY);
    if (g_pre) {
      emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
      buf_printf(g_pre, " _t%d = %s;\n", t, default_value(rt));
      emit_begin(c, id, g_pre, g_indent, rv);
    }
    else {
      /* No prelude available (e.g. inside another expression's prelude):
         fall back to a GCC statement expression. */
      buf_puts(b, "({ ");
      emit_ctype(c, rt, b); buf_printf(b, " _t%d = %s;\n", t, default_value(rt));
      emit_begin(c, id, b, 0, rv);
      buf_printf(b, "_t%d; })", t);
      g_result_poly = sp;
      return;
    }
    g_result_poly = sp;
    buf_printf(b, "_t%d", t);
    return;
  }

  /* MultiWriteNode as expression: execute the destructuring (side effect),
     then return the RHS value (Ruby semantics: value of `a, b = arr` is arr). */
  if (!strcmp(ty, "MultiWriteNode")) {
    int value = nt_ref(nt, id, "value");
    /* emit the multi-write as a statement first (for its side effects) */
    emit_stmt(c, id, g_pre, g_indent);
    /* then yield the RHS value as the expression's result */
    emit_expr(c, value, b);
    return;
  }

  unsupported(c, id, "expression");
}

/* ---- output statements (puts/print/p) ---- */

static void emit_puts_one(Compiler *c, int arg, Buf *b, int indent) {
  arg = unwrap_parens(c, arg);
  TyKind t = comp_ntype(c, arg);
  emit_indent(b, indent);
  if (t == TY_INT) {
    /* a nullable int at the sentinel prints as nil (an empty line) */
    int tv = ++g_tmp;
    buf_printf(b, "{ mrb_int _t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; if (_t%d == SP_INT_NIL) putchar('\\n'); else printf(\"%%lld\\n\", (long long)_t%d); }\n", tv, tv);
  }
  else if (t == TY_FLOAT) {
    buf_puts(b, "{ const char *_fs = sp_float_opt_to_s("); emit_expr(c, arg, b);
    buf_puts(b, "); fputs(_fs, stdout); putchar('\\n'); }\n");
  }
  else if (t == TY_STRING) {
    buf_puts(b, "{ const char *_ps = (const char *)("); emit_expr(c, arg, b);
    buf_puts(b, "); if (_ps) fputs(_ps, stdout); if (!_ps || !*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); }\n");
  }
  else if (t == TY_BOOL) {
    buf_puts(b, "puts(("); emit_expr(c, arg, b); buf_puts(b, ") ? \"true\" : \"false\");\n");
  }
  else if (t == TY_SYMBOL) {
    buf_puts(b, "puts(sp_sym_to_s("); emit_expr(c, arg, b); buf_puts(b, "));\n");
  }
  else if (ty_is_array(t) && array_kind(t)) {
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
      buf_printf(b, "{ const char *_ps = sp_StrArray_get(%s, _t%d); if (_ps) fputs(_ps, stdout); if (!_ps || !*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); }\n", a, ti);
    free(ab.p);
  }
  else if (t == TY_EXCEPTION) {
    buf_puts(b, "{ const char *_ps = sp_exc_message("); emit_expr(c, arg, b);
    buf_puts(b, "); if (_ps) fputs(_ps, stdout); if (!_ps || !*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); }\n");
  }
  else if (t == TY_POLY) {
    buf_puts(b, "sp_poly_puts("); emit_expr(c, arg, b); buf_puts(b, ");\n");
  }
  else if (t == TY_POLY_ARRAY) {
    int ta = ++g_tmp, ti = ++g_tmp;
    Buf ab; memset(&ab, 0, sizeof ab); emit_expr(c, arg, &ab);
    buf_printf(b, "{ sp_PolyArray *_t%d = %s;\n", ta, ab.p ? ab.p : "");
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) sp_poly_puts(sp_PolyArray_get(_t%d, _t%d)); }\n",
               ti, ti, ta, ti, ta, ti);
    free(ab.p);
  }
  else if (ty_is_object(t) && comp_method_in_class(c, ty_object_class(t), "to_s") >= 0) {
    int cid = ty_object_class(t);
    buf_puts(b, "{ const char *_ps = (const char *)(");
    buf_printf(b, "sp_%s_to_s(", c->classes[cid].name);
    const char *rty = nt_type(c->nt, arg);
    if (rty && (!strcmp(rty, "LocalVariableReadNode") || !strcmp(rty, "InstanceVariableReadNode") || !strcmp(rty, "SelfNode"))) {
      emit_expr(c, arg, b);
    }
    else {
      int tt = ++g_tmp;
      emit_indent(g_pre, g_indent); emit_ctype(c, t, g_pre);
      buf_printf(g_pre, " _t%d = ", tt);
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, arg, &rb);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      buf_printf(b, "_t%d", tt);
    }
    buf_puts(b, ")); if (_ps) fputs(_ps, stdout); if (!_ps || !*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); }\n");
  }
  else if (nt_type(c->nt, arg) && !strcmp(nt_type(c->nt, arg), "ArrayNode") &&
           ({ int _n = 0; nt_arr(c->nt, arg, "elements", &_n); _n == 0; })) {
    buf_puts(b, "(void)0;  /* puts [] prints nothing */\n");
  }
  else if (t == TY_NIL) {
    buf_puts(b, "(void)("); emit_expr(c, arg, b); buf_puts(b, "); putchar('\\n');  /* puts nil */\n");
  }
  else if (nt_type(c->nt, arg) && !strcmp(nt_type(c->nt, arg), "ConstantReadNode") &&
           nt_str(c->nt, arg, "name") && comp_class_index(c, nt_str(c->nt, arg, "name")) >= 0) {
    /* `puts SomeClass` -- a bare class constant renders its name */
    buf_printf(b, "puts(\"%s\");\n", nt_str(c->nt, arg, "name"));
  }
  else {
    unsupported(c, arg, "puts argument");
  }
}
static void emit_print_one(Compiler *c, int arg, Buf *b, int indent) {
  TyKind t = comp_ntype(c, arg);
  emit_indent(b, indent);
  if (t == TY_INT) {
    buf_puts(b, "printf(\"%lld\", (long long)"); emit_expr(c, arg, b); buf_puts(b, ");\n");
  }
  else if (t == TY_FLOAT) {
    buf_puts(b, "fputs(sp_float_to_s("); emit_expr(c, arg, b); buf_puts(b, "), stdout);\n");
  }
  else if (t == TY_STRING) {
    buf_puts(b, "{ const char *_s = ("); emit_expr(c, arg, b);
    buf_puts(b, "); if (_s) fputs(_s, stdout); }\n");
  }
  else if (t == TY_BOOL) {
    buf_puts(b, "fputs(("); emit_expr(c, arg, b); buf_puts(b, ") ? \"true\" : \"false\", stdout);\n");
  }
  else {
    unsupported(c, arg, "print argument");
  }
}
static void emit_p_one(Compiler *c, int arg, Buf *b, int indent) {
  TyKind t = comp_ntype(c, arg);
  /* `p x.class` prints the class name bare (it is a Class, not a String). */
  if (t == TY_STRING && nt_type(c->nt, arg) && !strcmp(nt_type(c->nt, arg), "CallNode") &&
      nt_str(c->nt, arg, "name") && !strcmp(nt_str(c->nt, arg, "name"), "class") &&
      nt_ref(c->nt, arg, "receiver") >= 0) {
    emit_indent(b, indent);
    buf_puts(b, "fputs("); emit_expr(c, arg, b); buf_puts(b, ", stdout); putchar('\\n');\n");
    return;
  }
  emit_indent(b, indent);
  if (t == TY_INT) {
    /* p of a nullable int at the sentinel prints "nil" */
    int tv = ++g_tmp;
    buf_printf(b, "{ mrb_int _t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; if (_t%d == SP_INT_NIL) fputs(\"nil\\n\", stdout); else printf(\"%%lld\\n\", (long long)_t%d); }\n", tv, tv);
  }
  else if (t == TY_FLOAT) {
    buf_puts(b, "{ const char *_fs = sp_float_opt_inspect("); emit_expr(c, arg, b);
    buf_puts(b, "); fputs(_fs, stdout); putchar('\\n'); }\n");
  }
  else if (t == TY_STRING) {
    /* a nullable string (NULL) prints "nil" */
    int tv = ++g_tmp;
    buf_printf(b, "{ const char *_t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; fputs(_t%d ? sp_str_inspect(_t%d) : \"nil\", stdout); putchar('\\n'); }\n", tv, tv);
  }
  else if (t == TY_BOOL) {
    buf_puts(b, "puts(("); emit_expr(c, arg, b); buf_puts(b, ") ? \"true\" : \"false\");\n");
  }
  else if (t == TY_SYMBOL) {
    buf_puts(b, "fputs(sp_str_concat(SPL(\":\"), sp_sym_to_s(");
    emit_expr(c, arg, b);
    buf_puts(b, ")), stdout); putchar('\\n');\n");
  }
  else if (ty_is_array(t) && array_kind(t)) {
    buf_printf(b, "fputs(sp_%sArray_inspect(", array_kind(t));
    emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  }
  else if (ty_is_hash(t) && ty_hash_cname(t)) {
    buf_printf(b, "fputs(sp_%sHash_inspect(", ty_hash_cname(t));
    emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  }
  else if (t == TY_POLY_ARRAY) {
    buf_puts(b, "fputs(sp_PolyArray_inspect("); emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  }
  else if (t == TY_POLY) {
    buf_puts(b, "fputs(sp_poly_inspect("); emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  }
  else if (t == TY_NIL) {
    buf_puts(b, "(void)("); emit_expr(c, arg, b); buf_puts(b, "); fputs(\"nil\\n\", stdout);\n");
  }
  else if (nt_type(c->nt, arg) && !strcmp(nt_type(c->nt, arg), "ArrayNode") &&
           ({ int _n = 0; nt_arr(c->nt, arg, "elements", &_n); _n == 0; })) {
    buf_puts(b, "fputs(\"[]\\n\", stdout);\n");  /* p [] */
  }
  else {
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
  if (!strcmp(name, "system") && argc >= 1) {
    int ts = ++g_tmp;
    emit_indent(b, indent);
    buf_printf(b, "{ const char *_sys_%d[] = { ", ts);
    for (int k = 0; k < argc; k++) { if (k > 0) buf_puts(b, ", "); emit_expr(c, argv[k], b); }
    buf_printf(b, ", NULL }; sp_system_args(%d, _sys_%d); }\n", argc, ts);
    return 1;
  }
  /* trap(...) stmt: no-op (Spinel has no signal-handler runtime) */
  if (!strcmp(name, "trap") && argc >= 1) return 1;
  if (!strcmp(name, "warn")) {
    /* Kernel#warn: each argument to stderr with a trailing newline */
    for (int k = 0; k < argc; k++) {
      TyKind at = comp_ntype(c, argv[k]);
      emit_indent(b, indent); buf_puts(b, "fputs(");
      if (at == TY_STRING) emit_expr(c, argv[k], b);
      else if (at == TY_INT) { buf_puts(b, "sp_int_to_s("); emit_expr(c, argv[k], b); buf_puts(b, ")"); }
      else if (at == TY_FLOAT) { buf_puts(b, "sp_float_to_s("); emit_expr(c, argv[k], b); buf_puts(b, ")"); }
      else if (at == TY_SYMBOL) { buf_puts(b, "sp_sym_to_s("); emit_expr(c, argv[k], b); buf_puts(b, ")"); }
      else { buf_puts(b, "((void)("); emit_expr(c, argv[k], b); buf_puts(b, "), \"\")"); }
      buf_puts(b, ", stderr); fputc('\\n', stderr);\n");
    }
    return 1;
  }
  return 0;
}

/* ---- assignment ---- */

static void emit_assign(Compiler *c, int id, Buf *b, int indent) {
  const char *nm = nt_str(c->nt, id, "name");
  int v = nt_ref(c->nt, id, "value");
  LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
  emit_indent(b, indent);
  emit_local_ref(c, id, nm, b);
  buf_puts(b, " = ");
  /* `x = nil` -> the variable's type-appropriate default */
  const char *vty = nt_type(c->nt, v);
  int vn = 0;
  int is_empty_array = vty && !strcmp(vty, "ArrayNode") && (nt_arr(c->nt, v, "elements", &vn), vn == 0);
  int hn = 0;
  int is_empty_hash = vty && !strcmp(vty, "HashNode") && (nt_arr(c->nt, v, "elements", &hn), hn == 0);
  /* h = Hash.new / Hash.new(default) */
  int is_hash_new = 0, hash_new_default = -1;
  if (vty && !strcmp(vty, "CallNode") && !strcmp(nt_str(c->nt, v, "name") ? nt_str(c->nt, v, "name") : "", "new")) {
    int hr = nt_ref(c->nt, v, "receiver");
    const char *hrt = hr >= 0 ? nt_type(c->nt, hr) : NULL;
    if (hrt && !strcmp(hrt, "ConstantReadNode") &&
        !strcmp(nt_str(c->nt, hr, "name") ? nt_str(c->nt, hr, "name") : "", "Hash")) {
      is_hash_new = 1;
      int ha = nt_ref(c->nt, v, "arguments");
      int hac = 0;
      const int *hav = ha >= 0 ? nt_arr(c->nt, ha, "arguments", &hac) : NULL;
      if (hac >= 1) hash_new_default = hav[0];
    }
  }

  if (vty && !strcmp(vty, "NilNode") && lv) {
    if (lv->type == TY_RANGE) buf_puts(b, "(sp_Range){0}");
    else buf_puts(b, default_value(lv->type));
  }
  else if (is_empty_array && lv && array_kind(lv->type)) {
    /* `a = []` -> a new array of the variable's resolved element type */
    buf_printf(b, "sp_%sArray_new()", array_kind(lv->type));
  }
  else if (is_empty_array && lv && lv->type == TY_POLY_ARRAY) {
    buf_puts(b, "sp_PolyArray_new()");
  }
  else if ((is_empty_hash || is_hash_new) && lv && ty_hash_cname(lv->type)) {
    const char *hcn = ty_hash_cname(lv->type);
    int poly_val = (lv->type == TY_SYM_POLY_HASH || lv->type == TY_STR_POLY_HASH);
    if (is_hash_new && hash_new_default >= 0) {
      buf_printf(b, "sp_%sHash_new_with_default(", hcn);
      if (poly_val) emit_boxed(c, hash_new_default, b); else emit_expr(c, hash_new_default, b);
      buf_puts(b, ")");
    }
    else {
      buf_printf(b, "sp_%sHash_new()", hcn);
    }
  }
  else if (lv && lv->type == TY_POLY_ARRAY && ty_is_array(comp_ntype(c, v)) && comp_ntype(c, v) != TY_POLY_ARRAY) {
    /* widen typed array literal to PolyArray for this slot */
    TyKind vt = comp_ntype(c, v);
    if (vt == TY_INT_ARRAY) { buf_puts(b, "sp_PolyArray_from_int_array("); emit_expr(c, v, b); buf_puts(b, ")"); }
    else if (vt == TY_STR_ARRAY) { buf_puts(b, "sp_PolyArray_from_str_array("); emit_expr(c, v, b); buf_puts(b, ")"); }
    else if (vt == TY_FLOAT_ARRAY) { buf_puts(b, "sp_PolyArray_from_float_array("); emit_expr(c, v, b); buf_puts(b, ")"); }
    else emit_expr(c, v, b);
  }
  else if (lv && lv->type == TY_POLY) {
    emit_boxed(c, v, b);   /* poly slot: box the (non-poly) RHS */
  }
  else if (lv && lv->type == TY_STR_POLY_HASH &&
           (comp_ntype(c, v) == TY_STR_STR_HASH || comp_ntype(c, v) == TY_STR_INT_HASH)) {
    /* widen a concrete str-keyed hash into the poly-valued slot */
    buf_printf(b, "sp_StrPolyHash_from_%s(", comp_ntype(c, v) == TY_STR_STR_HASH ? "str_str_hash" : "str_int_hash");
    emit_expr(c, v, b); buf_puts(b, ")");
  }
  else {
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
  const char *en = rename_local(nm);
  emit_indent(b, indent);

  /* A captured/cell var: x op= v is x = x op v through the cell deref. Only
     int cells exist today (capture is int-restricted). */
  int celled = (lv && lv->is_cell) || (g_cap_struct && g_cap_names && nameset_has(g_cap_names, nm));
  if (celled) {
    emit_local_ref(c, id, nm, b); buf_puts(b, " = ");
    if (t == TY_INT && (!strcmp(op, "+") || !strcmp(op, "-") || !strcmp(op, "*"))) {
      emit_local_ref(c, id, nm, b); buf_printf(b, " %s ", op); emit_expr(c, v, b); buf_puts(b, ";\n");
      return;
    }
    const char *fn = int_arith_fn(op);
    if (fn) { buf_printf(b, "%s(", fn); emit_local_ref(c, id, nm, b); buf_puts(b, ", "); emit_expr(c, v, b); buf_puts(b, ");\n"); return; }
    emit_local_ref(c, id, nm, b); buf_printf(b, " %s ", op); emit_expr(c, v, b); buf_puts(b, ";\n");
    return;
  }

  if (t == TY_STRING && !strcmp(op, "+")) {
    buf_printf(b, "lv_%s = sp_str_concat(lv_%s, ", en, en);
    emit_expr(c, v, b); buf_puts(b, ");\n");
    return;
  }
  if (t == TY_INT && (!strcmp(op, "+") || !strcmp(op, "-") || !strcmp(op, "*"))) {
    TyKind vt = comp_ntype(c, v);
    if (vt == TY_POLY) {
      buf_printf(b, "lv_%s %s= sp_poly_to_i(", en, op); emit_expr(c, v, b); buf_puts(b, ");\n");
    } else {
      buf_printf(b, "lv_%s %s= ", en, op); emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    return;
  }
  if (t == TY_INT) {
    const char *fn = int_arith_fn(op);
    if (fn) { buf_printf(b, "lv_%s = %s(lv_%s, ", en, fn, en); emit_expr(c, v, b); buf_puts(b, ");\n"); return; }
  }
  if (t == TY_FLOAT && (!strcmp(op, "+") || !strcmp(op, "-") || !strcmp(op, "*") || !strcmp(op, "/"))) {
    buf_printf(b, "lv_%s %s= ", en, op); emit_expr(c, v, b); buf_puts(b, ";\n");
    return;
  }
  if (ty_is_object(t)) {
    int defcls2 = -1;
    int cid2 = ty_object_class(t);
    int mi2 = comp_method_in_chain(c, cid2, op, &defcls2);
    if (mi2 >= 0) {
      Scope *ms2 = &c->scopes[mi2];
      LocalVar *p2 = ms2->nparams >= 1 ? scope_local(ms2, ms2->pnames[0]) : NULL;
      int atmp2 = ++g_tmp;
      emit_indent(g_pre, g_indent);
      emit_ctype(c, p2 ? p2->type : comp_ntype(c, v), g_pre);
      buf_printf(g_pre, " _t%d = ", atmp2);
      emit_expr(c, v, g_pre);
      buf_puts(g_pre, ";\n");
      buf_printf(b, "lv_%s = sp_%s_%s((sp_%s *)lv_%s, _t%d);\n",
                 en, c->classes[defcls2].name, mc(ms2->name),
                 c->classes[defcls2].name, en, atmp2);
      return;
    }
  }
  unsupported(c, id, "operator assignment");
}

/* ---- control flow ---- */

static void emit_cond(Compiler *c, int id, Buf *b) {
  TyKind t = comp_ntype(c, id);
  if (t == TY_POLY) { buf_puts(b, "sp_poly_truthy("); emit_expr(c, id, b); buf_puts(b, ")"); return; }
  if (t == TY_NIL)  { buf_puts(b, "(("); emit_expr(c, id, b); buf_puts(b, "), 0)"); return; }
  /* Ruby truthiness: only nil and false are falsy. A nullable scalar reads
     falsy at its sentinel (NULL string / SP_INT_NIL / NaN float); a pointer
     value is falsy when NULL. Every other concrete value is truthy. */
  if (t == TY_STRING || ty_is_array(t) || ty_is_hash(t) || ty_is_object(t) ||
      t == TY_PROC || t == TY_STRINGIO || t == TY_STRINGSCANNER || t == TY_MATCHDATA || t == TY_EXCEPTION) {
    buf_puts(b, "(("); emit_expr(c, id, b); buf_puts(b, ") != 0)"); return;
  }
  if (t == TY_INT)   { buf_puts(b, "(("); emit_expr(c, id, b); buf_puts(b, ") != SP_INT_NIL)"); return; }
  if (t == TY_FLOAT) { buf_puts(b, "(!sp_float_is_nil("); emit_expr(c, id, b); buf_puts(b, "))"); return; }
  if (t == TY_SYMBOL) { buf_puts(b, "(("); emit_expr(c, id, b); buf_puts(b, "), 1)"); return; }
  /* &block parameter used as condition — same semantics as block_given? */
  if (t == TY_UNKNOWN) {
    const char *nty = nt_type(c->nt, id);
    if (nty && !strcmp(nty, "LocalVariableReadNode")) {
      const char *nm = nt_str(c->nt, id, "name");
      Scope *s = nm ? comp_scope_of(c, id) : NULL;
      if (s && s->blk_param && nm && !strcmp(s->blk_param, nm)) {
        buf_puts(b, g_block_id >= 0 ? "1" : "0");
        return;
      }
    }
  }
  if (t != TY_BOOL) unsupported(c, id, "condition (non-bool)");
  emit_expr(c, id, b);
}

static void emit_if(Compiler *c, int id, Buf *b, int indent, int is_unless, int tail) {
  const NodeTable *nt = c->nt;
  int pred = nt_ref(nt, id, "predicate");
  int then_b = nt_ref(nt, id, "statements");
  int sub = nt_ref(nt, id, is_unless ? "else_clause" : "subsequent");

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
    }
    else if (sty && !strcmp(sty, "IfNode")) {
      buf_puts(b, "\n");
      emit_indent(b, indent);
      buf_puts(b, "else {\n");
      emit_if(c, sub, b, indent + 1, 0, tail);
      emit_indent(b, indent); buf_puts(b, "}\n");
    }
    else {
      buf_puts(b, "\n");
    }
  }
  else {
    buf_puts(b, "\n");
  }
}

/* Emit `when ClassName` test against a poly (sp_RbVal) scrutinee temp.
   Returns 1 if the class is known and the check was emitted, 0 otherwise. */
static int emit_poly_class_when(Compiler *c, int cond_id, const char *tmp, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *cty = nt_type(nt, cond_id);
  if (!cty || strcmp(cty, "ConstantReadNode")) return 0;
  const char *cn = nt_str(nt, cond_id, "name");
  if (!cn) return 0;
  if (!strcmp(cn, "Integer") || !strcmp(cn, "Fixnum"))
    buf_printf(b, "%s.tag == SP_TAG_INT", tmp);
  else if (!strcmp(cn, "String"))
    buf_printf(b, "%s.tag == SP_TAG_STR", tmp);
  else if (!strcmp(cn, "Float"))
    buf_printf(b, "%s.tag == SP_TAG_FLT", tmp);
  else if (!strcmp(cn, "Symbol"))
    buf_printf(b, "%s.tag == SP_TAG_SYM", tmp);
  else if (!strcmp(cn, "NilClass"))
    buf_printf(b, "%s.tag == SP_TAG_NIL", tmp);
  else if (!strcmp(cn, "TrueClass"))
    buf_printf(b, "(%s.tag == SP_TAG_BOOL && %s.v.b)", tmp, tmp);
  else if (!strcmp(cn, "FalseClass"))
    buf_printf(b, "(%s.tag == SP_TAG_BOOL && !%s.v.b)", tmp, tmp);
  else if (!strcmp(cn, "Numeric"))
    buf_printf(b, "(%s.tag == SP_TAG_INT || %s.tag == SP_TAG_FLT)", tmp, tmp);
  else if (!strcmp(cn, "Range"))
    buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id == SP_BUILTIN_RANGE)", tmp, tmp);
  else if (!strcmp(cn, "Array"))
    buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id <= -1 && %s.cls_id >= -12)", tmp, tmp, tmp);
  else if (!strcmp(cn, "Hash"))
    buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id <= -13 && %s.cls_id >= -20)", tmp, tmp, tmp);
  else {
    int cid = comp_class_index(c, cn);
    if (cid >= 0) {
      buf_printf(b, "(%s.tag == SP_TAG_OBJ && (", tmp);
      int first = 1;
      for (int k = 0; k < c->nclasses; k++) {
        if (k == cid || is_descendant(c, k, cid)) {
          buf_printf(b, "%s%s.cls_id == %d", first ? "" : " || ", tmp, k);
          first = 0;
        }
      }
      if (first) buf_puts(b, "0");
      buf_puts(b, "))");
    }
    else return 0;
  }
  return 1;
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
        /* `when *arr` — array membership test */
        if (nt_type(nt, conds[j]) && !strcmp(nt_type(nt, conds[j]), "SplatNode")) {
          int inner = nt_ref(nt, conds[j], "expression");
          TyKind at = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
          int ta = ++g_tmp;
          if (at == TY_INT_ARRAY) {
            buf_printf(b, "({ sp_IntArray *_t%d = ", ta); emit_expr(c, inner, b);
            buf_printf(b, "; _t%d && sp_IntArray_include(_t%d, _t%d); })", ta, ta, t);
          }
          else if (at == TY_STR_ARRAY) {
            buf_printf(b, "({ sp_StrArray *_t%d = ", ta); emit_expr(c, inner, b);
            buf_printf(b, "; _t%d && sp_StrArray_include(_t%d, _t%d); })", ta, ta, t);
          }
          else if (at == TY_FLOAT_ARRAY) {
            buf_printf(b, "({ sp_FloatArray *_t%d = ", ta); emit_expr(c, inner, b);
            buf_printf(b, "; _t%d && sp_FloatArray_include(_t%d, _t%d); })", ta, ta, t);
          }
          else if (at == TY_POLY_ARRAY) {
            buf_printf(b, "({ sp_PolyArray *_t%d = ", ta); emit_expr(c, inner, b);
            buf_printf(b, "; _t%d && sp_PolyArray_include(_t%d, ", ta, ta);
            emit_boxed(c, pred, b);
            buf_puts(b, "); })");
          }
          else {
            buf_puts(b, "0 /* unsupported splat type */");
          }
        }
        else {
          const char *cnty = nt_type(nt, conds[j]);
          /* RationalNode: `when 0r` — matches integer iff denominator==1 */
          if (cnty && !strcmp(cnty, "RationalNode")) {
            const char *rnum = nt_str(nt, conds[j], "rat_num");
            const char *rden = nt_str(nt, conds[j], "rat_den");
            long long den = rden ? atoll(rden) : 1;
            long long num = rnum ? atoll(rnum) : 0;
            if (den == 1) buf_printf(b, "(_t%d == %lldLL)", t, num);
            else buf_puts(b, "0");
          }
          /* ImaginaryNode: `when 0i` — Complex(0,imag); integer matches only if imag==0 */
          else if (cnty && !strcmp(cnty, "ImaginaryNode")) {
            int numnode = nt_ref(nt, conds[j], "numeric");
            long long imval = numnode >= 0 ? (long long)nt_int(nt, numnode, "value", 0) : -1;
            if (imval == 0) buf_printf(b, "(_t%d == 0LL)", t);
            else buf_puts(b, "0");
          }
          else {
          /* when ClassName: Module#=== resolves via is_a? semantics */
          const char *cty2 = nt_type(nt, conds[j]);
          const char *cn2 = cty2 && !strcmp(cty2, "ConstantReadNode") ? nt_str(nt, conds[j], "name") : NULL;
          if (cn2 && pt == TY_POLY) {
            char tmp[32]; snprintf(tmp, sizeof tmp, "_t%d", t);
            if (!emit_poly_class_when(c, conds[j], tmp, b))
              buf_puts(b, "0");
          }
          else if (cn2) {
            int yes = ty_matches_class(pt, cn2, 0);
            buf_printf(b, "%d", yes > 0 ? 1 : 0);
          }
          else {
          int reidx = re_lit_index(c, conds[j]);
          if (reidx >= 0 && pt == TY_STRING) {
            buf_printf(b, "sp_re_match_p(sp_re_pat_%d, _t%d)", reidx, t);
          }
          else if (comp_ntype(c, conds[j]) == TY_RANGE && pt != TY_STRING) {
            /* `when lo..hi` is range membership, not equality */
            int tr = ++g_tmp;
            buf_printf(b, "({ sp_Range _t%d = ", tr); emit_expr(c, conds[j], b);
            buf_printf(b, "; sp_range_include(&_t%d, _t%d); })", tr, t);
          }
          else if (eq_family(pt) && eq_family(comp_ntype(c, conds[j])) && eq_family(pt) != eq_family(comp_ntype(c, conds[j]))) {
            /* a when value of a different comparable family never matches */
            buf_puts(b, "0");
          }
          else if (pt == TY_STRING) {
            buf_printf(b, "sp_str_eq(_t%d, ", t); emit_expr(c, conds[j], b); buf_puts(b, ")");
          }
          else if (pt == TY_POLY) {
            buf_printf(b, "sp_poly_eq(_t%d, ", t); emit_boxed(c, conds[j], b); buf_puts(b, ")");
          }
          else {
            buf_printf(b, "(_t%d == ", t); emit_expr(c, conds[j], b); buf_puts(b, ")");
          }
          } /* close non-ConstantReadNode else */
          } /* close else { int reidx... } */
        }
      }
      else {
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

/* Emit `_crN = <branch's last value>` (boxed to the case's result type when
   that is poly), after the branch's leading statements. */
static void emit_case_branch_value(Compiler *c, int stmts, TyKind rt, int cr, Buf *b) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *bb = stmts >= 0 ? nt_arr(nt, stmts, "body", &n) : NULL;
  for (int k = 0; k < n - 1; k++) emit_stmt(c, bb[k], b, 0);
  buf_printf(b, "_cr%d = ", cr);
  if (n > 0) { if (rt == TY_POLY) emit_boxed(c, bb[n - 1], b); else emit_expr(c, bb[n - 1], b); }
  else buf_puts(b, rt == TY_POLY ? "sp_box_nil()" : default_value(rt));
  buf_puts(b, "; ");
}

/* `case` in expression position: a GCC statement-expression yielding the
   matched branch's value (or the result type's nil/default on no match). */
static void emit_case_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  TyKind rt = comp_ntype(c, id);
  int pred = nt_ref(nt, id, "predicate");
  int nw = 0;
  const int *whens = nt_arr(nt, id, "conditions", &nw);
  int else_c = nt_ref(nt, id, "else_clause");
  int cr = ++g_tmp;
  buf_puts(b, "({ ");
  emit_ctype(c, rt, b);
  buf_printf(b, " _cr%d = %s; ", cr, rt == TY_RANGE ? "(sp_Range){0}" : default_value(rt));
  int t = -1;
  TyKind pt = TY_UNKNOWN;
  if (pred >= 0) {
    pt = comp_ntype(c, pred);
    t = ++g_tmp;
    emit_ctype(c, pt, b); buf_printf(b, " _t%d = ", t); emit_expr(c, pred, b); buf_puts(b, "; ");
  }
  for (int w = 0; w < nw; w++) {
    int wn = whens[w];
    int wc = 0;
    const int *conds = nt_arr(nt, wn, "conditions", &wc);
    buf_puts(b, w == 0 ? "if (" : "else if (");
    for (int j = 0; j < wc; j++) {
      if (j) buf_puts(b, " || ");
      if (pred >= 0) {
        /* when ClassName: Module#=== resolves via is_a? semantics */
        const char *cty2 = nt_type(nt, conds[j]);
        const char *cn2 = cty2 && !strcmp(cty2, "ConstantReadNode") ? nt_str(nt, conds[j], "name") : NULL;
        if (cn2 && pt == TY_POLY) {
          char tmp[32]; snprintf(tmp, sizeof tmp, "_t%d", t);
          if (!emit_poly_class_when(c, conds[j], tmp, b)) buf_puts(b, "0");
        }
        else if (cn2) { int yes = ty_matches_class(pt, cn2, 0); buf_printf(b, "%d", yes > 0 ? 1 : 0); }
        else {
        int reidx = re_lit_index(c, conds[j]);
        if (reidx >= 0 && pt == TY_STRING) { buf_printf(b, "sp_re_match_p(sp_re_pat_%d, _t%d)", reidx, t); }
        else if (comp_ntype(c, conds[j]) == TY_RANGE && pt != TY_STRING) {
          int tr = ++g_tmp;
          buf_printf(b, "({ sp_Range _t%d = ", tr); emit_expr(c, conds[j], b);
          buf_printf(b, "; sp_range_include(&_t%d, _t%d); })", tr, t);
        }
        else if (eq_family(pt) && eq_family(comp_ntype(c, conds[j])) && eq_family(pt) != eq_family(comp_ntype(c, conds[j]))) {
          /* a when value of a different comparable family never matches */
          buf_puts(b, "0");
        }
        else if (pt == TY_STRING) { buf_printf(b, "sp_str_eq(_t%d, ", t); emit_expr(c, conds[j], b); buf_puts(b, ")"); }
        else if (pt == TY_POLY) { buf_printf(b, "sp_poly_eq(_t%d, ", t); emit_boxed(c, conds[j], b); buf_puts(b, ")"); }
        else { buf_printf(b, "(_t%d == ", t); emit_expr(c, conds[j], b); buf_puts(b, ")"); }
        } /* close non-ConstantReadNode else */
      }
      else { buf_puts(b, "("); emit_expr(c, conds[j], b); buf_puts(b, ")"); }
    }
    buf_puts(b, ") { ");
    emit_case_branch_value(c, nt_ref(nt, wn, "statements"), rt, cr, b);
    buf_puts(b, "} ");
  }
  if (else_c >= 0) {
    buf_puts(b, "else { ");
    emit_case_branch_value(c, nt_ref(nt, else_c, "statements"), rt, cr, b);
    buf_puts(b, "} ");
  }
  buf_printf(b, "_cr%d; })", cr);
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

static void emit_for(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  int idx = nt_ref(nt, id, "index");
  int coll = nt_ref(nt, id, "collection");
  int body = nt_ref(nt, id, "statements");
  const char *vn = idx >= 0 ? nt_str(nt, idx, "name") : NULL;
  TyKind ct = comp_ntype(c, coll);

  if (ct == TY_RANGE && nt_type(nt, coll) && !strcmp(nt_type(nt, coll), "RangeNode")) {
    /* for v in lo..hi -- a plain counted loop */
    int excl = (int)(nt_int(nt, coll, "flags", 0) & 4) ? 1 : 0;
    int thi = ++g_tmp;
    emit_indent(b, indent); buf_puts(b, "{ mrb_int ");
    buf_printf(b, "_t%d = ", thi); emit_expr(c, nt_ref(nt, coll, "right"), b); buf_puts(b, ";\n");
    emit_indent(b, indent + 1);
    buf_printf(b, "for (lv_%s = ", vn); emit_expr(c, nt_ref(nt, coll, "left"), b);
    buf_printf(b, "; lv_%s %s _t%d; lv_%s++) {\n", vn, excl ? "<" : "<=", thi, vn);
    emit_stmts(c, body, b, indent + 2);
    emit_indent(b, indent + 1); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    return;
  }
  if (ty_is_array(ct) || ct == TY_POLY_ARRAY) {
    const char *k = array_kind(ct);
    int ta = ++g_tmp, ti = ++g_tmp;
    /* Multi-variable for: `for a, b in coll` -- each element is an inner array. */
    const char *idx_ty = nt_type(nt, idx);
    if (idx_ty && !strcmp(idx_ty, "MultiTargetNode")) {
      int ln = 0;
      const int *lefts = nt_arr(nt, idx, "lefts", &ln);
      int tv = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "{ sp_%sArray *_t%d = ", k ? k : "Poly", ta); emit_expr(c, coll, b); buf_puts(b, ";\n");
      emit_indent(b, indent + 1);
      buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                 ti, ti, k ? k : "Poly", ta, ti);
      emit_indent(b, indent + 2);
      /* get the outer element as a poly value for inner destructuring */
      if (k) /* typed array: box the element to poly */
        buf_printf(b, "sp_RbVal _t%d = sp_box_%s(sp_%sArray_get(_t%d, _t%d));\n",
                   tv, !strcmp(k,"Int")?"int":!strcmp(k,"Float")?"float":"str", k, ta, ti);
      else
        buf_printf(b, "sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);\n", tv, ta, ti);
      for (int i = 0; i < ln; i++) {
        const char *lnm = nt_str(nt, lefts[i], "name");
        if (!lnm) continue;
        TyKind vt = scope_local(comp_scope_of(c, idx), lnm) ?
                    scope_local(comp_scope_of(c, idx), lnm)->type : TY_POLY;
        emit_indent(b, indent + 2);
        if (vt == TY_INT || vt == TY_UNKNOWN)
          buf_printf(b, "lv_%s = sp_unbox_int(sp_poly_arr_get(_t%d, %d));\n", lnm, tv, i);
        else if (vt == TY_FLOAT)
          buf_printf(b, "lv_%s = sp_unbox_float(sp_poly_arr_get(_t%d, %d));\n", lnm, tv, i);
        else if (vt == TY_STRING)
          buf_printf(b, "lv_%s = sp_unbox_str(sp_poly_arr_get(_t%d, %d));\n", lnm, tv, i);
        else
          buf_printf(b, "lv_%s = sp_poly_arr_get(_t%d, %d);\n", lnm, tv, i);
      }
      emit_stmts(c, body, b, indent + 2);
      emit_indent(b, indent + 1); buf_puts(b, "}\n");
      emit_indent(b, indent); buf_puts(b, "}\n");
      return;
    }
    emit_indent(b, indent);
    buf_printf(b, "{ sp_%sArray *_t%d = ", k ? k : "Poly", ta); emit_expr(c, coll, b); buf_puts(b, ";\n");
    emit_indent(b, indent + 1);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
               ti, ti, k ? k : "Poly", ta, ti);
    emit_indent(b, indent + 2);
    buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", vn, k ? k : "Poly", ta, ti);
    emit_stmts(c, body, b, indent + 2);
    emit_indent(b, indent + 1); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    return;
  }
  emit_indent(b, indent); buf_puts(b, "/* unsupported for-loop collection */\n");
}

static void emit_return(Compiler *c, int id, Buf *b, int indent) {
  int args = nt_ref(c->nt, id, "arguments");
  int n = 0;
  const int *a = args >= 0 ? nt_arr(c->nt, args, "arguments", &n) : NULL;

  if (g_ensure_depth > 0) {
    /* Inside a begin..ensure body: defer the return until ensure runs. */
    EnsureCtx *ctx = &g_ensure_stack[g_ensure_depth - 1];
    emit_indent(b, indent);
    buf_puts(b, "{ ");
    if (ctx->has_retval) {
      if (n > 1) {
        int ta = ++g_tmp;
        buf_printf(b, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", ta, ta);
        for (int k = 0; k < n; k++) {
          buf_printf(b, "sp_PolyArray_push(_t%d, ", ta);
          emit_boxed(c, a[k], b);
          buf_puts(b, "); ");
        }
        buf_printf(b, "_retv%d = _t%d; ", ctx->lid, ta);
      }
      else if (n > 0) {
        buf_printf(b, "_retv%d = ", ctx->lid);
        if (g_ret_type == TY_POLY && comp_ntype(c, a[0]) != TY_POLY) emit_boxed(c, a[0], b);
        else emit_expr(c, a[0], b);
        buf_puts(b, "; ");
      }
    }
    buf_printf(b, "_retf%d = 1; sp_exc_top--; goto _ensure%d; }\n",
               ctx->lid, ctx->lid);
    return;
  }

  emit_indent(b, indent);
  if (n > 1) {
    int ta = ++g_tmp;
    buf_printf(b, "{ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ta, ta);
    for (int k = 0; k < n; k++) {
      buf_printf(b, " sp_PolyArray_push(_t%d, ", ta);
      emit_boxed(c, a[k], b);
      buf_puts(b, ");");
    }
    buf_printf(b, " return _t%d; }\n", ta);
  }
  else if (n > 0) {
    buf_puts(b, "return ");
    if (g_ret_type == TY_POLY && comp_ntype(c, a[0]) != TY_POLY) emit_boxed(c, a[0], b);
    else emit_expr(c, a[0], b);
    buf_puts(b, ";\n");
  }
  else if (g_ret_type == TY_POLY) buf_puts(b, "return sp_box_nil();\n");
  else buf_puts(b, "return;\n");
}

static void emit_stmt_inner(Compiler *c, int id, Buf *b, int indent);
static void emit_stmt_tail_inner(Compiler *c, int id, Buf *b, int indent);

/* A rescue type name that conventionally catches "anything" in tests. */
static int rescue_is_catchall_name(const char *n) {
  return n && (!strcmp(n, "StandardError") || !strcmp(n, "Exception") ||
               !strcmp(n, "RuntimeError"));
}

/* Emit one rescue clause (and its `subsequent` chain) inside the handler
   branch. Frame counter `fr` makes the saved cls/msg vars unique. */
static void emit_rescue(Compiler *c, int id, Buf *b, int indent, int fr, const char *resultvar) {
  const NodeTable *nt = c->nt;
  int nexc = 0;
  const int *exc = nt_arr(nt, id, "exceptions", &nexc);
  int ref = nt_ref(nt, id, "reference");
  int stmts = nt_ref(nt, id, "statements");
  int sub = nt_ref(nt, id, "subsequent");

  int rc = ++g_tmp;
  emit_indent(b, indent);
  buf_printf(b, "const char *_rcls_%d = (const char *)sp_last_exc_cls; (void)_rcls_%d;\n", rc, rc);
  emit_indent(b, indent);
  buf_printf(b, "const char *_rmsg_%d = sp_exc_msg[sp_exc_top]; (void)_rmsg_%d;\n", rc, rc);

  /* type-match condition: catch-all when no types or a StandardError-ish
     type; otherwise exact class-name match */
  int catchall = (nexc == 0);
  for (int i = 0; i < nexc; i++) {
    const char *en = nt_type(nt, exc[i]);
    if (en && !strcmp(en, "ConstantReadNode") && rescue_is_catchall_name(nt_str(nt, exc[i], "name")))
      catchall = 1;
  }

  const char *save_cls = g_rescue_cls, *save_msg = g_rescue_msg;
  static char clsbuf[32], msgbuf[32];
  snprintf(clsbuf, sizeof clsbuf, "_rcls_%d", rc);
  snprintf(msgbuf, sizeof msgbuf, "_rmsg_%d", rc);

  if (!catchall) {
    emit_indent(b, indent);
    buf_puts(b, "if (");
    int first = 1;
    for (int i = 0; i < nexc; i++) {
      const char *en = nt_type(nt, exc[i]);
      if (!en || (strcmp(en, "ConstantReadNode") && strcmp(en, "ConstantPathNode"))) continue;
      if (!first) buf_puts(b, " || ");
      first = 0;
      buf_printf(b, "sp_str_eq(_rcls_%d, \"%s\")", rc, nt_str(nt, exc[i], "name"));
    }
    if (first) buf_puts(b, "1");  /* no usable type -> always */
    buf_puts(b, ") {\n");
    indent++;
  }

  g_rescue_cls = clsbuf; g_rescue_msg = msgbuf;
  if (ref >= 0 && nt_type(nt, ref) && !strcmp(nt_type(nt, ref), "LocalVariableTargetNode")) {
    emit_indent(b, indent);
    buf_printf(b, "lv_%s = sp_exc_new(_rcls_%d, _rmsg_%d);\n", nt_str(nt, ref, "name"), rc, rc);
  }
  if (resultvar) {
    const char *sv = g_result_var; g_result_var = resultvar;
    emit_stmts_tail(c, stmts, b, indent);
    g_result_var = sv;
  }
  else {
    emit_stmts(c, stmts, b, indent);
  }
  g_rescue_cls = save_cls; g_rescue_msg = save_msg;

  if (!catchall) {
    indent--;
    emit_indent(b, indent);
    buf_puts(b, "}\n");
    emit_indent(b, indent);
    buf_puts(b, "else {\n");
    if (sub >= 0) emit_rescue(c, sub, b, indent + 1, fr, resultvar);
    else {
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_raise_cls(_rcls_%d, _rmsg_%d);\n", rc, rc);
    }
    emit_indent(b, indent);
    buf_puts(b, "}\n");
  }
}

/* begin/body/rescue (ensure/else deferred) via the setjmp exception model.
   When resultvar != NULL, the body's and rescue handlers' values are
   assigned to it (begin/rescue as an expression). */
static void emit_begin(Compiler *c, int id, Buf *b, int indent, const char *resultvar) {
  const NodeTable *nt = c->nt;
  int body = nt_ref(nt, id, "statements");
  int rescue = nt_ref(nt, id, "rescue_clause");
  int else_c = nt_ref(nt, id, "else_clause");
  int ensure_c = nt_ref(nt, id, "ensure_clause");
  int else_stmts = else_c >= 0 ? nt_ref(nt, else_c, "statements") : -1;
  int ensure_stmts = ensure_c >= 0 ? nt_ref(nt, ensure_c, "statements") : -1;
  int fr = ++g_tmp;

  if (ensure_stmts >= 0 && g_ensure_depth < MAX_ENSURE_DEPTH) {
    /* Ensure clause present: use goto-based deferred-return mechanism so that
       a `return` inside the body still runs the ensure before leaving. */
    int eid = ++g_tmp;
    int has_retval = (g_ret_type != TY_VOID && g_ret_type != TY_UNKNOWN);
    emit_indent(b, indent); buf_printf(b, "int _retf%d = 0;\n", eid);
    /* _excf/_excmsg/_exccls track an unhandled exception (no rescue) so
       that ensure can re-raise it after running.  Saved immediately after
       sp_exc_top-- while the index is still valid. */
    emit_indent(b, indent); buf_printf(b, "int _excf%d = 0;\n", eid);
    emit_indent(b, indent); buf_printf(b, "const char *_excmsg%d = NULL;\n", eid);
    emit_indent(b, indent); buf_printf(b, "const char *_exccls%d = NULL;\n", eid);
    if (has_retval) {
      emit_indent(b, indent); emit_ctype(c, g_ret_type, b);
      buf_printf(b, " _retv%d = %s;\n", eid, default_value(g_ret_type));
    }
    g_ensure_stack[g_ensure_depth++] = (EnsureCtx){ eid, has_retval };

    emit_indent(b, indent); buf_puts(b, "sp_exc_top++;\n");
    emit_indent(b, indent); buf_puts(b, "if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {\n");
    if (resultvar && else_stmts < 0) {
      const char *sv = g_result_var; g_result_var = resultvar;
      emit_stmts_tail(c, body, b, indent + 1);
      g_result_var = sv;
    }
    else {
      emit_stmts(c, body, b, indent + 1);
    }
    emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
    if (else_stmts >= 0) {
      if (resultvar) {
        const char *sv = g_result_var; g_result_var = resultvar;
        emit_stmts_tail(c, else_stmts, b, indent + 1);
        g_result_var = sv;
      }
      else emit_stmts(c, else_stmts, b, indent + 1);
    }
    emit_indent(b, indent); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "else {\n");
    emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
    if (rescue >= 0) {
      emit_rescue(c, rescue, b, indent + 1, fr, resultvar);
    }
    else {
      /* No rescue: save exception info for re-raise after ensure runs.
         sp_exc_top has just been decremented so sp_exc_top is the right index. */
      emit_indent(b, indent + 1);
      buf_printf(b, "_excf%d = 1; _excmsg%d = sp_exc_msg[sp_exc_top]; _exccls%d = sp_exc_cls[sp_exc_top];\n",
                 eid, eid, eid);
    }
    emit_indent(b, indent); buf_puts(b, "}\n");

    g_ensure_depth--;

    /* Ensure label: reached by deferred-return goto AND by normal fall-through. */
    buf_printf(b, "_ensure%d: ;\n", eid);
    emit_stmts(c, ensure_stmts, b, indent);

    emit_indent(b, indent);
    if (g_ensure_depth > 0) {
      EnsureCtx *outer = &g_ensure_stack[g_ensure_depth - 1];
      if (has_retval && outer->has_retval) {
        buf_printf(b, "if (_retf%d) { _retv%d = _retv%d; _retf%d = 1; sp_exc_top--; goto _ensure%d; }\n",
                   eid, outer->lid, eid, outer->lid, outer->lid);
      }
      else {
        buf_printf(b, "if (_retf%d) { _retf%d = 1; sp_exc_top--; goto _ensure%d; }\n",
                   eid, outer->lid, outer->lid);
      }
      /* Unhandled exception: propagate info to outer ensure context. */
      emit_indent(b, indent);
      buf_printf(b, "if (_excf%d) { _excf%d = 1; _excmsg%d = _excmsg%d; _exccls%d = _exccls%d; sp_exc_top--; goto _ensure%d; }\n",
                 eid, outer->lid, outer->lid, eid, outer->lid, eid, outer->lid);
    }
    else {
      if (has_retval) buf_printf(b, "if (_retf%d) return _retv%d;\n", eid, eid);
      else if (g_ret_type == TY_POLY) buf_printf(b, "if (_retf%d) return sp_box_nil();\n", eid);
      else if (g_ret_type == TY_UNKNOWN) buf_printf(b, "if (_retf%d) return 0;\n", eid); /* main() */
      else buf_printf(b, "if (_retf%d) return;\n", eid);
      /* Unhandled exception: re-raise using the saved class/message. */
      emit_indent(b, indent);
      buf_printf(b, "if (_excf%d) sp_raise_cls(_exccls%d, _excmsg%d);\n", eid, eid, eid);
    }
    return;
  }

  /* No ensure (or ensure depth limit reached): original structure. */
  emit_indent(b, indent); buf_puts(b, "sp_exc_top++;\n");
  emit_indent(b, indent); buf_puts(b, "if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {\n");
  /* body value is the begin value only when there is no else clause */
  if (resultvar && else_stmts < 0) {
    const char *sv = g_result_var; g_result_var = resultvar;
    emit_stmts_tail(c, body, b, indent + 1);
    g_result_var = sv;
  }
  else {
    emit_stmts(c, body, b, indent + 1);
  }
  emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
  if (else_stmts >= 0) {  /* else runs only on success; its value is the begin value */
    if (resultvar) {
      const char *sv = g_result_var; g_result_var = resultvar;
      emit_stmts_tail(c, else_stmts, b, indent + 1);
      g_result_var = sv;
    }
    else {
      emit_stmts(c, else_stmts, b, indent + 1);
    }
  }
  if (ensure_stmts >= 0) emit_stmts(c, ensure_stmts, b, indent + 1);
  emit_indent(b, indent); buf_puts(b, "}\n");
  emit_indent(b, indent); buf_puts(b, "else {\n");
  emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
  if (rescue >= 0) emit_rescue(c, rescue, b, indent + 1, fr, resultvar);
  if (ensure_stmts >= 0) emit_stmts(c, ensure_stmts, b, indent + 1);
  emit_indent(b, indent); buf_puts(b, "}\n");
}

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

  if (!strcmp(ty, "YieldNode")) {
    if (g_block_id < 0) return;  /* inlined without block: yield is dead code */
    emit_block_invoke(c, nt_ref(nt, id, "arguments"), b, indent, 0);
    return;
  }

  if (!strcmp(ty, "CallNode")) {
    /* declarative-only calls emitted as no-ops */
    {
      const char *nm = nt_str(nt, id, "name");
      int recv = nt_ref(nt, id, "receiver");
      if (recv < 0 && nm && (!strcmp(nm, "include") || !strcmp(nm, "extend") ||
                             !strcmp(nm, "prepend") || !strcmp(nm, "module_function") ||
                             !strcmp(nm, "private") || !strcmp(nm, "protected") ||
                             !strcmp(nm, "public") || !strcmp(nm, "attr_reader") ||
                             !strcmp(nm, "attr_writer") || !strcmp(nm, "attr_accessor"))) {
        /* These are class-body declarations handled at analysis time; skip. */
        return;
      }
    }
    if (is_block_call(c, id)) { emit_block_invoke(c, nt_ref(nt, id, "arguments"), b, indent, 0); return; }
    if (emit_output_call(c, id, b, indent)) return;
    /* Signal.trap / ::Signal.trap stmt: no-op */
    {
      const char *snm = nt_str(nt, id, "name");
      int srecv = nt_ref(nt, id, "receiver");
      int sargs = nt_ref(nt, id, "arguments");
      int sargc = 0; if (sargs >= 0) nt_arr(nt, sargs, "arguments", &sargc);
      if (srecv >= 0 && snm && !strcmp(snm, "trap") && sargc >= 1) {
        const char *rty2 = nt_type(nt, srecv);
        if (rty2 && (!strcmp(rty2, "ConstantReadNode") || !strcmp(rty2, "ConstantPathNode"))) {
          const char *rn = nt_str(nt, srecv, "name");
          if (rn && !strcmp(rn, "Signal")) return;  /* no-op */
        }
      }
    }
    if (emit_inline_call(c, id, b, indent)) return;
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
                int rc = ty_object_class(rt);
                char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", base);
                int defc = -1; comp_writer_in_chain(c, rc, base, &defc);
                int iv = comp_ivar_index(&c->classes[defc < 0 ? rc : defc], ivn);
                TyKind ivt = iv >= 0 ? c->classes[defc < 0 ? rc : defc].ivar_types[iv] : TY_UNKNOWN;
                emit_indent(b, indent);
                buf_puts(b, "("); emit_expr(c, recv, b); buf_printf(b, ")->iv_%s = ", base);
                if (ivt == TY_POLY && comp_ntype(c, argv[0]) != TY_POLY) emit_boxed(c, argv[0], b);
                else emit_expr(c, argv[0], b);
                buf_puts(b, ";\n");
                return;
              }
            }
          }
        }
        /* poly receiver: switch on cls_id and store into each candidate
           class's ivar, converting the rhs to that ivar's slot type. */
        else if (rt == TY_POLY) {
          char base[256];
          if (ln - 1 < sizeof base) {
            memcpy(base, nm, ln - 1); base[ln - 1] = '\0';
            int args = nt_ref(nt, id, "arguments");
            int an = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
            int ncand = 0;
            for (int k = 0; k < c->nclasses; k++)
              if (comp_is_writer(&c->classes[k], base)) ncand++;
            if (an >= 1 && ncand > 0) {
              TyKind at = comp_ntype(c, argv[0]);
              int tv = ++g_tmp, tval = ++g_tmp;
              emit_indent(b, indent);
              buf_printf(b, "{ sp_RbVal _t%d = ", tv); emit_expr(c, recv, b); buf_puts(b, "; ");
              emit_ctype(c, at, b); buf_printf(b, " _t%d = ", tval); emit_expr(c, argv[0], b); buf_puts(b, ";");
              buf_printf(b, " switch (_t%d.cls_id) {", tv);
              char src[32]; snprintf(src, sizeof src, "_t%d", tval);
              for (int k = 0; k < c->nclasses; k++) {
                if (!comp_is_writer(&c->classes[k], base)) continue;
                char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", base);
                int iv = comp_ivar_index(&c->classes[k], ivn);
                TyKind ivt = iv >= 0 ? c->classes[k].ivar_types[iv] : at;
                /* skip a class whose slot can't hold this concrete rhs (the
                   runtime object isn't that class anyway): a raw assignment
                   between mismatched C types would not compile */
                if (at != ivt && at != TY_POLY && ivt != TY_POLY) continue;
                buf_printf(b, " case %d: ((sp_%s *)_t%d.v.p)->iv_%s = ", k, c->classes[k].name, tv, base);
                if (ivt == TY_POLY && at != TY_POLY) emit_boxed_text(c, at, src, b);
                else if (at == TY_POLY && ivt != TY_POLY) emit_unbox_text(c, ivt, src, b);
                else buf_puts(b, src);
                buf_puts(b, "; break;");
              }
              buf_puts(b, " } }\n");
              return;
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
  if (!strcmp(ty, "LocalVariableOrWriteNode") || !strcmp(ty, "LocalVariableAndWriteNode")) {
    int is_or = !strcmp(ty, "LocalVariableOrWriteNode");
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
    TyKind t = lv ? lv->type : TY_UNKNOWN;
    const char *en = rename_local(nm);
    if (t == TY_POLY) {
      emit_indent(b, indent);
      buf_printf(b, "if (%ssp_poly_truthy(lv_%s)) lv_%s = ", is_or ? "!" : "", en, en);
      emit_boxed(c, v, b); buf_puts(b, ";\n");
    }
    else if (t == TY_BOOL) {
      emit_indent(b, indent);
      buf_printf(b, "if (%slv_%s) lv_%s = ", is_or ? "!" : "", en, en);
      emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    else if (!is_or) {  /* a &&= v on an always-truthy var: always assign */
      emit_indent(b, indent);
      buf_printf(b, "lv_%s = ", en); emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    /* a ||= v on an always-truthy var: no-op */
    return;
  }
  if (!strcmp(ty, "InstanceVariableOrWriteNode") || !strcmp(ty, "InstanceVariableAndWriteNode")) {
    int is_or = !strcmp(ty, "InstanceVariableOrWriteNode");
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    Scope *cws2 = comp_scope_of(c, id);
    int sc2 = cws2 ? cws2->class_id : -1;
    TyKind ivt2 = TY_UNKNOWN;
    if (sc2 >= 0) { int iv2 = comp_ivar_index(&c->classes[sc2], nm); if (iv2 >= 0) ivt2 = c->classes[sc2].ivar_types[iv2]; }
    char ref2[300];
    if (cws2 && cws2->is_cmethod && sc2 >= 0)
      snprintf(ref2, sizeof ref2, "civ_%s_%s", c->classes[sc2].name, nm + 1);
    else
      snprintf(ref2, sizeof ref2, "%s->iv_%s", g_self, nm + 1);
    if (ivt2 == TY_POLY) {
      emit_indent(b, indent);
      buf_printf(b, "if (%ssp_poly_truthy(%s)) %s = ", is_or ? "!" : "", ref2, ref2);
      emit_boxed(c, v, b); buf_puts(b, ";\n");
    }
    else if (ivt2 == TY_BOOL) {
      emit_indent(b, indent);
      buf_printf(b, "if (%s%s) %s = ", is_or ? "!" : "", ref2, ref2);
      emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    else if (!is_or) {
      emit_indent(b, indent);
      buf_printf(b, "%s = ", ref2); emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    return;
  }
  if (!strcmp(ty, "CallOrWriteNode") || !strcmp(ty, "CallAndWriteNode")) {
    int is_or = !strcmp(ty, "CallOrWriteNode");
    int recv = nt_ref(nt, id, "receiver");
    const char *attr = nt_str(nt, id, "name");  /* attr/reader name */
    int v = nt_ref(nt, id, "value");
    if (recv < 0 || !attr) { unsupported(c, id, is_or ? "call-or-write" : "call-and-write"); return; }
    TyKind rt = comp_ntype(c, recv);
    if (!ty_is_object(rt)) { unsupported(c, id, is_or ? "call-or-write (non-object)" : "call-and-write (non-object)"); return; }
    int class_id = ty_object_class(rt);
    char ivn[300]; snprintf(ivn, sizeof ivn, "@%s", attr);
    int iidx = comp_ivar_index(&c->classes[class_id], ivn);
    TyKind ivt = iidx >= 0 ? c->classes[class_id].ivar_types[iidx] : TY_UNKNOWN;
    int tr = ++g_tmp;
    emit_indent(b, indent);
    buf_puts(b, "{ ");
    emit_ctype(c, rt, b); buf_printf(b, " _t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, "; ");
    if (ivt == TY_POLY) {
      buf_printf(b, "if (%ssp_poly_truthy(((sp_%s *)_t%d.v.p)->iv_%s)) ((sp_%s *)_t%d.v.p)->iv_%s = ",
                 is_or ? "!" : "", c->classes[class_id].name, tr, attr,
                 c->classes[class_id].name, tr, attr);
      emit_boxed(c, v, b); buf_puts(b, "; }\n");
    }
    else if (ivt == TY_BOOL) {
      buf_printf(b, "if (%s_t%d->iv_%s) _t%d->iv_%s = ", is_or ? "!" : "", tr, attr, tr, attr);
      emit_expr(c, v, b); buf_puts(b, "; }\n");
    }
    else if (!is_or) {  /* &&= on always-truthy type: always assign */
      buf_printf(b, "_t%d->iv_%s = ", tr, attr); emit_expr(c, v, b); buf_puts(b, "; }\n");
    }
    else { buf_puts(b, "}\n"); }  /* ||= on always-truthy type: no-op, but receiver evaluated */
    return;
  }
  if (!strcmp(ty, "InstanceVariableWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    Scope *cws = comp_scope_of(c, id);
    /* Ivar write in a class/module body (outside any def): write to the
       module-level civ_ variable. */
    if (cws && cws->class_id < 0 && !cws->is_cmethod && g_class_body_id >= 0) {
      emit_indent(b, indent);
      buf_printf(b, "civ_%s_%s = ", c->classes[g_class_body_id].name, nm + 1);
    }
    /* True top-level (outside any class or method): skip. */
    else if (!cws || (cws->class_id < 0 && !cws->is_cmethod && !cws->yields)) { return; }
    else {
      emit_indent(b, indent);
      if (cws && cws->is_cmethod && cws->class_id >= 0)
        buf_printf(b, "civ_%s_%s = ", c->classes[cws->class_id].name, nm + 1);
      else
        buf_printf(b, "%s->iv_%s = ", g_self, nm + 1);
    }
    const char *vty = nt_type(nt, v);
    int sc = cws ? cws->class_id : -1;
    if (sc < 0 && g_class_body_id >= 0) sc = g_class_body_id;
    TyKind ivt = TY_INT;
    if (sc >= 0) { int iv = comp_ivar_index(&c->classes[sc], nm); if (iv >= 0) ivt = c->classes[sc].ivar_types[iv]; }
    int ven = 0;
    int v_empty_array = vty && !strcmp(vty, "ArrayNode") && (nt_arr(nt, v, "elements", &ven), ven == 0);
    int v_empty_hash = 0;
    if (!v_empty_array && vty) {
      int hen = 0;
      if (!strcmp(vty, "HashNode") || !strcmp(vty, "KeywordHashNode"))
        v_empty_hash = (nt_arr(nt, v, "elements", &hen), hen == 0);
    }
    if (vty && !strcmp(vty, "NilNode")) {
      if (ivt == TY_RANGE) buf_puts(b, "(sp_Range){0}");
      else if (ivt == TY_POLY) buf_puts(b, "sp_box_nil()");
      else buf_puts(b, default_value(ivt));
    }
    else if (v_empty_array && ivt == TY_POLY_ARRAY) buf_puts(b, "sp_PolyArray_new()");
    else if (v_empty_array && array_kind(ivt)) buf_printf(b, "sp_%sArray_new()", array_kind(ivt));
    else if (v_empty_hash && ty_is_hash(ivt)) {
      const char *hcn = ty_hash_cname(ivt);
      if (hcn) buf_printf(b, "sp_%sHash_new()", hcn);
      else emit_expr(c, v, b);
    }
    else if (ivt == TY_POLY && comp_ntype(c, v) != TY_POLY) {
      /* a poly ivar slot needs a boxed RHS */
      emit_boxed(c, v, b);
    }
    else {
      emit_expr(c, v, b);
    }
    buf_puts(b, ";\n");
    return;
  }
  if (!strcmp(ty, "ClassVariableWriteNode")) {
    const char *nm = nt_str(nt, id, "name");  /* "@@x" */
    int v = nt_ref(nt, id, "value");
    int sc = comp_scope_of(c, id)->class_id;
    if (sc < 0) sc = g_class_body_id;
    if (sc < 0) { unsupported(c, id, "class variable write (no class scope)"); return; }
    TyKind ct = TY_INT;
    int idx = comp_cvar_index(&c->classes[sc], nm);
    if (idx >= 0) ct = c->classes[sc].cvar_types[idx];
    emit_indent(b, indent);
    buf_printf(b, "cvar_%s_%s = ", c->classes[sc].name, nm + 2);
    if (ct == TY_POLY) emit_boxed(c, v, b); else emit_expr(c, v, b);
    buf_puts(b, ";\n");
    return;
  }
  if (!strcmp(ty, "ClassVariableOperatorWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    const char *op = nt_str(nt, id, "binary_operator");
    int v = nt_ref(nt, id, "value");
    int sc = comp_scope_of(c, id)->class_id;
    if (sc < 0) sc = g_class_body_id;
    if (sc < 0) { unsupported(c, id, "class variable op-write (no class scope)"); return; }
    TyKind ct = TY_INT;
    int idx = comp_cvar_index(&c->classes[sc], nm);
    if (idx >= 0) ct = c->classes[sc].cvar_types[idx];
    char ref[300]; snprintf(ref, sizeof ref, "cvar_%s_%s", c->classes[sc].name, nm + 2);
    emit_indent(b, indent);
    if (ct == TY_STRING && op && !strcmp(op, "+")) {
      buf_printf(b, "%s = sp_str_concat(%s, ", ref, ref);
      emit_expr(c, v, b); buf_puts(b, ");\n");
    }
    else {
      buf_printf(b, "%s %s= ", ref, op ? op : "+");
      emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    return;
  }
  if (!strcmp(ty, "ClassVariableOrWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    int sc = comp_scope_of(c, id)->class_id;
    if (sc < 0) sc = g_class_body_id;
    if (sc < 0) { unsupported(c, id, "class variable or-write (no class scope)"); return; }
    char ref[300]; snprintf(ref, sizeof ref, "cvar_%s_%s", c->classes[sc].name, nm + 2);
    emit_indent(b, indent);
    buf_printf(b, "if (!(%s)) { %s = ", ref, ref); emit_expr(c, v, b);
    buf_puts(b, "; }\n");
    return;
  }
  if (!strcmp(ty, "ClassVariableAndWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    int sc = comp_scope_of(c, id)->class_id;
    if (sc < 0) sc = g_class_body_id;
    if (sc < 0) { unsupported(c, id, "class variable and-write (no class scope)"); return; }
    char ref[300]; snprintf(ref, sizeof ref, "cvar_%s_%s", c->classes[sc].name, nm + 2);
    emit_indent(b, indent);
    buf_printf(b, "if (%s) { %s = ", ref, ref); emit_expr(c, v, b);
    buf_puts(b, "; }\n");
    return;
  }
  if (!strcmp(ty, "InstanceVariableOperatorWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    const char *op = nt_str(nt, id, "binary_operator");
    int sc = comp_scope_of(c, id)->class_id;
    TyKind vt = TY_UNKNOWN;
    if (sc >= 0) { int iv = comp_ivar_index(&c->classes[sc], nm); if (iv >= 0) vt = c->classes[sc].ivar_types[iv]; }
    char ref[300];
    Scope *cs = comp_scope_of(c, id);
    if (cs && cs->is_cmethod && cs->class_id >= 0)
      snprintf(ref, sizeof ref, "civ_%s_%s", c->classes[cs->class_id].name, nm + 1);
    else
      snprintf(ref, sizeof ref, "%s->iv_%s", g_self, nm + 1);
    emit_indent(b, indent);
    if (vt == TY_STRING && op && !strcmp(op, "+")) {
      buf_printf(b, "%s = sp_str_concat(%s, ", ref, ref);
      emit_expr(c, nt_ref(nt, id, "value"), b); buf_puts(b, ");\n");
    }
    else if (op && ty_is_object(vt)) {
      int idefcls = -1;
      int icid = ty_object_class(vt);
      int imi = comp_method_in_chain(c, icid, op, &idefcls);
      if (imi >= 0) {
        Scope *ims = &c->scopes[imi];
        LocalVar *ip = ims->nparams >= 1 ? scope_local(ims, ims->pnames[0]) : NULL;
        int iatmp = ++g_tmp;
        int ival = nt_ref(nt, id, "value");
        emit_indent(g_pre, g_indent);
        emit_ctype(c, ip ? ip->type : comp_ntype(c, ival), g_pre);
        buf_printf(g_pre, " _t%d = ", iatmp);
        emit_expr(c, ival, g_pre);
        buf_puts(g_pre, ";\n");
        buf_printf(b, "%s = sp_%s_%s((sp_%s *)%s, _t%d);\n",
                   ref, c->classes[idefcls].name, mc(ims->name),
                   c->classes[idefcls].name, ref, iatmp);
      }
      else {
        buf_printf(b, "%s %s= ", ref, op);
        emit_expr(c, nt_ref(nt, id, "value"), b); buf_puts(b, ";\n");
      }
    }
    else {
      buf_printf(b, "%s %s= ", ref, op ? op : "+");
      emit_expr(c, nt_ref(nt, id, "value"), b); buf_puts(b, ";\n");
    }
    return;
  }
  if (!strcmp(ty, "GlobalVariableWriteNode") || !strcmp(ty, "ConstantWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int isg = ty[0] == 'G';
    const char *pfx = isg ? "gv" : "cst";
    const char *raw_key = isg ? nm + 1 : nm;
    const char *key = isg ? comp_resolve_gvar(c, raw_key) : raw_key;
    LocalVar *lv = isg ? comp_gvar(c, key) : comp_const(c, key);
    if (!lv) { /* not registered (non-ident name or class const) -> ignore */ return; }
    int v = nt_ref(nt, id, "value");
    if (!isg && lv->init_guarded) {
      /* flag the const as in-progress while its Class.new runs, so a
         self-referential read inside initialize raises NameError */
      emit_indent(b, indent); buf_printf(b, "sp_init_in_progress_%s = 1;\n", key);
    }
    emit_indent(b, indent);
    buf_printf(b, "%s_%s = ", pfx, key);
    const char *vty = nt_type(nt, v);
    if (vty && !strcmp(vty, "NilNode")) buf_puts(b, lv->type == TY_RANGE ? "(sp_Range){0}" : default_value(lv->type));
    else emit_expr(c, v, b);
    buf_puts(b, ";\n");
    if (!isg && lv->init_guarded) {
      emit_indent(b, indent); buf_printf(b, "sp_init_in_progress_%s = 0;\n", key);
    }
    return;
  }
  if (!strcmp(ty, "ConstantPathOperatorWriteNode")) {
    int tgt = nt_ref(nt, id, "target");
    const char *nm = tgt >= 0 ? nt_str(nt, tgt, "name") : NULL;
    LocalVar *cv = nm ? comp_const(c, nm) : NULL;
    if (!cv) { unsupported(c, id, "constant path operator write"); return; }
    const char *op = nt_str(nt, id, "binary_operator");
    int v = nt_ref(nt, id, "value");
    emit_indent(b, indent);
    if (cv->type == TY_STRING && op && !strcmp(op, "+")) {
      buf_printf(b, "cst_%s = sp_str_concat(cst_%s, ", nm, nm); emit_expr(c, v, b); buf_puts(b, ");\n");
    }
    else {
      buf_printf(b, "cst_%s %s= ", nm, op ? op : "+"); emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    return;
  }
  if (!strcmp(ty, "ConstantPathOrWriteNode") || !strcmp(ty, "ConstantPathAndWriteNode")) {
    int is_or = !strcmp(ty, "ConstantPathOrWriteNode");
    int tgt = nt_ref(nt, id, "target");
    const char *nm = tgt >= 0 ? nt_str(nt, tgt, "name") : NULL;
    LocalVar *cv = nm ? comp_const(c, nm) : NULL;
    if (!cv) { unsupported(c, id, "constant path or/and write"); return; }
    int v = nt_ref(nt, id, "value");
    if (cv->type == TY_POLY) {
      emit_indent(b, indent);
      buf_printf(b, "if (%ssp_poly_truthy(cst_%s)) cst_%s = ", is_or ? "!" : "", nm, nm);
      emit_boxed(c, v, b); buf_puts(b, ";\n");
    }
    else if (cv->type == TY_BOOL) {
      emit_indent(b, indent);
      buf_printf(b, "if (%scst_%s) cst_%s = ", is_or ? "!" : "", nm, nm); emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    else if (!is_or) {  /* &&= on an always-truthy constant: always assign */
      emit_indent(b, indent);
      buf_printf(b, "cst_%s = ", nm); emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    /* ||= on an always-truthy constant: no-op */
    return;
  }
  if (!strcmp(ty, "ConstantOperatorWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    LocalVar *cv = nm ? comp_const(c, nm) : NULL;
    if (!cv) return;
    const char *op = nt_str(nt, id, "binary_operator");
    int v = nt_ref(nt, id, "value");
    emit_indent(b, indent);
    if (cv->type == TY_STRING && op && !strcmp(op, "+")) {
      buf_printf(b, "cst_%s = sp_str_concat(cst_%s, ", nm, nm); emit_expr(c, v, b); buf_puts(b, ");\n");
    }
    else {
      buf_printf(b, "cst_%s %s= ", nm, op ? op : "+"); emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    return;
  }
  if (!strcmp(ty, "ConstantOrWriteNode") || !strcmp(ty, "ConstantAndWriteNode")) {
    int is_or = !strcmp(ty, "ConstantOrWriteNode");
    const char *nm = nt_str(nt, id, "name");
    LocalVar *cv = nm ? comp_const(c, nm) : NULL;
    if (!cv) return;
    int v = nt_ref(nt, id, "value");
    if (cv->type == TY_POLY) {
      emit_indent(b, indent);
      buf_printf(b, "if (%ssp_poly_truthy(cst_%s)) { cst_%s = ", is_or ? "!" : "", nm, nm);
      emit_boxed(c, v, b); buf_puts(b, "; }\n");
    }
    else if (cv->type == TY_BOOL) {
      emit_indent(b, indent);
      buf_printf(b, "if (%scst_%s) { cst_%s = ", is_or ? "!" : "", nm, nm); emit_expr(c, v, b); buf_puts(b, "; }\n");
    }
    else if (!is_or) {  /* &&= on an always-truthy constant: always assign */
      emit_indent(b, indent);
      buf_printf(b, "cst_%s = ", nm); emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    /* ||= on an always-truthy constant: no-op */
    return;
  }
  if (!strcmp(ty, "GlobalVariableOperatorWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
    LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
    if (!lv) return;
    const char *op = nt_str(nt, id, "binary_operator");
    int v = nt_ref(nt, id, "value");
    emit_indent(b, indent);
    if (lv->type == TY_STRING && op && !strcmp(op, "+")) {
      buf_printf(b, "gv_%s = sp_str_concat(gv_%s, ", rn, rn);
      emit_expr(c, v, b); buf_puts(b, ");\n");
    }
    else {
      buf_printf(b, "gv_%s %s= ", rn, op ? op : "+");
      emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    return;
  }
  if (!strcmp(ty, "GlobalVariableOrWriteNode") || !strcmp(ty, "GlobalVariableAndWriteNode")) {
    int is_or = !strcmp(ty, "GlobalVariableOrWriteNode");
    const char *nm = nt_str(nt, id, "name");
    const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
    LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
    if (!lv) return;
    int v = nt_ref(nt, id, "value");
    emit_indent(b, indent);
    buf_printf(b, "if (%sgv_%s) { gv_%s = ", is_or ? "!" : "", rn, rn);
    emit_expr(c, v, b);
    buf_puts(b, "; }\n");
    return;
  }
  if (!strcmp(ty, "MatchRequiredNode")) {
    /* `value => pattern`: destructure pattern into locals. */
    int value = nt_ref(nt, id, "value");
    int pattern = nt_ref(nt, id, "pattern");
    if (value < 0 || pattern < 0) return;
    const char *pty = nt_type(nt, pattern);
    if (!pty) return;
    if (!strcmp(pty, "ArrayPatternNode")) {
      int rn = 0;
      const int *reqs = nt_arr(nt, pattern, "requireds", &rn);
      TyKind vt = comp_ntype(c, value);
      const char *k = ty_is_array(vt) ? ((vt == TY_POLY_ARRAY) ? "Poly" : array_kind(vt)) : NULL;
      if (!k) k = "Int";
      int tarr = ++g_tmp;
      emit_indent(b, indent);
      emit_ctype(c, vt != TY_UNKNOWN ? vt : TY_INT_ARRAY, b);
      buf_printf(b, " _t%d = ", tarr); emit_expr(c, value, b); buf_puts(b, ";\n");
      emit_indent(b, indent);
      buf_printf(b, "SP_GC_ROOT(_t%d);\n", tarr);
      /* Length check: raise NoMatchingPatternError if sizes differ. */
      emit_indent(b, indent);
      buf_printf(b, "if (!_t%d || _t%d->len != %dLL) sp_raise_cls(\"NoMatchingPatternError\", \"[array pattern mismatch]\");\n", tarr, tarr, (long long)rn);
      for (int i = 0; i < rn; i++) {
        const char *lty2 = nt_type(nt, reqs[i]);
        if (!lty2 || strcmp(lty2, "LocalVariableTargetNode")) continue;
        const char *lnm = nt_str(nt, reqs[i], "name");
        if (!lnm) continue;
        emit_indent(b, indent);
        buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, %dLL);\n", lnm, k, tarr, (long long)i);
      }
    }
    else if (!strcmp(pty, "HashPatternNode")) {
      int pn = 0;
      const int *pelms = nt_arr(nt, pattern, "elements", &pn);
      /* Evaluate value hash into a temp. */
      TyKind vt = comp_ntype(c, value);
      const char *hn = ty_is_hash(vt) ? ty_hash_cname(vt) : NULL;
      int thash = ++g_tmp;
      emit_indent(b, indent);
      if (hn) { buf_printf(b, "sp_%sHash *_t%d = ", hn, thash); }
      else { buf_printf(b, "void *_t%d = (void *)", thash); }
      emit_expr(c, value, b); buf_puts(b, ";\n");
      for (int i = 0; i < pn; i++) {
        const char *ety = nt_type(nt, pelms[i]);
        if (!ety || strcmp(ety, "AssocNode")) continue;
        int pkey = nt_ref(nt, pelms[i], "key");
        int ptgt = nt_ref(nt, pelms[i], "value");
        if (ptgt < 0) continue;
        const char *tty = nt_type(nt, ptgt);
        if (!tty || strcmp(tty, "LocalVariableTargetNode")) continue;
        const char *lnm = nt_str(nt, ptgt, "name");
        if (!lnm || !hn) continue;
        emit_indent(b, indent);
        /* unbox TY_POLY hash value to the local's concrete type */
        TyKind hvt = ty_hash_val(vt);
        Scope *hpsc = comp_scope_of(c, id);
        LocalVar *hplv = hpsc ? scope_local(hpsc, lnm) : NULL;
        TyKind hpltype = hplv ? hplv->type : TY_UNKNOWN;
        if (hvt == TY_POLY && hpltype != TY_UNKNOWN && hpltype != TY_POLY) {
          int htmp = ++g_tmp;
          buf_printf(b, "{ sp_RbVal _t%d = sp_%sHash_get(_t%d, ", htmp, hn, thash);
          emit_expr(c, pkey, b); buf_puts(b, "); ");
          if (hpltype == TY_STRING) buf_printf(b, "lv_%s = _t%d.v.s; }\n", lnm, htmp);
          else if (hpltype == TY_INT) buf_printf(b, "lv_%s = _t%d.v.i; }\n", lnm, htmp);
          else if (hpltype == TY_FLOAT) buf_printf(b, "lv_%s = _t%d.v.f; }\n", lnm, htmp);
          else if (hpltype == TY_BOOL) buf_printf(b, "lv_%s = _t%d.v.b; }\n", lnm, htmp);
          else if (hpltype == TY_SYMBOL) buf_printf(b, "lv_%s = (sp_sym)_t%d.v.i; }\n", lnm, htmp);
          else { buf_puts(b, "}\n"); emit_indent(b, indent);
                 buf_printf(b, "lv_%s = sp_%sHash_get(_t%d, ", lnm, hn, thash);
                 emit_expr(c, pkey, b); buf_puts(b, ");\n"); }
        }
        else {
          buf_printf(b, "lv_%s = sp_%sHash_get(_t%d, ", lnm, hn, thash);
          emit_expr(c, pkey, b); buf_puts(b, ");\n");
        }
      }
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
    int rn = 0;
    const int *rights = nt_arr(nt, id, "rights", &rn);
    int rest_nid = nt_ref(nt, id, "rest");
    int rest_inner = -1;
    const char *rest_var = NULL;
    const char *rest_gvar = NULL;  /* global variable name (without $) for *$rest */
    if (rest_nid >= 0) {
      const char *rsty = nt_type(nt, rest_nid);
      if (rsty && !strcmp(rsty, "SplatNode"))
        rest_inner = nt_ref(nt, rest_nid, "expression");
      if (rest_inner >= 0 && nt_type(nt, rest_inner)) {
        if (!strcmp(nt_type(nt, rest_inner), "LocalVariableTargetNode"))
          rest_var = nt_str(nt, rest_inner, "name");
        else if (!strcmp(nt_type(nt, rest_inner), "GlobalVariableTargetNode")) {
          const char *gnm_r = nt_str(nt, rest_inner, "name");
          if (gnm_r) rest_gvar = comp_resolve_gvar(c, gnm_r + 1);
        }
      }
    }
    if (!els) {
      /* scalar RHS (`a, b = 1`): the first target takes the value, the rest
         their slot default (Ruby gives nil; we land the typed zero). A call /
         super / yield can return a multi-value tuple, so those are excluded
         and fall through to the tuple-destructuring path. */
      TyKind st = comp_ntype(c, value);
      int multi_src = vty && (!strcmp(vty, "CallNode") || !strcmp(vty, "SuperNode") ||
                              !strcmp(vty, "ForwardingSuperNode") || !strcmp(vty, "YieldNode"));
      if (vty && !multi_src && !ty_is_array(st) && !ty_is_hash(st) && st != TY_UNKNOWN) {
        for (int i = 0; i < ln; i++) {
          const char *lty = nt_type(nt, lefts[i]);
          if (!lty || strcmp(lty, "LocalVariableTargetNode")) continue;
          emit_indent(b, indent);
          buf_printf(b, "lv_%s = ", nt_str(nt, lefts[i], "name"));
          if (i == 0) emit_expr(c, value, b);
          else buf_puts(b, default_value(comp_ntype(c, lefts[i])));
          buf_puts(b, ";\n");
        }
        return;
      }
      /* any expression returning a typed array: runtime destructure */
      if (ty_is_array(st) && st != TY_UNKNOWN) {
        const char *k = (st == TY_POLY_ARRAY) ? "Poly" : array_kind(st);
        if (!k) k = "Int";
        TyKind elem = ty_array_elem(st);
        int tarr = ++g_tmp;
        emit_indent(b, indent);
        emit_ctype(c, st, b);
        buf_printf(b, " _t%d = ", tarr); emit_expr(c, value, b); buf_puts(b, ";\n");
        emit_indent(b, indent);
        buf_printf(b, "SP_GC_ROOT(_t%d);\n", tarr);
        Scope *rt_scope = comp_scope_of(c, id);
        for (int i = 0; i < ln; i++) {
          const char *lty = nt_type(nt, lefts[i]);
          if (!lty) continue;
          if (!strcmp(lty, "LocalVariableTargetNode")) {
            emit_indent(b, indent);
            buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, %dLL);\n",
                       nt_str(nt, lefts[i], "name"), k, tarr, i);
          }
          else if (!strcmp(lty, "InstanceVariableTargetNode") &&
                   rt_scope && rt_scope->class_id >= 0) {
            const char *ivnm = nt_str(nt, lefts[i], "name");
            if (!ivnm) continue;
            emit_indent(b, indent);
            char get_expr[64]; snprintf(get_expr, sizeof get_expr, "sp_%sArray_get(_t%d, %dLL)", k, tarr, i);
            TyKind ivt = TY_UNKNOWN;
            int iv_rt = comp_ivar_index(&c->classes[rt_scope->class_id], ivnm);
            if (iv_rt >= 0) ivt = c->classes[rt_scope->class_id].ivar_types[iv_rt];
            if (rt_scope->is_cmethod)
              buf_printf(b, "civ_%s_%s = ", c->classes[rt_scope->class_id].name, ivnm + 1);
            else
              buf_printf(b, "%s->iv_%s = ", g_self, ivnm + 1);
            if (ivt == TY_POLY && elem != TY_POLY) {
              Buf bx; memset(&bx, 0, sizeof bx);
              emit_boxed_text(c, elem, get_expr, &bx);
              buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
            }
            else buf_puts(b, get_expr);
            buf_puts(b, ";\n");
          }
          else if ((!strcmp(lty, "ConstantTargetNode") || !strcmp(lty, "ConstantPathTargetNode"))) {
            const char *cnm_rt = nt_str(nt, lefts[i], "name");
            if (!cnm_rt || !comp_const(c, cnm_rt)) continue;
            emit_indent(b, indent);
            buf_printf(b, "cst_%s = sp_%sArray_get(_t%d, %dLL);\n", cnm_rt, k, tarr, i);
          }
        }
        if (rest_var) {
          Scope *rscope = comp_scope_of(c, id);
          LocalVar *rlv = scope_local(rscope, rest_var);
          TyKind rest_arr_t = rlv ? rlv->type : st;
          if (!ty_is_array(rest_arr_t)) rest_arr_t = st;
          const char *rk = (rest_arr_t == TY_POLY_ARRAY) ? "Poly" : array_kind(rest_arr_t);
          if (!rk) rk = k;
          int tr = ++g_tmp;
          emit_indent(b, indent);
          buf_printf(b, "sp_%sArray *_t%d = sp_%sArray_slice(_t%d, %dLL, _t%d->len - %dLL - %dLL);\n",
                     rk, tr, rk, tarr, ln, tarr, ln, rn);
          emit_indent(b, indent);
          buf_printf(b, "SP_GC_ROOT(_t%d);\n", tr);
          emit_indent(b, indent);
          buf_printf(b, "lv_%s = _t%d;\n", rest_var, tr);
        }
        for (int j = 0; j < rn; j++) {
          const char *lty = nt_type(nt, rights[j]);
          if (!lty) continue;
          if (!strcmp(lty, "LocalVariableTargetNode")) {
            emit_indent(b, indent);
            buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d->len - %dLL + %dLL);\n",
                       nt_str(nt, rights[j], "name"), k, tarr, tarr, rn, j);
          }
          else if (!strcmp(lty, "InstanceVariableTargetNode") &&
                   rt_scope && rt_scope->class_id >= 0) {
            const char *ivnm2 = nt_str(nt, rights[j], "name");
            if (!ivnm2) continue;
            emit_indent(b, indent);
            char get_expr2[80];
            snprintf(get_expr2, sizeof get_expr2, "sp_%sArray_get(_t%d, _t%d->len - %dLL + %dLL)", k, tarr, tarr, rn, j);
            TyKind ivt2 = TY_UNKNOWN;
            int iv_rt2 = comp_ivar_index(&c->classes[rt_scope->class_id], ivnm2);
            if (iv_rt2 >= 0) ivt2 = c->classes[rt_scope->class_id].ivar_types[iv_rt2];
            if (rt_scope->is_cmethod)
              buf_printf(b, "civ_%s_%s = ", c->classes[rt_scope->class_id].name, ivnm2 + 1);
            else
              buf_printf(b, "%s->iv_%s = ", g_self, ivnm2 + 1);
            if (ivt2 == TY_POLY && elem != TY_POLY) {
              Buf bx2; memset(&bx2, 0, sizeof bx2);
              emit_boxed_text(c, elem, get_expr2, &bx2);
              buf_puts(b, bx2.p ? bx2.p : "sp_box_nil()"); free(bx2.p);
            }
            else buf_puts(b, get_expr2);
            buf_puts(b, ";\n");
          }
        }
        return;
      }
      unsupported(c, id, "multiple assignment");
    }
    if (rest_nid < 0 && en < ln + rn) { unsupported(c, id, "multiple assignment"); return; }
    /* evaluate all RHS values into temps first (so `a, b = b, a` swaps).
       Save each temp index separately: emit_expr may consume extra g_tmp
       slots via preludes (e.g. array literals), so base+i is unreliable. */
    int *tmps = en > 0 ? alloca(sizeof(int) * (size_t)en) : NULL;
    for (int i = 0; i < en; i++) {
      tmps[i] = ++g_tmp;
      Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, els[i], &vb);
      emit_indent(b, indent);
      emit_ctype(c, comp_ntype(c, els[i]), b);
      buf_printf(b, " _t%d = ", tmps[i]);
      buf_puts(b, vb.p ? vb.p : ""); buf_puts(b, ";\n"); free(vb.p);
    }
    /* assign lefts */
    for (int i = 0; i < ln; i++) {
      const char *lty = nt_type(nt, lefts[i]);
      if (i >= en) {
        if (lty && !strcmp(lty, "LocalVariableTargetNode")) {
          emit_indent(b, indent);
          buf_printf(b, "lv_%s = %s;\n", nt_str(nt, lefts[i], "name"),
                     default_value(comp_ntype(c, lefts[i])));
        }
        continue;
      }
      if (lty && !strcmp(lty, "LocalVariableTargetNode")) {
        emit_indent(b, indent);
        buf_printf(b, "lv_%s = _t%d;\n", nt_str(nt, lefts[i], "name"), tmps[i]);
      }
      else if (lty && (!strcmp(lty, "ConstantPathTargetNode") || !strcmp(lty, "ConstantTargetNode")) &&
               nt_str(nt, lefts[i], "name") && comp_const(c, nt_str(nt, lefts[i], "name"))) {
        emit_indent(b, indent);
        buf_printf(b, "cst_%s = _t%d;\n", nt_str(nt, lefts[i], "name"), tmps[i]);
      }
      else if (lty && !strcmp(lty, "InstanceVariableTargetNode")) {
        const char *ivnm = nt_str(nt, lefts[i], "name");
        if (!ivnm) continue;
        Scope *iv_sc = comp_scope_of(c, id);
        int iv_cid = iv_sc ? iv_sc->class_id : -1;
        if (iv_cid < 0 && g_class_body_id >= 0) iv_cid = g_class_body_id;
        TyKind ivt = TY_UNKNOWN;
        if (iv_cid >= 0) {
          int iv_idx = comp_ivar_index(&c->classes[iv_cid], ivnm);
          if (iv_idx >= 0) ivt = c->classes[iv_cid].ivar_types[iv_idx];
        }
        emit_indent(b, indent);
        if (iv_sc && iv_sc->is_cmethod && iv_cid >= 0)
          buf_printf(b, "civ_%s_%s = ", c->classes[iv_cid].name, ivnm + 1);
        else
          buf_printf(b, "%s->iv_%s = ", g_self, ivnm + 1);
        TyKind valt = comp_ntype(c, els[i]);
        if (ivt == TY_POLY && valt != TY_POLY) {
          char expr[32]; snprintf(expr, sizeof expr, "_t%d", tmps[i]);
          Buf bx; memset(&bx, 0, sizeof bx);
          emit_boxed_text(c, valt, expr, &bx);
          buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
        }
        else buf_printf(b, "_t%d", tmps[i]);
        buf_puts(b, ";\n");
      }
      else if (lty && !strcmp(lty, "CallTargetNode")) {
        /* setter call: e.g. @c.v = _t<i> */
        const char *setnm = nt_str(nt, lefts[i], "name");
        int recv_id2 = nt_ref(nt, lefts[i], "receiver");
        size_t snlen = setnm ? strlen(setnm) : 0;
        if (!setnm || snlen < 2 || setnm[snlen - 1] != '=' || recv_id2 < 0)
          { unsupported(c, id, "multiple assignment call target"); continue; }
        TyKind rt2 = comp_ntype(c, recv_id2);
        if (!ty_is_object(rt2))
          { unsupported(c, id, "multiple assignment call target non-object"); continue; }
        char base2[256]; memcpy(base2, setnm, snlen - 1); base2[snlen - 1] = '\0';
        int rc2 = ty_object_class(rt2);
        if (!comp_writer_in_chain(c, rc2, base2, NULL))
          { unsupported(c, id, "multiple assignment call target no writer"); continue; }
        char ivn2[260]; snprintf(ivn2, sizeof ivn2, "@%s", base2);
        int defc2 = -1; comp_writer_in_chain(c, rc2, base2, &defc2);
        int iv2 = comp_ivar_index(&c->classes[defc2 < 0 ? rc2 : defc2], ivn2);
        TyKind ivt2 = iv2 >= 0 ? c->classes[defc2 < 0 ? rc2 : defc2].ivar_types[iv2] : TY_UNKNOWN;
        emit_indent(b, indent);
        buf_puts(b, "("); emit_expr(c, recv_id2, b); buf_printf(b, ")->iv_%s = ", base2);
        TyKind valt2 = comp_ntype(c, els[i]);
        if (ivt2 == TY_POLY && valt2 != TY_POLY) {
          char expr2[32]; snprintf(expr2, sizeof expr2, "_t%d", tmps[i]);
          Buf bx2; memset(&bx2, 0, sizeof bx2);
          emit_boxed_text(c, valt2, expr2, &bx2);
          buf_puts(b, bx2.p ? bx2.p : "sp_box_nil()"); free(bx2.p);
        }
        else buf_printf(b, "_t%d", tmps[i]);
        buf_puts(b, ";\n");
      }
      else if (lty && !strcmp(lty, "MultiTargetNode")) {
        /* (b, c) = _t<i>  where _t<i> is a typed array */
        TyKind at = comp_ntype(c, els[i]);
        const char *k = array_kind(at);
        if (!k) { unsupported(c, id, "multiple assignment nested target"); continue; }
        int inn2 = 0;
        const int *inner_lefts = nt_arr(nt, lefts[i], "lefts", &inn2);
        for (int j = 0; j < inn2; j++) {
          const char *ilty2 = inner_lefts ? nt_type(nt, inner_lefts[j]) : NULL;
          if (!ilty2 || strcmp(ilty2, "LocalVariableTargetNode")) { unsupported(c, id, "multiple assignment nested target"); continue; }
          emit_indent(b, indent);
          buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, %d);\n",
                     nt_str(nt, inner_lefts[j], "name"), k, tmps[i], j);
        }
      }
      else if (lty && !strcmp(lty, "GlobalVariableTargetNode")) {
        const char *gnm = nt_str(nt, lefts[i], "name");
        const char *rn2 = gnm ? comp_resolve_gvar(c, gnm + 1) : NULL;
        if (!rn2 || !comp_gvar(c, rn2)) { unsupported(c, id, "multiple assignment global target"); continue; }
        emit_indent(b, indent);
        buf_printf(b, "gv_%s = _t%d;\n", rn2, tmps[i]);
      }
      else if (lty && !strcmp(lty, "IndexTargetNode")) {
        int recv_id = nt_ref(nt, lefts[i], "receiver");
        int idx_args = nt_ref(nt, lefts[i], "arguments");
        int idx_argc = 0;
        const int *idx_argv = idx_args >= 0 ? nt_arr(nt, idx_args, "arguments", &idx_argc) : NULL;
        if (recv_id < 0 || idx_argc < 1) { unsupported(c, id, "multiple assignment index target"); continue; }
        TyKind recv_t = comp_ntype(c, recv_id);
        emit_indent(b, indent);
        if (ty_is_array(recv_t)) {
          const char *k = (recv_t == TY_POLY_ARRAY) ? "Poly" : array_kind(recv_t);
          if (!k) k = "Int";
          buf_printf(b, "sp_%sArray_set(", k);
          emit_expr(c, recv_id, b); buf_puts(b, ", ");
          emit_expr(c, idx_argv[0], b); buf_puts(b, ", ");
          if (recv_t == TY_POLY_ARRAY) {
            TyKind valt = comp_ntype(c, els[i]);
            char tmp_expr[32]; snprintf(tmp_expr, sizeof tmp_expr, "_t%d", tmps[i]);
            Buf bxi; memset(&bxi, 0, sizeof bxi);
            emit_boxed_text(c, valt, tmp_expr, &bxi);
            buf_puts(b, bxi.p ? bxi.p : "sp_box_nil()"); free(bxi.p);
          }
          else buf_printf(b, "_t%d", tmps[i]);
          buf_puts(b, ");\n");
        }
        else if (ty_is_hash(recv_t)) {
          const char *hn = ty_hash_cname(recv_t);
          if (!hn) { unsupported(c, id, "multiple assignment hash index target unknown kind"); continue; }
          buf_printf(b, "sp_%sHash_set(", hn);
          emit_expr(c, recv_id, b); buf_puts(b, ", ");
          emit_expr(c, idx_argv[0], b); buf_puts(b, ", ");
          if (recv_t == TY_SYM_POLY_HASH || recv_t == TY_STR_POLY_HASH || recv_t == TY_POLY_POLY_HASH) {
            TyKind valt = comp_ntype(c, els[i]);
            char tmp_expr2[32]; snprintf(tmp_expr2, sizeof tmp_expr2, "_t%d", tmps[i]);
            Buf bxi2; memset(&bxi2, 0, sizeof bxi2);
            emit_boxed_text(c, valt, tmp_expr2, &bxi2);
            buf_puts(b, bxi2.p ? bxi2.p : "sp_box_nil()"); free(bxi2.p);
          }
          else buf_printf(b, "_t%d", tmps[i]);
          buf_puts(b, ");\n");
        }
        else { unsupported(c, id, "multiple assignment index target non-array/hash"); }
      }
      else if (lty && !strcmp(lty, "ClassVariableTargetNode")) {
        const char *cnm = nt_str(nt, lefts[i], "name");
        if (!cnm || cnm[0] != '@' || cnm[1] != '@') { unsupported(c, id, "multiple assignment class variable target"); continue; }
        Scope *cv_sc = comp_scope_of(c, id);
        int cv_cid = (cv_sc && cv_sc->class_id >= 0) ? cv_sc->class_id : g_class_body_id;
        if (cv_cid < 0) { unsupported(c, id, "multiple assignment class variable target no class"); continue; }
        if (comp_cvar_index(&c->classes[cv_cid], cnm) < 0) { unsupported(c, id, "multiple assignment class variable target unregistered"); continue; }
        emit_indent(b, indent);
        buf_printf(b, "cvar_%s_%s = _t%d;\n", c->classes[cv_cid].name, cnm + 2, tmps[i]);
      }
      else unsupported(c, id, "multiple assignment target");
    }
    /* build and assign rest (splat) target */
    if (rest_var) {
      int rstart = ln, rend = en - rn;
      if (rend < rstart) rend = rstart;
      Scope *rscope = comp_scope_of(c, id);
      LocalVar *rlv = scope_local(rscope, rest_var);
      TyKind rest_arr_t = rlv ? rlv->type : TY_INT_ARRAY;
      if (!ty_is_array(rest_arr_t)) rest_arr_t = TY_INT_ARRAY;
      const char *k = (rest_arr_t == TY_POLY_ARRAY) ? "Poly" : array_kind(rest_arr_t);
      if (!k) k = "Int";
      int tr = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);\n", k, tr, k, tr);
      if (rest_arr_t == TY_POLY_ARRAY) {
        for (int i = rstart; i < rend; i++) {
          TyKind et = comp_ntype(c, els[i]);
          char tmp_expr[32]; snprintf(tmp_expr, sizeof tmp_expr, "_t%d", tmps[i]);
          Buf bx; memset(&bx, 0, sizeof bx);
          emit_boxed_text(c, et, tmp_expr, &bx);
          emit_indent(b, indent);
          buf_printf(b, "sp_PolyArray_push(_t%d, %s);\n", tr, bx.p ? bx.p : "sp_box_nil()");
          free(bx.p);
        }
      }
      else {
        for (int i = rstart; i < rend; i++) {
          emit_indent(b, indent);
          buf_printf(b, "sp_%sArray_push(_t%d, _t%d);\n", k, tr, tmps[i]);
        }
      }
      emit_indent(b, indent);
      buf_printf(b, "lv_%s = _t%d;\n", rest_var, tr);
    }
    if (rest_gvar && comp_gvar(c, rest_gvar)) {
      int rstart = ln, rend = en - rn;
      if (rend < rstart) rend = rstart;
      LocalVar *glv_r = comp_gvar(c, rest_gvar);
      TyKind rest_arr_t = glv_r ? glv_r->type : TY_INT_ARRAY;
      if (!ty_is_array(rest_arr_t)) rest_arr_t = TY_INT_ARRAY;
      const char *k = (rest_arr_t == TY_POLY_ARRAY) ? "Poly" : array_kind(rest_arr_t);
      if (!k) k = "Int";
      int tr = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);\n", k, tr, k, tr);
      for (int i = rstart; i < rend; i++) {
        emit_indent(b, indent);
        buf_printf(b, "sp_%sArray_push(_t%d, _t%d);\n", k, tr, tmps[i]);
      }
      emit_indent(b, indent);
      buf_printf(b, "gv_%s = _t%d;\n", rest_gvar, tr);
    }
    /* assign rights (post-splat fixed targets) */
    for (int j = 0; j < rn; j++) {
      int ridx = en - rn + j;
      const char *lty = nt_type(nt, rights[j]);
      if (!lty) continue;
      const char *rnm_j = nt_str(nt, rights[j], "name");
      if (!strcmp(lty, "LocalVariableTargetNode")) {
        emit_indent(b, indent);
        if (ridx >= 0 && ridx < en) {
          buf_printf(b, "lv_%s = _t%d;\n", rnm_j, tmps[ridx]);
        }
        else {
          buf_printf(b, "lv_%s = %s;\n", rnm_j, default_value(comp_ntype(c, rights[j])));
        }
      }
      else if ((!strcmp(lty, "ConstantPathTargetNode") || !strcmp(lty, "ConstantTargetNode")) &&
               rnm_j && comp_const(c, rnm_j)) {
        emit_indent(b, indent);
        if (ridx >= 0 && ridx < en) {
          buf_printf(b, "cst_%s = _t%d;\n", rnm_j, tmps[ridx]);
        }
      }
      else if (!strcmp(lty, "InstanceVariableTargetNode") && rnm_j) {
        Scope *iv_sc2 = comp_scope_of(c, id);
        int iv_cid2 = iv_sc2 ? iv_sc2->class_id : -1;
        TyKind ivt2 = TY_UNKNOWN;
        if (iv_cid2 >= 0) {
          int iv_idx2 = comp_ivar_index(&c->classes[iv_cid2], rnm_j);
          if (iv_idx2 >= 0) ivt2 = c->classes[iv_cid2].ivar_types[iv_idx2];
        }
        emit_indent(b, indent);
        if (iv_sc2 && iv_sc2->is_cmethod && iv_cid2 >= 0)
          buf_printf(b, "civ_%s_%s = ", c->classes[iv_cid2].name, rnm_j + 1);
        else
          buf_printf(b, "%s->iv_%s = ", g_self, rnm_j + 1);
        if (ridx >= 0 && ridx < en) {
          TyKind valt2 = (ridx < en) ? comp_ntype(c, els[ridx]) : TY_UNKNOWN;
          if (ivt2 == TY_POLY && valt2 != TY_POLY) {
            char expr2[32]; snprintf(expr2, sizeof expr2, "_t%d", tmps[ridx]);
            Buf bx2; memset(&bx2, 0, sizeof bx2);
            emit_boxed_text(c, valt2, expr2, &bx2);
            buf_puts(b, bx2.p ? bx2.p : "sp_box_nil()"); free(bx2.p);
          }
          else buf_printf(b, "_t%d", tmps[ridx]);
        }
        else buf_puts(b, default_value(ivt2 != TY_UNKNOWN ? ivt2 : TY_INT));
        buf_puts(b, ";\n");
      }
    }
    return;
  }
  if (!strcmp(ty, "ClassNode") || !strcmp(ty, "ModuleNode")) {
    /* Run the body's side-effecting statements at the definition site
       (top-to-bottom, like CRuby). Method/attr/alias declarations are
       handled elsewhere; everything else (puts, constant writes, nested
       class/module bodies) executes inline here. */
    int cp = nt_ref(nt, id, "constant_path");
    const char *cname = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    int saved_cbi = g_class_body_id;
    if (cname) g_class_body_id = comp_class_index(c, cname);
    int body = nt_ref(nt, id, "body");
    int n = 0;
    const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    for (int k = 0; k < n; k++) {
      const char *sty = nt_type(nt, stmts[k]);
      if (!sty) continue;
      if (!strcmp(sty, "DefNode") || !strcmp(sty, "AliasMethodNode")) continue;
      /* A receiver-less call in a class body is, by default, a declaration
         macro (attr_*, include, private, an FFI/DSL directive) -- skip it.
         Only run the genuine side-effecting ones: output calls and calls
         that resolve to a user-defined method. */
      if (!strcmp(sty, "CallNode") && nt_ref(nt, stmts[k], "receiver") < 0) {
        const char *cn = nt_str(nt, stmts[k], "name");
        int is_output = cn && (!strcmp(cn, "puts") || !strcmp(cn, "print") || !strcmp(cn, "p"));
        int is_user = cn && comp_method_index(c, cn) >= 0;
        if (!is_output && !is_user) continue;
      }
      emit_stmt(c, stmts[k], b, indent);
    }
    g_class_body_id = saved_cbi;
    return;
  }
  if (!strcmp(ty, "SuperNode") || !strcmp(ty, "ForwardingSuperNode")) {
    if (!emit_super_inline(c, id, b, indent, 0)) {
      emit_indent(b, indent); emit_super(c, id, b); buf_puts(b, ";\n");
    }
    return;
  }
  if (!strcmp(ty, "IndexOperatorWriteNode")) { emit_index_op_write(c, id, b, indent); return; }
  if (!strcmp(ty, "IndexAndWriteNode")) { emit_index_and_or_write(c, id, b, indent, 0); return; }
  if (!strcmp(ty, "IndexOrWriteNode"))  { emit_index_and_or_write(c, id, b, indent, 1); return; }
  if (!strcmp(ty, "IfNode"))     { emit_if(c, id, b, indent, 0, 0); return; }
  if (!strcmp(ty, "UnlessNode")) { emit_if(c, id, b, indent, 1, 0); return; }
  if (!strcmp(ty, "WhileNode"))  { emit_while(c, id, b, indent, 0); return; }
  if (!strcmp(ty, "UntilNode"))  { emit_while(c, id, b, indent, 1); return; }
  if (!strcmp(ty, "ForNode"))    { emit_for(c, id, b, indent); return; }
  if (!strcmp(ty, "BreakNode"))  { emit_indent(b, indent); buf_puts(b, "break;\n"); return; }
  if (!strcmp(ty, "NextNode"))   { emit_indent(b, indent); buf_puts(b, "continue;\n"); return; }
  if (!strcmp(ty, "RedoNode"))   { emit_indent(b, indent); buf_puts(b, "continue;\n"); return; }
  if (!strcmp(ty, "CaseNode"))   { emit_case(c, id, b, indent); return; }
  if (!strcmp(ty, "BeginNode"))  { emit_begin(c, id, b, indent, NULL); return; }
  if (!strcmp(ty, "RescueModifierNode")) {
    /* `expr rescue fallback` as a statement: run expr under a setjmp guard,
       fall through to the rescue expression on any exception. */
    int e = nt_ref(nt, id, "expression");
    int r = nt_ref(nt, id, "rescue_expression");
    emit_indent(b, indent); buf_puts(b, "sp_exc_top++;\n");
    emit_indent(b, indent); buf_puts(b, "if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {\n");
    if (e >= 0) emit_stmt(c, e, b, indent + 1);
    emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "else {\n");
    emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
    if (r >= 0) emit_stmt(c, r, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return;
  }
  if (!strcmp(ty, "ReturnNode")) { emit_return(c, id, b, indent); return; }
  if (!strcmp(ty, "DefNode"))    { return; } /* emitted separately */
  if (!strcmp(ty, "AliasGlobalVariableNode")) { return; } /* resolved at scan time */
  if (!strcmp(ty, "PreExecutionNode") || !strcmp(ty, "PostExecutionNode")) { return; } /* hoisted separately */

  /* any remaining value expression as a bare statement (its value is used
     only when this is the last statement of an inlined expr method) */
  emit_indent(b, indent);
  emit_expr(c, id, b);
  buf_puts(b, ";\n");
}

/* Tail position: the value of this statement is the method's return value. */
static void emit_stmt_tail_inner(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) unsupported(c, id, "tail statement (no type)");

  if (!strcmp(ty, "IfNode"))     { emit_if(c, id, b, indent, 0, 1); return; }
  if (!strcmp(ty, "UnlessNode")) { emit_if(c, id, b, indent, 1, 1); return; }
  if (!strcmp(ty, "ReturnNode")) { emit_return(c, id, b, indent); return; }
  /* `raise` diverges -- no value to return; emit as a plain statement. */
  if (!strcmp(ty, "CallNode") && nt_ref(nt, id, "receiver") < 0 &&
      nt_str(nt, id, "name") && !strcmp(nt_str(nt, id, "name"), "raise")) {
    emit_indent(b, indent); emit_expr(c, id, b); buf_puts(b, ";\n");
    return;
  }
  if (!strcmp(ty, "BeginNode")) {
    /* begin/rescue value -> a temp, assigned in both branches, then tail */
    TyKind rt = comp_ntype(c, id);
    if (is_scalar_ret(rt)) {
      int t = ++g_tmp;
      char rv[32]; snprintf(rv, sizeof rv, "_t%d", t);
      emit_indent(b, indent); emit_ctype(c, rt, b);
      buf_printf(b, " _t%d = %s;\n", t, rt == TY_RANGE ? "(sp_Range){0}" : default_value(rt));
      int sp = g_result_poly; g_result_poly = (rt == TY_POLY);
      emit_begin(c, id, b, indent, rv);
      g_result_poly = sp;
      emit_indent(b, indent); emit_tail_lead(b); buf_printf(b, "_t%d;\n", t);
      return;
    }
    /* Non-scalar (e.g. TY_VOID when body diverges with raise or return):
       emit as a plain statement; any `return` inside uses deferred mechanism. */
    emit_begin(c, id, b, indent, NULL);
    return;
  }

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

  /* a value expression: return it (or assign to the begin/rescue result) */
  emit_indent(b, indent);
  emit_tail_lead(b);
  int want_poly = g_result_var ? g_result_poly : (g_ret_type == TY_POLY);
  if (want_poly && comp_ntype(c, id) != TY_POLY) emit_boxed(c, id, b);
  else emit_expr(c, id, b);
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
  }
  else {
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
  }
  else {
    emit_stmt_tail(c, id, b, indent);
  }
}

/* ---- declarations ---- */

/* Heap-managed types need a GC root for their local slot. */
static int needs_root(TyKind t) { return t == TY_STRING || ty_is_array(t) || ty_is_hash(t) || ty_is_object(t) || t == TY_EXCEPTION || t == TY_POLY || t == TY_PROC; }

/* Emit `node` boxed into an sp_RbVal. Idempotent: an already-poly value is
   passed through unboxed (double-boxing is a classic silent-corruption bug). */
/* Box a C-text expression `expr` of static type `t` into an sp_RbVal. */
static const char *hash_box_cls(TyKind t) {
  switch (t) {
    case TY_STR_INT_HASH:   return "SP_BUILTIN_STR_INT_HASH";
    case TY_STR_STR_HASH:   return "SP_BUILTIN_STR_STR_HASH";
    case TY_INT_STR_HASH:   return "SP_BUILTIN_INT_STR_HASH";
    case TY_STR_POLY_HASH:  return "SP_BUILTIN_STR_POLY_HASH";
    case TY_SYM_POLY_HASH:  return "SP_BUILTIN_SYM_POLY_HASH";
    case TY_POLY_POLY_HASH: return "SP_BUILTIN_POLY_POLY_HASH";
    default:                return NULL;
  }
}

static void emit_boxed_text(Compiler *c, TyKind t, const char *expr, Buf *b) {
  if (t == TY_POLY) { buf_puts(b, expr); return; }
  if (t == TY_EXCEPTION) { buf_printf(b, "sp_box_obj(%s, SP_BUILTIN_EXCEPTION)", expr); return; }
  if (ty_is_object(t)) { buf_printf(b, "sp_box_obj(%s, %d)", expr, ty_object_class(t)); return; }
  if (ty_is_hash(t) && hash_box_cls(t)) { buf_printf(b, "sp_box_obj(%s, %s)", expr, hash_box_cls(t)); return; }
  const char *fn = NULL;
  switch (t) {
    case TY_INT: fn = "sp_box_int"; break;       case TY_FLOAT: fn = "sp_box_float"; break;
    case TY_STRING: fn = "sp_box_str"; break;     case TY_BOOL: fn = "sp_box_bool"; break;
    case TY_SYMBOL: fn = "sp_box_sym"; break;     case TY_RANGE: fn = "sp_box_range"; break;
    case TY_TIME: fn = "sp_box_time"; break;
    case TY_INT_ARRAY: fn = "sp_box_int_array"; break;
    case TY_FLOAT_ARRAY: fn = "sp_box_float_array"; break;
    case TY_STR_ARRAY: fn = "sp_box_str_array"; break;
    case TY_POLY_ARRAY: fn = "sp_box_poly_array"; break;
    case TY_NIL: buf_puts(b, "sp_box_nil()"); return;
    default: break;
  }
  if (fn) buf_printf(b, "%s(%s)", fn, expr);
  else buf_printf(b, "sp_box_int(%s)", expr);  /* fallback */
}

/* Emit `expr` (a poly value) unboxed to its concrete C representation. */
static void emit_unbox_text(Compiler *c, TyKind t, const char *expr, Buf *b) {
  if (t == TY_POLY) { buf_puts(b, expr); return; }
  switch (t) {
    case TY_INT:    buf_printf(b, "(%s).v.i", expr); return;
    case TY_FLOAT:  buf_printf(b, "(%s).v.f", expr); return;
    case TY_STRING: buf_printf(b, "(%s).v.s", expr); return;
    case TY_BOOL:   buf_printf(b, "(%s).v.b", expr); return;
    case TY_SYMBOL: buf_printf(b, "(sp_sym)(%s).v.i", expr); return;
    default: break;
  }
  if (ty_is_object(t)) { buf_printf(b, "(sp_%s *)(%s).v.p", c->classes[ty_object_class(t)].name, expr); return; }
  const char *cn = c_type_name(t);
  if (cn) buf_printf(b, "(%s)(%s).v.p", cn, expr);
  else buf_printf(b, "(%s).v.i", expr);
}

static void emit_boxed(Compiler *c, int node, Buf *b) {
  TyKind t = comp_ntype(c, node);
  if (t == TY_POLY) { emit_expr(c, node, b); return; }
  if (t == TY_EXCEPTION) {
    buf_printf(b, "sp_box_obj("); emit_expr(c, node, b); buf_puts(b, ", SP_BUILTIN_EXCEPTION)");
    return;
  }
  if (ty_is_object(t)) {
    buf_printf(b, "sp_box_obj(");
    emit_expr(c, node, b);
    buf_printf(b, ", %d)", ty_object_class(t));
    return;
  }
  if (ty_is_hash(t)) {
    const char *hid = hash_box_cls(t);
    if (hid) { buf_printf(b, "sp_box_obj("); emit_expr(c, node, b); buf_printf(b, ", %s)", hid); return; }
    unsupported(c, node, "boxing value into poly"); return;
  }
  /* regex values can appear in poly context (multi-typed local); box as nil */
  if (t == TY_REGEX) { buf_puts(b, "sp_box_nil()"); return; }
  /* an empty array literal [] has TY_UNKNOWN; box it as an empty IntArray */
  if (t == TY_UNKNOWN && nt_type(c->nt, node) && !strcmp(nt_type(c->nt, node), "ArrayNode")) {
    int _ne = 0; nt_arr(c->nt, node, "elements", &_ne);
    if (_ne == 0) { buf_puts(b, "sp_box_int_array(sp_IntArray_new())"); return; }
  }
  /* an empty hash literal {} has TY_UNKNOWN; box it as an empty PolyPolyHash */
  if (t == TY_UNKNOWN && nt_type(c->nt, node) && !strcmp(nt_type(c->nt, node), "HashNode")) {
    int _ne = 0; nt_arr(c->nt, node, "elements", &_ne);
    if (_ne == 0) { buf_puts(b, "sp_box_obj(sp_PolyPolyHash_new(), SP_BUILTIN_POLY_POLY_HASH)"); return; }
  }
  const char *fn = NULL;
  switch (t) {
    case TY_INT:    fn = "sp_box_int";   break;
    case TY_FLOAT:  fn = "sp_box_float"; break;
    case TY_STRING: fn = "sp_box_str";   break;
    case TY_BOOL:   fn = "sp_box_bool";  break;
    case TY_SYMBOL: fn = "sp_box_sym";   break;
    case TY_RANGE:  fn = "sp_box_range"; break;
    case TY_TIME:   fn = "sp_box_time";  break;
    case TY_INT_ARRAY:   fn = "sp_box_int_array";   break;
    case TY_FLOAT_ARRAY: fn = "sp_box_float_array"; break;
    case TY_STR_ARRAY:   fn = "sp_box_str_array";   break;
    case TY_POLY_ARRAY:  fn = "sp_box_poly_array";  break;
    case TY_NIL: {
      const char *nty = nt_type(c->nt, node);
      if (nty && !strcmp(nty, "NilNode")) { buf_puts(b, "sp_box_nil()"); return; }
      /* a nil-typed expression can still have side effects (e.g. a void-valued
         block call): evaluate it for effect, then yield nil */
      buf_puts(b, "("); emit_expr(c, node, b); buf_puts(b, ", sp_box_nil())");
      return;
    }
    default: break;
  }
  if (!fn) { unsupported(c, node, "boxing value into poly"); return; }
  buf_printf(b, "%s(", fn);
  emit_expr(c, node, b);
  buf_puts(b, ")");
}

/* `vol` makes the local volatile (required for locals live across a setjmp
   in a begin/rescue). Pointers need the volatile on the pointer itself
   (T * volatile), value types take a leading qualifier. */
static void declare_local(Compiler *c, Buf *b, LocalVar *lv, int vol) {
  TyKind t = lv->type;
  Buf cty; memset(&cty, 0, sizeof cty);
  const char *init = "0";
  int ptr = 0, root = needs_root(t);
  switch (t) {
    case TY_INT:    buf_puts(&cty, "mrb_int"); init = "0"; break;
    case TY_FLOAT:  buf_puts(&cty, "mrb_float"); init = "0.0"; break;
    case TY_BOOL:   buf_puts(&cty, "mrb_bool"); init = "0"; break;
    case TY_SYMBOL: buf_puts(&cty, "sp_sym"); init = "((sp_sym)-1)"; break;
    case TY_RANGE:  buf_puts(&cty, "sp_Range"); init = "{0}"; break;
    case TY_TIME:   buf_puts(&cty, "sp_Time"); init = "{0}"; break;
    case TY_STRING: buf_puts(&cty, "const char *"); init = "(&(\"\\xff\")[1])"; ptr = 1; break;
    case TY_POLY:   buf_puts(&cty, "sp_RbVal"); init = "sp_box_nil()"; break;
    default:
      if (is_scalar_ret(t) && t != TY_UNKNOWN) { emit_ctype(c, t, &cty); init = "NULL"; ptr = 1; }
      else {
        fprintf(stderr, "spinelc: local '%s' has unsupported type %s\n", lv->name, ty_name(t));
        exit(1);
      }
  }
  buf_puts(b, "    ");
  if (vol && !ptr) buf_puts(b, "volatile ");
  buf_puts(b, cty.p ? cty.p : "");
  if (vol && ptr) buf_puts(b, "volatile ");  /* cty ends with "* "; -> "* volatile " */
  buf_printf(b, " lv_%s = %s;\n", lv->name, init);
  if (t == TY_POLY) buf_printf(b, "    SP_GC_ROOT_RBVAL(lv_%s);\n", lv->name);
  else if (root) buf_printf(b, "    SP_GC_ROOT(lv_%s);\n", lv->name);
  free(cty.p);
}

/* Does scope index `si` contain a begin/rescue (so its locals need volatile)? */
static int scope_has_begin(Compiler *c, int si) {
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (ty && (!strcmp(ty, "BeginNode") || !strcmp(ty, "RescueNode")) && c->nscope[id] == si)
      return 1;
  }
  return 0;
}

/* Declare a scope's locals. Params are already C function parameters, so
   they only need a GC root; body locals get a full declaration. */
static void emit_scope_decls(Compiler *c, Scope *s, Buf *b) {
  int vol = scope_has_begin(c, (int)(s - c->scopes));
  for (int i = 0; i < s->nlocals; i++) {
    LocalVar *lv = &s->locals[i];
    if (s->blk_param && lv->name && !strcmp(lv->name, s->blk_param)) continue;  /* virtual &block slot */
    /* Captured-by-closure local: lives in a heap cell so the proc and this
       scope share storage. A param's incoming value is copied into the cell;
       a body local starts at 0. Int cells only (capture is int-restricted). */
    if (lv->is_cell) {
      if (lv->type != TY_INT && lv->type != TY_BOOL && lv->type != TY_UNKNOWN)
        unsupported(c, s->def_node, "closure capturing a non-integer variable (later slice)");
      buf_printf(b, "    mrb_int *_cell_%s = (mrb_int *)sp_gc_alloc(sizeof(mrb_int), NULL, NULL);\n", lv->name);
      buf_printf(b, "    SP_GC_ROOT(_cell_%s);\n", lv->name);
      if (lv->is_param) buf_printf(b, "    *_cell_%s = lv_%s;\n", lv->name, lv->name);
      else buf_printf(b, "    *_cell_%s = 0;\n", lv->name);
      continue;
    }
    if (lv->is_param) {
      if (needs_root(lv->type)) buf_printf(b, "    SP_GC_ROOT(lv_%s);\n", lv->name);
    }
    else {
      declare_local(c, b, lv, vol);
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
  if (s->class_id >= 0 && s->is_cmethod)
    buf_printf(b, "sp_%s_s_%s", c->classes[s->class_id].name, mc(s->name));
  else if (s->class_id >= 0)
    buf_printf(b, "sp_%s_%s", c->classes[s->class_id].name, mc(s->name));
  else
    buf_printf(b, "sp_%s", mc(s->name));
}

static void emit_method_signature(Compiler *c, Scope *s, Buf *b) {
  if (method_is_void(s)) buf_puts(b, "static void ");
  else { buf_puts(b, "static "); emit_ctype(c, s->ret, b); buf_puts(b, " "); }
  emit_method_cname(c, s, b);
  buf_puts(b, "(");
  int wrote = 0;
  if (s->class_id >= 0 && !s->is_cmethod) {
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
  TyKind saved_rt = g_ret_type;
  int saved_ed = g_ensure_depth; g_ensure_depth = 0;
  g_ret_type = method_is_void(s) ? TY_VOID : s->ret;
  if (method_is_void(s)) {
    emit_stmts(c, s->body, b, 1);
  }
  else {
    emit_stmts_tail(c, s->body, b, 1);
    buf_puts(b, "  return ");
    if (ty_is_object(s->ret)) { buf_puts(b, "NULL;\n"); /* unreachable default (object pointer) */ }
    else buf_printf(b, "%s;\n", default_value(s->ret));
  }
  g_ret_type = saved_rt; g_ensure_depth = saved_ed;
  buf_puts(b, "}\n");
}

/* ---- first-class Proc ---- */

/* Block bodies don't get their own Scope: a block's params and the locals it
   assigns live in the ENCLOSING scope (the inline model). To emit a proc body
   as a standalone function we therefore work over the body SUBTREE, not a
   scope: its bound names are the block params plus the locals it writes; a
   read of any other name is a captured/free variable. (NameSet + helpers are
   defined near the top, shared with the cell-capture machinery.) */

/* True if `id` starts a nested block/lambda whose locals belong to it, not to
   the proc we're walking -- recursion stops there. */
static int is_nested_block(const char *ty) {
  return ty && (!strcmp(ty, "BlockNode") || !strcmp(ty, "LambdaNode"));
}

/* Collect the local names WRITTEN in the proc body subtree (the proc's own
   locals), not descending into nested blocks. */
static void proc_collect_locals(Compiler *c, int id, NameSet *locals) {
  if (id < 0) return;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return;
  if (!strcmp(ty, "LocalVariableWriteNode") || !strcmp(ty, "LocalVariableTargetNode") ||
      !strcmp(ty, "LocalVariableOperatorWriteNode") || !strcmp(ty, "LocalVariableOrWriteNode") ||
      !strcmp(ty, "LocalVariableAndWriteNode"))
    nameset_add(locals, nt_str(c->nt, id, "name"));
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) {
    int ch = nt_ref_at(c->nt, id, i);
    if (ch >= 0 && !is_nested_block(nt_type(c->nt, ch))) proc_collect_locals(c, ch, locals);
  }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n);
    for (int k = 0; k < n; k++)
      if (ids[k] >= 0 && !is_nested_block(nt_type(c->nt, ids[k]))) proc_collect_locals(c, ids[k], locals);
  }
}

/* Collect all local names used (read or written) directly in the proc body,
   not crossing nested blocks. The caller classifies each as the proc's own
   param/local or a captured enclosing var (is_cell). */
static void proc_collect_used(Compiler *c, int id, NameSet *out) {
  if (id < 0) return;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return;
  if (!strcmp(ty, "LocalVariableReadNode") || !strcmp(ty, "LocalVariableWriteNode") ||
      !strcmp(ty, "LocalVariableTargetNode") || !strcmp(ty, "LocalVariableOperatorWriteNode") ||
      !strcmp(ty, "LocalVariableOrWriteNode") || !strcmp(ty, "LocalVariableAndWriteNode"))
    nameset_add(out, nt_str(c->nt, id, "name"));
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(c->nt, id, i); if (ch >= 0 && !is_nested_block(nt_type(c->nt, ch))) proc_collect_used(c, ch, out); }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n); for (int k = 0; k < n; k++) if (ids[k] >= 0 && !is_nested_block(nt_type(c->nt, ids[k]))) proc_collect_used(c, ids[k], out); }
}

/* The ParametersNode of a proc-creating node. A `->{}` LambdaNode carries it
   directly (`parameters`); a `proc {}` / `lambda {}` block nests it one level
   deeper (block -> BlockParametersNode -> ParametersNode). */
static int proc_params_node(Compiler *c, int create) {
  const char *ty = nt_type(c->nt, create);
  if (ty && !strcmp(ty, "LambdaNode")) return nt_ref(c->nt, create, "parameters");
  int block = nt_ref(c->nt, create, "block");
  if (block < 0) return -1;
  int bp = nt_ref(c->nt, block, "parameters");   /* BlockParametersNode */
  if (bp < 0) return -1;
  return nt_ref(c->nt, bp, "parameters");        /* ParametersNode */
}
static const char *proc_param_name(Compiler *c, int create, int idx) {
  int pn = proc_params_node(c, create);
  if (pn < 0) return NULL;
  int n = 0;
  const int *reqs = nt_arr(c->nt, pn, "requireds", &n);
  return idx < n ? nt_str(c->nt, reqs[idx], "name") : NULL;
}
/* The StatementsNode body of a proc-creating node. */
static int proc_body_node(Compiler *c, int create) {
  const char *ty = nt_type(c->nt, create);
  if (ty && !strcmp(ty, "LambdaNode")) return nt_ref(c->nt, create, "body");
  int block = nt_ref(c->nt, create, "block");
  return block >= 0 ? nt_ref(c->nt, block, "body") : -1;
}

/* Proc args + return ride the mrb_int slot of sp_proc_call. A value that fits
   an mrb_int directly (int/bool/symbol/nil) needs no conversion; a heap pointer
   (string/array/hash/object) is laundered through (mrb_int)(uintptr_t). Other
   shapes (float, poly, range, time) don't fit the slot and defer. */
static int proc_slot_is_direct(TyKind t) { return t == TY_INT || t == TY_BOOL || t == TY_SYMBOL || t == TY_NIL || t == TY_UNKNOWN; }
static int proc_slot_is_ptr(TyKind t) { return t == TY_STRING || ty_is_array(t) || ty_is_hash(t) || ty_is_object(t); }

/* Lower a `proc {}` / `lambda {}` / `Proc.new {}` / `->(){}` literal: emit a
   standalone `static mrb_int _proc_N(void *cap, mrb_int *args)` (sp_proc_call's
   ABI) into g_procs, and emit the boxing `sp_proc_new_meta(...)` value into `b`. */
static void emit_proc_literal(Compiler *c, int create, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *cty = nt_type(nt, create);
  int is_lambda_node = cty && !strcmp(cty, "LambdaNode");
  if (!is_lambda_node && nt_ref(nt, create, "block") < 0) { unsupported(c, create, "proc literal without a block"); return; }

  Scope *bs = comp_scope_of(c, create);  /* enclosing scope: holds params + locals */
  int body = proc_body_node(c, create);

  int arity = 0;
  while (proc_param_name(c, create, arity)) arity++;

  /* Classify the names used in the proc body: the proc's params, captured
     enclosing locals (marked is_cell by analyze), and the proc's own body
     locals. Captures populate the cap struct; body locals are declared inside
     the fn; params come from args[]. */
  NameSet params = {0}, used = {0}, locals = {0}, caps = {0};
  for (int k = 0; k < arity; k++) nameset_add(&params, proc_param_name(c, create, k));
  proc_collect_used(c, body, &used);
  proc_collect_locals(c, body, &locals);
  for (int u = 0; u < used.n; u++) {
    const char *nm = used.v[u];
    if (nameset_has(&params, nm)) continue;
    LocalVar *lv = scope_local(bs, nm);
    if (lv && lv->is_cell) {
      if (lv->type != TY_INT && lv->type != TY_BOOL && lv->type != TY_UNKNOWN) {
        free(params.v); free(used.v); free(locals.v); free(caps.v);
        unsupported(c, create, "proc capturing a non-integer variable (later slice)");
        return;
      }
      nameset_add(&caps, nm);
    }
    else if (!nameset_has(&locals, nm)) {
      /* read of an enclosing var that wasn't celled and isn't proc-local:
         no storage exists for it inside the fn -- defer rather than miscompile */
      free(params.v); free(used.v); free(locals.v); free(caps.v);
      unsupported(c, create, "proc referencing an uncaptured outer variable (later slice)");
      return;
    }
  }

  /* proc {} / Proc.new {} are procs; lambda {} and ->(){} are lambdas */
  const char *cn = nt_str(nt, create, "name");
  int is_lambda = is_lambda_node || (cn && !strcmp(cn, "lambda"));

  /* body return type = last statement's type */
  TyKind ret = TY_NIL;
  { int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    if (bn > 0) ret = comp_ntype(c, bb[bn - 1]); }
  /* The proc fn returns mrb_int (the ABI); a string return is laundered through
     (mrb_int)(uintptr_t). Array/hash/object returns construct via the statement
     prelude (not available in this manual return path) and defer for now, as do
     float/poly/range/time, which don't fit the slot. */
  int ret_ptr = (ret == TY_STRING);
  int ret_void = (ret == TY_VOID);  /* body's last expr has no value (e.g. puts) */
  if (!proc_slot_is_direct(ret) && !ret_ptr && !ret_void) {
    free(params.v); free(used.v); free(locals.v); free(caps.v);
    unsupported(c, create, "proc with array/hash/object/float/poly return (later slice)");
    return;
  }

  int pid = ++g_proc_counter;
  int ncap = caps.n;

  /* parameter metadata for Proc#parameters: kinds (:req for lambdas, :opt for
     procs) + names, as interned symbol ids (pre-interned in analyze). */
  char meta_args[64];
  if (arity > 0) {
    buf_printf(&g_procs, "static const sp_sym _proc_kinds_%d[] = {", pid);
    for (int k = 0; k < arity; k++) buf_printf(&g_procs, "%s(sp_sym)%d", k ? ", " : "", comp_sym_intern(c, is_lambda ? "req" : "opt"));
    buf_puts(&g_procs, "};\n");
    buf_printf(&g_procs, "static const sp_sym _proc_names_%d[] = {", pid);
    for (int k = 0; k < arity; k++) buf_printf(&g_procs, "%s(sp_sym)%d", k ? ", " : "", comp_sym_intern(c, proc_param_name(c, create, k)));
    buf_puts(&g_procs, "};\n");
    snprintf(meta_args, sizeof meta_args, "_proc_kinds_%d, _proc_names_%d", pid, pid);
  }
  else snprintf(meta_args, sizeof meta_args, "NULL, NULL");

  /* capture struct + GC scan (only when the proc captures). cap_scan marks
     the cap struct itself first (sp_Proc_scan does not), then each cell --
     matching the sp_hashproc convention; marking only the cells would leave
     the cap struct unreachable and free it out from under the proc. */
  if (ncap > 0) {
    buf_printf(&g_procs, "typedef struct {");
    for (int i = 0; i < ncap; i++) buf_printf(&g_procs, " mrb_int *%s;", caps.v[i]);
    buf_printf(&g_procs, " } _proc_cap_%d;\n", pid);
    buf_printf(&g_procs, "static void _proc_cap_scan_%d(void *p) {\n", pid);
    buf_printf(&g_procs, "  sp_gc_mark(p);\n");
    buf_printf(&g_procs, "  _proc_cap_%d *_c = (_proc_cap_%d *)p;\n", pid, pid);
    for (int i = 0; i < ncap; i++) buf_printf(&g_procs, "  if (_c->%s) sp_gc_mark((void *)_c->%s);\n", caps.v[i], caps.v[i]);
    buf_puts(&g_procs, "}\n");
  }

  buf_printf(&g_proc_protos, "static mrb_int _proc_%d(void *_cap, mrb_int *args);\n", pid);

  /* Save every emission global: the proc body is a fresh function context. */
  Buf *sv_pre = g_pre; int sv_indent = g_indent, sv_nren = g_nren, sv_block = g_block_id;
  const char *sv_bpn = g_block_param_name, *sv_self = g_self, *sv_rv = g_result_var;
  TyKind sv_rt = g_ret_type;
  const char *sv_cap_struct = g_cap_struct; NameSet *sv_cap_names = g_cap_names;
  int sv_ensure_depth = g_ensure_depth;
  g_pre = NULL; g_indent = 0; g_nren = 0; g_block_id = -1; g_block_param_name = NULL;
  g_self = "self"; g_result_var = NULL; g_ret_type = ret; g_ensure_depth = 0;
  char cap_struct_name[32] = "";
  if (ncap > 0) { snprintf(cap_struct_name, sizeof cap_struct_name, "_proc_cap_%d", pid); g_cap_struct = cap_struct_name; g_cap_names = &caps; }
  else { g_cap_struct = NULL; g_cap_names = NULL; }

  Buf *pb = &g_procs;
  buf_printf(pb, "static mrb_int _proc_%d(void *_cap, mrb_int *args) {\n", pid);
  buf_puts(pb, "    SP_GC_SAVE();\n");
  if (ncap == 0) buf_puts(pb, "    (void)_cap;\n");
  buf_puts(pb, "    (void)args;\n");
  for (int k = 0; k < arity; k++) {
    const char *p = proc_param_name(c, create, k);
    LocalVar *lv = scope_local(bs, p);
    TyKind pt = lv ? lv->type : TY_INT;
    buf_puts(pb, "    "); emit_ctype(c, pt, pb); buf_printf(pb, " lv_%s = ", p);
    /* a heap-pointer param is laundered back from the mrb_int slot */
    if (proc_slot_is_ptr(pt)) { buf_puts(pb, "("); emit_ctype(c, pt, pb); buf_printf(pb, ")(uintptr_t)args[%d];\n", k); }
    else buf_printf(pb, "args[%d];\n", k);
  }
  for (int i = 0; i < locals.n; i++) {
    LocalVar *lv = scope_local(bs, locals.v[i]);
    /* a celled local is a captured var (accessed via _cap), not a fn-local */
    /* skip virtual &block slots (TY_UNKNOWN) but allow rescue-bind vars (TY_EXCEPTION) */
    if (lv && lv->type != TY_UNKNOWN && !lv->is_cell) declare_local(c, pb, lv, 0);
  }
  if (ret_ptr) {
    /* launder a heap-pointer return through the mrb_int slot: emit the body's
       leading statements, then `return (mrb_int)(uintptr_t)(<value>)`. */
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], pb, 1);
    buf_puts(pb, "  return (mrb_int)(uintptr_t)(");
    if (bn > 0) emit_expr(c, bb[bn - 1], pb); else buf_puts(pb, "0");
    buf_puts(pb, ");\n");
  }
  else if (ret_void) {
    /* no usable value: run the body as plain statements, return nil (0) */
    emit_stmts(c, body, pb, 1);
    buf_puts(pb, "  return 0;\n");
  }
  else {
    emit_stmts_tail(c, body, pb, 1);
    buf_puts(pb, "  return 0;\n");
  }
  buf_puts(pb, "}\n");

  g_pre = sv_pre; g_indent = sv_indent; g_nren = sv_nren; g_block_id = sv_block;
  g_block_param_name = sv_bpn; g_self = sv_self; g_result_var = sv_rv; g_ret_type = sv_rt;
  g_cap_struct = sv_cap_struct; g_cap_names = sv_cap_names; g_ensure_depth = sv_ensure_depth;

  if (ncap == 0) {
    buf_printf(b, "sp_proc_new_meta((void *)_proc_%d, NULL, NULL, %d, %s, %d, %s)",
               pid, arity, is_lambda ? "TRUE" : "FALSE", arity, meta_args);
  }
  else {
    /* Allocate + populate the cap struct in the enclosing statement's prelude
       (it shares the enclosing cells by pointer), then box the proc. */
    if (g_pre) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "_proc_cap_%d *_capv_%d = (_proc_cap_%d *)sp_gc_alloc(sizeof(_proc_cap_%d), NULL, _proc_cap_scan_%d);\n", pid, pid, pid, pid, pid);
      for (int i = 0; i < ncap; i++) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "_capv_%d->%s = _cell_%s;\n", pid, caps.v[i], caps.v[i]); }
    }
    buf_printf(b, "sp_proc_new_meta((void *)_proc_%d, _capv_%d, _proc_cap_scan_%d, %d, %s, %d, %s)",
               pid, pid, pid, arity, is_lambda ? "TRUE" : "FALSE", arity, meta_args);
  }

  free(params.v); free(used.v); free(locals.v); free(caps.v);
}

/* Emit the struct + the constructor (sp_<Class>_new) for one class. */
static void emit_class_struct(Compiler *c, ClassInfo *ci, Buf *b) {
  /* the typedef is forward-declared for every class first (see codegen_program)
     so a class can embed a pointer to a class defined later in the file */
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

/* A class needs a GC scan iff any ivar holds a heap reference. */
static int class_needs_scan(ClassInfo *ci) {
  for (int i = 0; i < ci->nivars; i++) {
    TyKind t = ci->ivar_types[i];
    if (t == TY_STRING || ty_is_array(t) || ty_is_hash(t) || ty_is_object(t)) return 1;
  }
  return 0;
}

/* Emit the GC scan function (marks heap ivars) for a class that needs one. */
static void emit_class_scan(Compiler *c, ClassInfo *ci, Buf *b) {
  (void)c;
  if (!class_needs_scan(ci)) return;
  buf_printf(b, "static void sp_%s_scan(void *p) {\n", ci->name);
  buf_printf(b, "  sp_%s *o = (sp_%s *)p;\n", ci->name, ci->name);
  for (int i = 0; i < ci->nivars; i++) {
    TyKind t = ci->ivar_types[i];
    const char *iv = ci->ivars[i] + 1;
    if (t == TY_STRING) buf_printf(b, "  sp_mark_string(o->iv_%s);\n", iv);
    else if (ty_is_array(t) || ty_is_hash(t) || ty_is_object(t))
      buf_printf(b, "  if (o->iv_%s) sp_gc_mark(o->iv_%s);\n", iv, iv);
  }
  buf_puts(b, "}\n");
}

static void emit_class_new(Compiler *c, ClassInfo *ci, Buf *b) {
  int cid = comp_class_index(c, ci->name);
  if (ci->is_struct) {
    /* Struct constructor: one parameter per member, set the backing ivars. */
    buf_printf(b, "static sp_%s *sp_%s_new(", ci->name, ci->name);
    for (int i = 0; i < ci->nivars; i++) {
      if (i) buf_puts(b, ", ");
      emit_ctype(c, ci->ivar_types[i], b);
      buf_printf(b, " a%d", i);
    }
    if (ci->nivars == 0) buf_puts(b, "void");
    buf_printf(b, ") {\n  sp_%s *self = (sp_%s *)sp_gc_alloc(sizeof(sp_%s), NULL, %s%s%s);\n",
              ci->name, ci->name, ci->name,
              class_needs_scan(ci) ? "sp_" : "", class_needs_scan(ci) ? ci->name : "NULL",
              class_needs_scan(ci) ? "_scan" : "");
    buf_puts(b, "  SP_GC_ROOT(self);\n");
    buf_printf(b, "  self->cls_id = %d;\n", cid);
    for (int i = 0; i < ci->nivars; i++)
      buf_printf(b, "  self->iv_%s = a%d;\n", ci->ivars[i] + 1, i);  /* skip leading '@' */
    buf_puts(b, "  return self;\n}\n");
    return;
  }
  int initcls = cid;
  int init = comp_method_in_chain(c, cid, "initialize", &initcls);
  buf_printf(b, "static sp_%s *sp_%s_new(", ci->name, ci->name);
  if (init >= 0 && c->scopes[init].nparams > 0) {
    Scope *s = &c->scopes[init];
    for (int i = 0; i < s->nparams; i++) {
      if (i) buf_puts(b, ", ");
      LocalVar *p = scope_local(s, s->pnames[i]);
      emit_ctype(c, p ? p->type : TY_INT, b);
      buf_printf(b, " lv_%s", s->pnames[i]);
    }
  }
  else {
    buf_puts(b, "void");
  }
  buf_printf(b, ") {\n  sp_%s *self = (sp_%s *)sp_gc_alloc(sizeof(sp_%s), NULL, %s%s%s);\n",
            ci->name, ci->name, ci->name,
            class_needs_scan(ci) ? "sp_" : "", class_needs_scan(ci) ? ci->name : "NULL",
            class_needs_scan(ci) ? "_scan" : "");
  buf_printf(b, "  SP_GC_ROOT(self);\n");
  buf_printf(b, "  self->cls_id = %d;\n", cid);
  /* calloc zero-inits fields; a poly (boxed) ivar's zero pattern is not nil,
     so set poly ivars to boxed-nil before initialize runs (read-only ivars
     stay nil; written ones are overwritten). */
  for (int i = 0; i < ci->nivars; i++)
    if (ci->ivar_types[i] == TY_POLY)
      buf_printf(b, "  self->iv_%s = sp_box_nil();\n", ci->ivars[i] + 1);
  if (init >= 0) {
    buf_printf(b, "  sp_%s_initialize(", c->classes[initcls].name);
    if (initcls != cid) buf_printf(b, "(sp_%s *)", c->classes[initcls].name);
    buf_puts(b, "self");
    Scope *s = &c->scopes[init];
    for (int i = 0; i < s->nparams; i++) buf_printf(b, ", lv_%s", s->pnames[i]);
    buf_puts(b, ");\n");
  }
  buf_puts(b, "  return self;\n}\n");
}

/* Inline super { block } when the parent method uses yield.
   Returns 1 if the expansion was emitted, 0 if it should fall through to a
   regular function call (parent doesn't yield, has early return, etc.). */
static int emit_super_inline(Compiler *c, int id, Buf *b, int indent, int as_expr) {
  Scope *s = comp_scope_of(c, id);
  if (s->class_id < 0 || !s->name) return 0;
  int p = c->classes[s->class_id].parent;
  int defcls = -1;
  int mi = p >= 0 ? comp_method_in_chain(c, p, s->name, &defcls) : -1;
  if (mi < 0) return 0;
  Scope *m = &c->scopes[mi];
  if (!m->yields || scope_has_return(c, mi)) return 0;
  int block = nt_ref(c->nt, id, "block");
  if (block < 0) return 0;
  if (g_nren + m->nlocals >= MAX_RENAME) return 0;
  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && !strcmp(lv->name, m->blk_param)) continue;
    if (!is_scalar_ret(lv->type)) return 0;
  }

  int tag = ++g_tmp;
  int saved_nren = g_nren, saved_block = g_block_id;
  const char *saved_bpn = g_block_param_name;
  int saved_yfb = g_yield_block_fallback;

  g_yield_block_fallback = saved_block;
  g_block_id = block;
  g_block_param_name = m->blk_param;

  if (as_expr) buf_puts(b, "({\n");
  else { emit_indent(b, indent); buf_puts(b, "{\n"); }
  int din = indent + 1;

  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && !strcmp(lv->name, m->blk_param)) continue;
    snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", lv->name);
    snprintf(g_ren_to[g_nren], sizeof g_ren_to[0], "_y%d_%s", tag, lv->name);
    const char *rn = g_ren_to[g_nren];
    g_nren++;
    emit_indent(b, din);
    emit_ctype(c, lv->type, b);
    buf_printf(b, " lv_%s = %s;\n", rn, lv->type == TY_RANGE ? "(sp_Range){0}" : default_value(lv->type));
    if (needs_root(lv->type)) { emit_indent(b, din); buf_printf(b, "SP_GC_ROOT(lv_%s);\n", rn); }
  }

  const char *ty = nt_type(c->nt, id);
  int is_forwarding = ty && !strcmp(ty, "ForwardingSuperNode");
  int args = nt_ref(c->nt, id, "arguments");
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(c->nt, args, "arguments", &argc) : NULL;
  for (int i = 0; i < m->nparams; i++) {
    emit_indent(b, din);
    buf_printf(b, "lv__y%d_%s = ", tag, m->pnames[i]);
    int sv = g_nren; g_nren = saved_nren;
    if (is_forwarding) {
      if (i < s->nparams) buf_printf(b, "lv_%s", rename_local(s->pnames[i]));
      else { g_nren = sv; emit_arg_or_default(c, m, i, -1, b); sv = g_nren; }
    }
    else {
      emit_arg_or_default(c, m, i, i < argc ? argv[i] : -1, b);
    }
    g_nren = sv;
    buf_puts(b, ";\n");
  }

  if (as_expr) {
    TyKind rt = comp_ntype(c, id);
    int rtag = ++g_tmp;
    char rvbuf[32]; snprintf(rvbuf, sizeof rvbuf, "_t%d", rtag);
    emit_indent(b, din); emit_ctype(c, rt, b);
    buf_printf(b, " _t%d = %s;\n", rtag, default_value(rt));
    const char *sv_rv = g_result_var; g_result_var = rvbuf;
    int sp = g_result_poly; g_result_poly = (rt == TY_POLY);
    emit_stmts_tail(c, m->body, b, din);
    g_result_var = sv_rv; g_result_poly = sp;
    emit_indent(b, din); buf_printf(b, "_t%d;\n", rtag);
  }
  else emit_stmts(c, m->body, b, din);

  if (as_expr) { emit_indent(b, indent); buf_puts(b, "})"); }
  else { emit_indent(b, indent); buf_puts(b, "}\n"); }

  g_nren = saved_nren;
  g_block_id = saved_block;
  g_block_param_name = saved_bpn;
  g_yield_block_fallback = saved_yfb;
  return 1;
}

/* super(args) / super -> call the parent's same-named method. */
static void emit_super(Compiler *c, int id, Buf *b) {
  Scope *s = comp_scope_of(c, id);
  if (s->class_id < 0 || !s->name) { unsupported(c, id, "super (not in a method)"); return; }
  const char *ty = nt_type(c->nt, id);
  /* Prepend chain: super goes to the next shadow in the same class. */
  const char *shadow = comp_prep_chain_target(c, s->class_id, s->name);
  if (shadow) {
    buf_printf(b, "sp_%s_%s((sp_%s *)%s",
               c->classes[s->class_id].name, mc(shadow),
               c->classes[s->class_id].name, g_self);
    if (ty && !strcmp(ty, "ForwardingSuperNode")) {
      for (int i = 0; i < s->nparams; i++) buf_printf(b, ", lv_%s", s->pnames[i]);
    }
    else {
      int smi = -1;
      for (int k = c->nscopes - 1; k >= 1; k--) {
        Scope *sc = &c->scopes[k];
        if (sc->class_id == s->class_id && sc->name && !strcmp(sc->name, shadow))
          { smi = k; break; }
      }
      emit_args_filled(c, smi, nt_ref(c->nt, id, "arguments"), ", ", b);
    }
    buf_puts(b, ")");
    return;
  }
  /* Strip __prep_N_ prefix to get the user method name for parent chain lookup. */
  const char *uname = comp_prep_user_name(s->name);
  int p = c->classes[s->class_id].parent;
  int defcls = -1;
  int mi = p >= 0 ? comp_method_in_chain(c, p, uname, &defcls) : -1;
  if (mi < 0) { unsupported(c, id, "super (no parent method)"); return; }
  buf_printf(b, "sp_%s_%s((sp_%s *)%s", c->classes[defcls].name, mc(uname), c->classes[defcls].name, g_self);
  if (ty && !strcmp(ty, "ForwardingSuperNode")) {
    for (int i = 0; i < s->nparams; i++) buf_printf(b, ", lv_%s", s->pnames[i]);
  }
  else {
    emit_args_filled(c, mi, nt_ref(c->nt, id, "arguments"), ", ", b);
  }
  buf_puts(b, ")");
}

/* Emit the static regex-literal globals and the sp_re_init() that compiles
   them at startup. Always defines sp_re_init (empty when no literals) so
   main() can call it unconditionally. */
static void emit_regex_section(Buf *b) {
  for (int i = 0; i < g_re_count; i++) {
    buf_printf(b, "static mrb_regexp_pattern *sp_re_pat_%d;\n", i);
  }
  buf_puts(b, "static void sp_re_init(void) {\n");
  if (g_re_count > 0) {
    buf_puts(b, "  sp_re_set_error_handler(sp_re_startup_error_handler);\n");
    for (int i = 0; i < g_re_count; i++) {
      buf_puts(b, "  sp_re_startup_err = NULL;\n");
      buf_puts(b, "  if (setjmp(sp_re_startup_jmp) == 0) {\n");
      buf_printf(b, "    sp_re_pat_%d = re_compile(", i);
      emit_str_literal(b, g_re_src[i]);
      buf_printf(b, ", %d, %d);\n", (int)strlen(g_re_src[i]), g_re_flg[i]);
      buf_printf(b, "  } else {\n    sp_re_pat_%d = NULL;\n  }\n", i);
    }
  }
  buf_puts(b, "}\n\n");
}

/* ---- top level ---- */

char *codegen_program(const NodeTable *nt) {
  Compiler *c = comp_new(nt);
  analyze_program(c);

  Buf b; memset(&b, 0, sizeof b);
  memset(&g_procs, 0, sizeof g_procs);
  memset(&g_proc_protos, 0, sizeof g_proc_protos);
  g_proc_counter = 0;
  g_needs_at_exit = 0;
  g_re_count = 0;
  buf_puts(&b, "/* Generated by Spinel AOT compiler */\n");
  buf_puts(&b, "#include \"sp_runtime.h\"\n");
  {
    int ns = c->nsymbols;
    if (ns > 0) {
      buf_printf(&b, "static const char *const sp_sym_names[%d] = {", ns);
      for (int i = 0; i < ns; i++) {
        if (i) buf_puts(&b, ", ");
        emit_str_literal(&b, c->symbols[i]);
      }
      buf_puts(&b, "};\n");
    }
    /* dynamic intern pool: symbols minted at runtime (Symbol#upcase,
       :"interp", String#to_sym) get ids >= the static count. */
    buf_puts(&b, "static const char *sp_dyn_syms[8192]; static int sp_ndyn = 0;\n");
    buf_printf(&b, "static const char *sp_sym_to_s(sp_sym id){"
                   "if(id>=0&&id<%d)return %s;"
                   "if(id>=%d&&id<%d+sp_ndyn)return sp_dyn_syms[id-%d];"
                   "return \"\";}\n", ns, ns > 0 ? "sp_sym_names[id]" : "\"\"", ns, ns, ns);
    buf_printf(&b, "static sp_sym sp_sym_intern(const char *s){"
                   "for(int i=0;i<%d;i++)if(strcmp(%s,s)==0)return (sp_sym)i;"
                   "for(int i=0;i<sp_ndyn;i++)if(strcmp(sp_dyn_syms[i],s)==0)return (sp_sym)(%d+i);"
                   "if(sp_ndyn<8192){sp_dyn_syms[sp_ndyn]=sp_str_dup_external(s);return (sp_sym)(%d+sp_ndyn++);}"
                   "return (sp_sym)0;}\n\n", ns, ns > 0 ? "sp_sym_names[i]" : "\"\"", ns, ns);
  }
  buf_puts(&b, "static const char *sp_class_to_s(sp_Class c){(void)c;return \"\";}\n\n\n");

  /* class structs + GC scan functions. Forward-declare every typedef first so
     a class struct may embed a pointer to a class defined later. */
  for (int i = 0; i < c->nclasses; i++)
    buf_printf(&b, "typedef struct sp_%s_s sp_%s;\n", c->classes[i].name, c->classes[i].name);
  for (int i = 0; i < c->nclasses; i++) emit_class_struct(c, &c->classes[i], &b);
  for (int i = 0; i < c->nclasses; i++) emit_class_scan(c, &c->classes[i], &b);
  if (c->nclasses > 0) buf_puts(&b, "\n");

  /* class variables: one file-scope static per (class, @@var) */
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    for (int j = 0; j < ci->ncvars; j++) {
      TyKind t = ci->cvar_types[j] == TY_UNKNOWN ? TY_INT : ci->cvar_types[j];
      buf_puts(&b, "static ");
      emit_ctype(c, t, &b);
      buf_printf(&b, " cvar_%s_%s = %s;\n", ci->name, ci->cvars[j] + 2,
                 t == TY_RANGE ? "{0}" : default_value(t));
    }
  }

  /* module/class-level instance variables (accessed from a `def self.X`):
     one file-scope static per (class, @ivar). */
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    for (int j = 0; j < ci->nivars; j++) {
      TyKind t = ci->ivar_types[j] == TY_UNKNOWN ? TY_INT : ci->ivar_types[j];
      /* static initializers must be constant: struct-valued types (poly /
         range / time) zero-init with {0}; scalars use their default. */
      const char *init = (t == TY_POLY || t == TY_RANGE || t == TY_TIME) ? "{0}"
                       : (is_scalar_ret(t) && t != TY_POLY) ? default_value(t) : "0";
      buf_puts(&b, "static ");
      emit_ctype(c, t, &b);
      buf_printf(&b, " civ_%s_%s = %s;\n", ci->name, ci->ivars[j] + 1, init);
    }
  }

  /* method prototypes (scope 0 is top-level) */
  for (int s = 1; s < c->nscopes; s++) { if (c->scopes[s].yields || !c->scopes[s].reachable || scope_is_shadowed(c, s)) continue; emit_method_signature(c, &c->scopes[s], &b); buf_puts(&b, ";\n"); }
  /* constructor prototypes + definitions (after method protos: new calls initialize) */
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (ci->is_struct) {
      /* struct constructor takes typed member params -- the prototype must
         match the definition (an empty () prototype + a _Bool param differ) */
      buf_printf(&b, "static sp_%s *sp_%s_new(", ci->name, ci->name);
      for (int m = 0; m < ci->nivars; m++) { if (m) buf_puts(&b, ", "); emit_ctype(c, ci->ivar_types[m], &b); }
      if (ci->nivars == 0) buf_puts(&b, "void");
      buf_puts(&b, ");\n");
    }
    else {
      int icid = i;
      int init = comp_method_in_chain(c, i, "initialize", &icid);
      if (init >= 0 && c->scopes[init].nparams > 0) {
        buf_printf(&b, "static sp_%s *sp_%s_new(", ci->name, ci->name);
        Scope *s = &c->scopes[init];
        for (int m = 0; m < s->nparams; m++) {
          if (m) buf_puts(&b, ", ");
          LocalVar *p = scope_local(s, s->pnames[m]);
          emit_ctype(c, p ? p->type : TY_INT, &b);
        }
        buf_puts(&b, ");\n");
      }
      else buf_printf(&b, "static sp_%s *sp_%s_new(void);\n", ci->name, ci->name);
    }
  }
  if (c->nscopes > 1 || c->nclasses > 0) buf_puts(&b, "\n");

  /* global variables and top-level constants (file-scope statics) -- emitted
     ahead of the proc functions so a proc body may reference them by name. */
  for (int i = 0; i < c->ngvars; i++) {
    LocalVar *lv = &c->gvars[i];
    if (!is_scalar_ret(lv->type)) continue;
    buf_puts(&b, "static ");
    emit_ctype(c, lv->type, &b);
    buf_printf(&b, " gv_%s = %s;\n", lv->name,
               (lv->type == TY_RANGE || lv->type == TY_POLY) ? "{0}" : default_value(lv->type));
  }
  for (int i = 0; i < c->nconsts; i++) {
    LocalVar *lv = &c->consts[i];
    if (!is_scalar_ret(lv->type)) continue;
    buf_puts(&b, "static ");
    emit_ctype(c, lv->type, &b);
    buf_printf(&b, " cst_%s = %s;\n", lv->name,
               (lv->type == TY_RANGE || lv->type == TY_POLY) ? "{0}" : default_value(lv->type));
    if (lv->init_guarded) buf_printf(&b, "static int sp_init_in_progress_%s;\n", lv->name);
  }
  if (c->ngvars || c->nconsts) buf_puts(&b, "\n");

  /* Constructor defs, method defs, and main go into a separate buffer. Any
     proc literals they contain accumulate static functions into g_procs /
     g_proc_protos; we splice those in ahead of these bodies, since a proc
     function must be declared before the body that references it. */
  Buf body; memset(&body, 0, sizeof body);
  for (int i = 0; i < c->nclasses; i++) emit_class_new(c, &c->classes[i], &body);
  for (int s = 1; s < c->nscopes; s++) { if (c->scopes[s].yields || !c->scopes[s].reachable || scope_is_shadowed(c, s)) continue; emit_method(c, &c->scopes[s], &body); }

  /* Emit END block static functions for atexit registration */
  int end_count = 0;
  {
    int top_body = c->scopes[0].body;
    if (top_body >= 0) {
      const char *tty = nt_type(c->nt, top_body);
      int tn = 0;
      const int *tbody = (tty && !strcmp(tty, "StatementsNode"))
                         ? nt_arr(c->nt, top_body, "body", &tn) : NULL;
      for (int k = 0; k < tn; k++) {
        const char *sty = nt_type(c->nt, tbody[k]);
        if (!sty || strcmp(sty, "PostExecutionNode")) continue;
        int stmts = nt_ref(c->nt, tbody[k], "statements");
        end_count++;
        buf_printf(&body, "static void sp_end_fn_%d(void) { SP_GC_SAVE();\n", end_count);
        emit_stmts(c, stmts, &body, 1);
        buf_puts(&body, "}\n");
      }
    }
  }

  buf_puts(&body, "int main(int argc,char**argv){\n");
  buf_puts(&body, "    SP_GC_SAVE();\n");
  buf_puts(&body, "    sp_re_init();\n");
  buf_puts(&body, "    { sp_argv.len = argc - 1; sp_argv.data = (const char**)malloc(sizeof(const char*) * (size_t)(argc > 1 ? argc - 1 : 1)); for (int _ai = 0; _ai < argc - 1; _ai++) sp_argv.data[_ai] = sp_str_dup_external(argv[_ai + 1]); }\n");
  /* Register END blocks (atexit runs LIFO, so they execute in reverse registration order) */
  for (int e = 1; e <= end_count; e++)
    buf_printf(&body, "    atexit(sp_end_fn_%d);\n", e);
  emit_scope_decls(c, &c->scopes[0], &body);
  buf_puts(&body, "\n");
  /* Hoist BEGIN blocks to run first */
  {
    int top_body = c->scopes[0].body;
    if (top_body >= 0) {
      const char *tty = nt_type(c->nt, top_body);
      int tn = 0;
      const int *tbody = (tty && !strcmp(tty, "StatementsNode"))
                         ? nt_arr(c->nt, top_body, "body", &tn) : NULL;
      for (int k = 0; k < tn; k++) {
        const char *sty = nt_type(c->nt, tbody[k]);
        if (!sty || strcmp(sty, "PreExecutionNode")) continue;
        int stmts = nt_ref(c->nt, tbody[k], "statements");
        emit_stmts(c, stmts, &body, 1);
      }
    }
  }
  emit_stmts(c, c->scopes[0].body, &body, 1);
  if (g_needs_at_exit)
    buf_puts(&body, "  { mrb_int _ax_args[16] = {0}; for (mrb_int _ax = sp_at_exit_count - 1; _ax >= 0; _ax--) sp_proc_call(sp_at_exit_hooks[_ax], _ax_args); }\n");
  buf_puts(&body, "  return 0;\n}\n");

  emit_regex_section(&b);
  if (g_proc_protos.len) { buf_puts(&b, g_proc_protos.p); buf_puts(&b, "\n"); }
  if (g_procs.len) { buf_puts(&b, g_procs.p); buf_puts(&b, "\n"); }
  buf_puts(&b, body.p ? body.p : "");
  free(body.p);
  free(g_procs.p); free(g_proc_protos.p);
  memset(&g_procs, 0, sizeof g_procs);
  memset(&g_proc_protos, 0, sizeof g_proc_protos);

  comp_free(c);
  return b.p;
}
