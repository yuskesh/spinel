/* M1 code generator: scalars, locals, arithmetic/comparison operators,
 * string concatenation + interpolation, puts/print/p, and if/while
 * control flow. Emits the same runtime ABI as the legacy generator
 * (mrb_int / mrb_float / tag-byte strings / sp_* helpers). Unsupported
 * constructs abort loudly so gaps surface as errors, never miscompiles.
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

/* ---- diagnostics ---- */

static void unsupported(Compiler *c, int id, const char *what) {
  const char *ty = nt_type(c->nt, id);
  fprintf(stderr, "spinelc: unsupported %s: node %d (%s)\n",
          what, id, ty ? ty : "?");
  exit(1);
}

/* ---- C string literal escaping (tag-byte form) ---- */

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

/* `(&("\xff" "content")[1])`, or `(&("\xff")[1])` for the empty string. */
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

static void emit_indent(Buf *b, int n) { for (int i = 0; i < n; i++) buf_puts(b, "  "); }

/* ---- expression: call ---- */

static const char *int_arith_fn(const char *op) {
  if (!strcmp(op, "+"))  return "sp_int_add";
  if (!strcmp(op, "-"))  return "sp_int_sub";
  if (!strcmp(op, "*"))  return "sp_int_mul";
  if (!strcmp(op, "/"))  return "sp_idiv";
  if (!strcmp(op, "%"))  return "sp_imod";
  if (!strcmp(op, "**")) return "sp_int_pow";
  return NULL;
}

static void emit_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  if (!name) unsupported(c, id, "call (no name)");

  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  TyKind res = comp_ntype(c, id);

  /* unary */
  if ((!strcmp(name, "-@") || !strcmp(name, "+@")) && recv >= 0 && argc == 0) {
    buf_puts(b, name[0] == '-' ? "(-" : "(+");
    emit_expr(c, recv, b);
    buf_puts(b, ")");
    return;
  }
  if (!strcmp(name, "!") && recv >= 0 && argc == 0) {
    buf_puts(b, "(!"); emit_expr(c, recv, b); buf_puts(b, ")");
    return;
  }

  /* binary arithmetic */
  if (recv >= 0 && argc == 1 && int_arith_fn(name)) {
    if (rt == TY_STRING && !strcmp(name, "+")) {
      buf_puts(b, "sp_str_concat(");
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

  /* comparison */
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
    unsupported(c, id, "comparison");
  }

  /* equality */
  if (argc == 1 && (!strcmp(name, "==") || !strcmp(name, "!="))) {
    int eq = !strcmp(name, "==");
    if (rt == TY_STRING || a0 == TY_STRING) {
      buf_puts(b, eq ? "sp_str_eq(" : "(!sp_str_eq(");
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
      buf_puts(b, eq ? ")" : "))");
      return;
    }
    if (ty_is_numeric(rt) || rt == TY_BOOL) {
      buf_puts(b, "(");
      emit_expr(c, recv, b);
      buf_printf(b, " %s ", eq ? "==" : "!=");
      emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
    unsupported(c, id, "equality");
  }

  unsupported(c, id, "call");
}

/* ---- string interpolation ----
 * Builds a sp_sprintf("...fmt...", arg, arg, ...) call from an
 * InterpolatedStringNode's parts (literal StringNodes + embedded exprs). */

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
      /* literal text: escape % for the format string */
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
        buf_puts(&fmt, "%lld");
        buf_puts(&argbuf, "(long long)");
        emit_expr(c, expr, &argbuf);
      } else if (t == TY_STRING) {
        buf_puts(&fmt, "%s");
        emit_expr(c, expr, &argbuf);
      } else if (t == TY_FLOAT) {
        buf_puts(&fmt, "%s");
        buf_puts(&argbuf, "sp_float_to_s(");
        emit_expr(c, expr, &argbuf);
        buf_puts(&argbuf, ")");
      } else if (t == TY_BOOL) {
        buf_puts(&fmt, "%s");
        buf_puts(&argbuf, "(");
        emit_expr(c, expr, &argbuf);
        buf_puts(&argbuf, " ? \"true\" : \"false\")");
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
    /* no embedded exprs: a plain string literal */
    buf_puts(b, "(&(\"\\xff\" \"");
    /* fmt currently has %% for literal %; undo to raw for a literal */
    for (const char *p = fmt.p ? fmt.p : ""; *p; p++) {
      if (p[0] == '%' && p[1] == '%') { buf_puts(b, "%"); p++; }
      else buf_printf(b, "%c", *p);
    }
    buf_puts(b, "\")[1])");
    free(fmt.p); free(argbuf.p);
    return;
  }

  buf_puts(b, "sp_sprintf(\"");
  buf_puts(b, fmt.p ? fmt.p : "");
  buf_puts(b, "\"");
  buf_puts(b, argbuf.p ? argbuf.p : "");
  buf_puts(b, ")");
  free(fmt.p); free(argbuf.p);
}

/* ---- expression ---- */

static void emit_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) unsupported(c, id, "expression (no type)");

  if (!strcmp(ty, "IntegerNode")) {
    buf_printf(b, "%lldLL", nt_int(nt, id, "value", 0));
    return;
  }
  if (!strcmp(ty, "FloatNode")) {
    const char *v = nt_content(nt, id);
    buf_puts(b, v ? v : "0.0");
    return;
  }
  if (!strcmp(ty, "StringNode")) {
    emit_str_literal(b, nt_str(nt, id, "content"));
    return;
  }
  if (!strcmp(ty, "InterpolatedStringNode")) { emit_interp(c, id, b); return; }
  if (!strcmp(ty, "TrueNode"))  { buf_puts(b, "1"); return; }
  if (!strcmp(ty, "FalseNode")) { buf_puts(b, "0"); return; }
  if (!strcmp(ty, "LocalVariableReadNode")) {
    buf_printf(b, "lv_%s", nt_str(nt, id, "name"));
    return;
  }
  if (!strcmp(ty, "ParenthesesNode")) {
    int body = nt_ref(nt, id, "body");
    int n = 0;
    const int *bd = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    if (n != 1) unsupported(c, id, "parenthesized group");
    buf_puts(b, "(");
    emit_expr(c, bd[0], b);
    buf_puts(b, ")");
    return;
  }
  if (!strcmp(ty, "CallNode")) { emit_call(c, id, b); return; }

  unsupported(c, id, "expression");
}

/* ---- statements ---- */

/* Emit one value as `puts` would: type-dispatched, with a trailing
   newline. */
static void emit_puts_one(Compiler *c, int arg, Buf *b, int indent) {
  TyKind t = comp_ntype(c, arg);
  emit_indent(b, indent);
  if (t == TY_INT) {
    buf_puts(b, "printf(\"%lld\\n\", (long long)");
    emit_expr(c, arg, b);
    buf_puts(b, ");\n");
  } else if (t == TY_FLOAT) {
    buf_puts(b, "{ const char *_fs = sp_float_to_s(");
    emit_expr(c, arg, b);
    buf_puts(b, "); fputs(_fs, stdout); putchar('\\n'); }\n");
  } else if (t == TY_STRING) {
    buf_puts(b, "{ const char *_ps = (const char *)(");
    emit_expr(c, arg, b);
    buf_puts(b, "); if (_ps) { fputs(_ps, stdout); if (!*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); } else putchar('\\n'); }\n");
  } else if (t == TY_BOOL) {
    buf_puts(b, "puts((");
    emit_expr(c, arg, b);
    buf_puts(b, ") ? \"true\" : \"false\");\n");
  } else {
    unsupported(c, arg, "puts argument");
  }
}

static void emit_print_one(Compiler *c, int arg, Buf *b, int indent) {
  TyKind t = comp_ntype(c, arg);
  emit_indent(b, indent);
  if (t == TY_INT) {
    buf_puts(b, "printf(\"%lld\", (long long)");
    emit_expr(c, arg, b); buf_puts(b, ");\n");
  } else if (t == TY_FLOAT) {
    buf_puts(b, "fputs(sp_float_to_s(");
    emit_expr(c, arg, b); buf_puts(b, "), stdout);\n");
  } else if (t == TY_STRING) {
    buf_puts(b, "{ const char *_s = (");
    emit_expr(c, arg, b);
    buf_puts(b, "); if (_s) fputs(_s, stdout); }\n");
  } else if (t == TY_BOOL) {
    buf_puts(b, "fputs((");
    emit_expr(c, arg, b);
    buf_puts(b, ") ? \"true\" : \"false\", stdout);\n");
  } else {
    unsupported(c, arg, "print argument");
  }
}

static void emit_p_one(Compiler *c, int arg, Buf *b, int indent) {
  TyKind t = comp_ntype(c, arg);
  emit_indent(b, indent);
  if (t == TY_INT) {
    buf_puts(b, "printf(\"%lld\\n\", (long long)");
    emit_expr(c, arg, b); buf_puts(b, ");\n");
  } else if (t == TY_FLOAT) {
    buf_puts(b, "{ const char *_fs = sp_float_to_s(");
    emit_expr(c, arg, b);
    buf_puts(b, "); fputs(_fs, stdout); putchar('\\n'); }\n");
  } else if (t == TY_STRING) {
    buf_puts(b, "fputs(sp_str_inspect(");
    emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  } else if (t == TY_BOOL) {
    buf_puts(b, "puts((");
    emit_expr(c, arg, b);
    buf_puts(b, ") ? \"true\" : \"false\");\n");
  } else {
    unsupported(c, arg, "p argument");
  }
}

/* puts/print/p dispatch. Returns 1 if handled as an output call. */
static int emit_output_call(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name || recv >= 0) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);

  if (!strcmp(name, "puts")) {
    if (argc == 0) { emit_indent(b, indent); buf_puts(b, "putchar('\\n');\n"); return 1; }
    for (int k = 0; k < argc; k++) emit_puts_one(c, argv[k], b, indent);
    return 1;
  }
  if (!strcmp(name, "print")) {
    for (int k = 0; k < argc; k++) emit_print_one(c, argv[k], b, indent);
    return 1;
  }
  if (!strcmp(name, "p")) {
    for (int k = 0; k < argc; k++) emit_p_one(c, argv[k], b, indent);
    return 1;
  }
  return 0;
}

static void emit_assign(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  int v = nt_ref(nt, id, "value");
  emit_indent(b, indent);
  buf_printf(b, "lv_%s = ", nm);
  emit_expr(c, v, b);
  buf_puts(b, ";\n");
}

static void emit_op_assign(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  const char *op = nt_str(nt, id, "binary_operator");
  int v = nt_ref(nt, id, "value");
  LocalVar *lv = comp_local(c, nm);
  TyKind t = lv ? lv->type : TY_UNKNOWN;
  emit_indent(b, indent);

  if (t == TY_STRING && !strcmp(op, "+")) {
    buf_printf(b, "lv_%s = sp_str_concat(lv_%s, ", nm, nm);
    emit_expr(c, v, b);
    buf_puts(b, ");\n");
    return;
  }
  if (t == TY_INT && (!strcmp(op, "+") || !strcmp(op, "-") || !strcmp(op, "*"))) {
    buf_printf(b, "lv_%s %s= ", nm, op);
    emit_expr(c, v, b);
    buf_puts(b, ";\n");
    return;
  }
  if (t == TY_INT) {
    const char *fn = int_arith_fn(op);
    if (fn) {
      buf_printf(b, "lv_%s = %s(lv_%s, ", nm, fn, nm);
      emit_expr(c, v, b);
      buf_puts(b, ");\n");
      return;
    }
  }
  if (t == TY_FLOAT && (!strcmp(op, "+") || !strcmp(op, "-") ||
                        !strcmp(op, "*") || !strcmp(op, "/"))) {
    buf_printf(b, "lv_%s %s= ", nm, op);
    emit_expr(c, v, b);
    buf_puts(b, ";\n");
    return;
  }
  unsupported(c, id, "operator assignment");
}

/* Emit a C boolean condition for a Ruby predicate. M1 supports bool
   predicates (comparisons, &&/||-free). */
static void emit_cond(Compiler *c, int id, Buf *b) {
  if (comp_ntype(c, id) != TY_BOOL) unsupported(c, id, "condition (non-bool)");
  emit_expr(c, id, b);
}

static void emit_if(Compiler *c, int id, Buf *b, int indent, int is_unless) {
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
  emit_stmts(c, then_b, b, indent + 1);
  emit_indent(b, indent);
  buf_puts(b, "}");

  if (sub >= 0) {
    const char *sty = nt_type(nt, sub);
    if (sty && !strcmp(sty, "ElseNode")) {
      buf_puts(b, " else {\n");
      int s = nt_ref(nt, sub, "statements");
      emit_stmts(c, s, b, indent + 1);
      emit_indent(b, indent);
      buf_puts(b, "}\n");
    } else if (sty && !strcmp(sty, "IfNode")) {
      buf_puts(b, " else {\n");
      emit_if(c, sub, b, indent + 1, 0);
      emit_indent(b, indent);
      buf_puts(b, "}\n");
    } else {
      buf_puts(b, "\n");
    }
  } else {
    buf_puts(b, "\n");
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

static void emit_stmt(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) unsupported(c, id, "statement (no type)");

  if (!strcmp(ty, "CallNode")) {
    if (emit_output_call(c, id, b, indent)) return;
    /* otherwise a value-producing / side-effecting call as a statement */
    emit_indent(b, indent);
    emit_expr(c, id, b);
    buf_puts(b, ";\n");
    return;
  }
  if (!strcmp(ty, "LocalVariableWriteNode")) { emit_assign(c, id, b, indent); return; }
  if (!strcmp(ty, "LocalVariableOperatorWriteNode")) { emit_op_assign(c, id, b, indent); return; }
  if (!strcmp(ty, "IfNode"))     { emit_if(c, id, b, indent, 0); return; }
  if (!strcmp(ty, "UnlessNode")) { emit_if(c, id, b, indent, 1); return; }
  if (!strcmp(ty, "WhileNode"))  { emit_while(c, id, b, indent, 0); return; }
  if (!strcmp(ty, "UntilNode"))  { emit_while(c, id, b, indent, 1); return; }

  unsupported(c, id, "statement");
}

/* Emit the body of a StatementsNode (or a single statement). */
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

/* ---- local declarations ---- */

static void emit_local_decls(Compiler *c, Buf *b) {
  for (int i = 0; i < c->nlocals; i++) {
    LocalVar *lv = &c->locals[i];
    switch (lv->type) {
      case TY_INT:
        buf_printf(b, "    mrb_int lv_%s = 0;\n", lv->name); break;
      case TY_FLOAT:
        buf_printf(b, "    mrb_float lv_%s = 0.0;\n", lv->name); break;
      case TY_BOOL:
        buf_printf(b, "    mrb_bool lv_%s = 0;\n", lv->name); break;
      case TY_STRING:
        buf_printf(b, "    const char * lv_%s = (&(\"\\xff\")[1]);\n", lv->name);
        buf_printf(b, "    SP_GC_ROOT(lv_%s);\n", lv->name);
        break;
      default:
        fprintf(stderr, "spinelc: local '%s' has unsupported type %s\n",
                lv->name, ty_name(lv->type));
        exit(1);
    }
  }
}

/* ---- top level ---- */

char *codegen_program(const NodeTable *nt) {
  Compiler *c = comp_new(nt);
  analyze_program(c);

  Buf b; memset(&b, 0, sizeof b);
  buf_puts(&b, "/* Generated by Spinel AOT compiler */\n");
  buf_puts(&b, "#include \"sp_runtime.h\"\n");
  buf_puts(&b, "static const char *sp_sym_to_s(sp_sym id){(void)id;return \"\";}\n\n");
  buf_puts(&b, "static const char *sp_class_to_s(sp_Class c){(void)c;return \"\";}\n\n\n");
  buf_puts(&b, "int main(int argc,char**argv){\n");
  buf_puts(&b, "    SP_GC_SAVE();\n");
  emit_local_decls(c, &b);
  buf_puts(&b, "\n");

  int root = nt->root_id;
  const char *rty = nt_type(nt, root);
  if (!rty || strcmp(rty, "ProgramNode") != 0) unsupported(c, root, "root");
  int stmts = nt_ref(nt, root, "statements");
  emit_stmts(c, stmts, &b, 1);

  buf_puts(&b, "  return 0;\n}\n");

  comp_free(c);
  return b.p;
}
