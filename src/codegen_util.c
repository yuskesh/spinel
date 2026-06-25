#include "codegen_internal.h"

Buf *g_pre = NULL;

/* SP_COLLECT_ERRORS recovery: in collect mode a codegen gap longjmps back to a
   per-unit recovery point armed by the output driver (codegen.c), so one run
   surfaces every unsupported construct instead of aborting on the first. */
jmp_buf g_unsup_recover;
int g_unsup_armed = 0;
int collect_mode(void) {
  static int collect = -1;
  if (collect < 0) collect = getenv("SP_COLLECT_ERRORS") ? 1 : 0;
  return collect;
}

void buf_putn(Buf *b, const char *s, size_t n) {
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
void buf_puts(Buf *b, const char *s) { buf_putn(b, s, strlen(s)); }
void buf_printf(Buf *b, const char *fmt, ...) {
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
int  g_indent = 0;
/* Argument-hoist overrides: emit_args_filled pre-evaluates GC-hazardous
   call arguments into rooted temps; emit_expr then substitutes the temp
   name when it reaches the overridden node. */
int  g_argov_node[MAX_ARG_OVERRIDE];
char g_argov_text[MAX_ARG_OVERRIDE][16];
int  g_n_argov = 0;
/* True if evaluating the subtree at `id` may allocate (and so may trigger
   a GC): any call, container literal, or string interpolation qualifies. */
int subtree_may_allocate(const NodeTable *nt, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (!strcmp(ty, "CallNode") || !strcmp(ty, "ArrayNode") ||
      !strcmp(ty, "HashNode") || !strcmp(ty, "KeywordHashNode") ||
      !strcmp(ty, "InterpolatedStringNode") || !strcmp(ty, "SuperNode") ||
      !strcmp(ty, "ForwardingSuperNode") || !strcmp(ty, "YieldNode"))
    return 1;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (subtree_may_allocate(nt, nt_ref_at(nt, id, i))) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (subtree_may_allocate(nt, ids[j])) return 1;
  }
  return 0;
}
int  g_tmp = 0;
char g_ren_from[MAX_RENAME][96];
char g_ren_to[MAX_RENAME][112];
int  g_nren = 0;
int  g_block_id = -1;
int  g_yield_block_fallback = -1;
const char *g_block_param_name = NULL;
/* Inside an Enumerator.new { |y| ... } generator body, the name of the yielder
   block param. A `y << v` / `y.yield(v)` on it lowers to a Fiber.yield. */
const char *g_yielder_name = NULL;
const char *g_self = "self";
/* Member-access operator for `self`: "->" when self is a pointer (the usual
   heap object), "." when emitting a value-type method body (self is a value). */
const char *g_self_deref = "->";
int g_class_body_id = -1;
int g_emitting_class_id = -1;
const char *g_dm_subst_name = NULL;
int g_dm_subst_node = -1;
int g_ie_class_id = -1;
int g_ie_discard_value = 0;
const char *g_rescue_cls = NULL, *g_rescue_msg = NULL;
const char *g_retry_label = NULL;
int g_redo_stack[64];
int g_redo_depth = 0;
const char *g_loop_break_var = NULL;
/* When a direct instance_exec/eval splice is wrapped in a do{}while(0), this
   holds the C result temp so a top-level `next <v>` captures its value before
   continuing out of the splice (mirrors g_loop_break_var for `break`). */
const char *g_ie_next_var = NULL;
/* Set while emitting an instance_exec/eval splice whose result temp is poly:
   a `break <v>` / `next <v>` carrying a scalar value must box it to match. */
int g_ie_res_poly = 0;
const char *g_result_var = NULL;
int g_result_poly = 0;
/* Non-lambda proc `return` support. While emitting a method that owns a
   proc-return frame, g_method_pr_label / g_method_pr_var name the single-exit
   goto label and the value var, so an explicit `return` funnels there (popping
   the frame once) instead of returning directly. While emitting a returning
   proc's body, g_proc_return_home is the C expression for the home frame index
   (read from the proc capture), so `return` longjmps to the home method. */
const char *g_method_pr_label = NULL;
const char *g_method_pr_var = NULL;
const char *g_proc_return_home = NULL;
/* Loop-invariant string-length hoisting: while a loop whose receiver string is
   not mutated in its body is being emitted, g_hoist_len_recv holds that
   receiver's AST local name and g_hoist_len_var the C temp caching its length;
   a matching `s.length`/`s.size` then emits the temp instead of strlen. */
const char *g_hoist_len_var = NULL;
const char *g_hoist_len_recv = NULL;
TyKind g_ret_type = TY_UNKNOWN;
int g_current_scope_is_lowered = 0;
EnsureCtx g_ensure_stack[MAX_ENSURE_DEPTH];
int       g_ensure_depth = 0;
Buf g_procs;
Buf g_proc_protos;
int g_proc_counter = 0;
int g_needs_proc_poly_retslot = 0; /* any proc returns TY_POLY via _sp_proc_poly_ret */
int g_needs_proc_poly_argslot = 0; /* any proc takes a TY_POLY arg via _sp_proc_poly_args */
/* Fiber body functions accumulate here (similar to g_procs but void(*)(sp_Fiber*)). */
int g_fiber_counter = 0;
char **g_re_src; int *g_re_flg; int g_re_count, g_re_cap;
int re_engine_flags(int pf) {
  int f = 0;
  if (pf & 4) f |= 1;
  if (pf & 8) f |= 8;
  if (pf & 16) f |= 6;
  return f;
}
int re_has_captures(const char *src) {
  if (!src) return 0;
  for (const char *p = src; *p; p++) {
    if (*p == '\\') { if (p[1]) p++; continue; }
    if (*p == '(' && p[1] != '?') return 1;
  }
  return 0;
}
int re_lit_index(Compiler *c, int nid) {
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
const char *re_lit_src(Compiler *c, int nid) {
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
void emit_interp(Compiler *c, int id, Buf *b);  /* forward */

/* Emit a regex pattern expression to `b`, handling both static literals and
   interpolated patterns. For interpolated patterns, setup is emitted to
   g_pre and a temp mrb_regexp_pattern* variable name is written to `b`.
   Returns 1 if handled, 0 if nid is not a recognizable regex. */
int emit_regex_pat_to_buf(Compiler *c, int nid, Buf *b) {
  int ri = re_lit_index(c, nid);
  if (ri >= 0) { buf_printf(b, "sp_re_pat_%d", ri); return 1; }
  const char *ty = nt_type(c->nt, nid);
  if (ty && !strcmp(ty, "InterpolatedRegularExpressionNode")) {
    int flg = re_engine_flags((int)nt_int(c->nt, nid, "flags", 0));
    int ts = ++g_tmp, tp = ++g_tmp;
    /* Emit the interpolated pattern into a local buffer: an embedded call that
       roots its own args pushes those decls to g_pre, which must land as whole
       statements before this temp's decl, not inside its initializer (#1498). */
    Buf pv; memset(&pv, 0, sizeof pv);
    emit_interp(c, nid, &pv);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "const char *_t%d = %s;\n", ts, pv.p ? pv.p : "\"\"");
    free(pv.p);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "mrb_regexp_pattern *_t%d = re_compile(_t%d, (int64_t)strlen(_t%d), %d);\n", tp, ts, ts, flg);
    buf_printf(b, "_t%d", tp);
    return 1;
  }
  return 0;
}
int nameset_has(NameSet *s, const char *nm) {
  if (!nm) return 0;
  for (int i = 0; i < s->n; i++) if (!strcmp(s->v[i], nm)) return 1;
  return 0;
}
void nameset_add(NameSet *s, const char *nm) {
  if (!nm || nameset_has(s, nm)) return;
  if (s->n >= s->cap) { s->cap = s->cap ? s->cap * 2 : 8; s->v = realloc(s->v, sizeof(char *) * (size_t)s->cap); }
  s->v[s->n++] = nm;
}
const char *g_cap_struct = NULL;
NameSet *g_cap_names = NULL;
int g_needs_at_exit = 0;
int g_needs_class_machinery = 0;
int g_has_user_global_marks = 0;
int g_uses_symbols = 0;
int g_uses_marshal = 0;
int g_uses_regex = 0;
int g_uses_argv = 0;
int g_uses_random = 0;
int g_re_init_needed = 0;
void emit_local_ref(Compiler *c, int scope_node, const char *name, Buf *b) {
  if (g_cap_struct && g_cap_names && nameset_has(g_cap_names, name)) {
    /* A TY_PROC capture is stored as (mrb_int)(uintptr_t)sp_Proc* in the cell.
       Cast it back to sp_Proc* so call sites work. A heap-object cell is a real
       typed pointer, so its deref is already the right lvalue (no cast). */
    LocalVar *clv = scope_node >= 0 ? scope_local(comp_scope_of(c, scope_node), name) : NULL;
    if (clv && clv->type == TY_PROC)
      buf_printf(b, "(sp_Proc *)(uintptr_t)(*((%s *)_cap)->%s)", g_cap_struct, name);
    else
      buf_printf(b, "(*((%s *)_cap)->%s)", g_cap_struct, name);
    return;
  }
  LocalVar *lv = scope_node >= 0 ? scope_local(comp_scope_of(c, scope_node), name) : NULL;
  if (lv && lv->is_cell) {
    if (lv->type == TY_PROC) buf_printf(b, "(sp_Proc *)(uintptr_t)(*_cell_%s)", name);
    else buf_printf(b, "(*_cell_%s)", name);
    return;
  }
  buf_printf(b, "lv_%s", rename_local(name));
}
void emit_yblk_ref(Buf *b) {
  if (g_cap_struct && g_cap_names && nameset_has(g_cap_names, "__yblk__")) {
    buf_printf(b, "(sp_Proc *)(uintptr_t)(*(((%s *)_cap)->__yblk__))", g_cap_struct);
  }
  else {
    buf_puts(b, "lv___yblk__");
  }
}
void emit_tail_lead(Buf *b) {
  if (g_result_var) buf_printf(b, "%s = ", g_result_var);
  else buf_puts(b, "return ");
}
const char *rename_local(const char *nm) {
  for (int i = 0; i < g_nren; i++)
    if (strcmp(g_ren_from[i], nm) == 0) return g_ren_to[i];
  return nm;
}
__attribute__((noreturn)) void unsupported(Compiler *c, int id, const char *what) {
  const char *ty = nt_type(c->nt, id);
  /* Ruby-map the diagnostic (#1338): a codegen gap reports against the source
     line the parser stamped (the same position the #line machinery uses), so
     the message is anchored to the .rb file instead of an opaque node id.
     Falls back to the bare form when the position wasn't stamped. */
  int ln = (int)nt_int(c->nt, id, "node_line", 0);
  char pos[1200]; pos[0] = 0;
  if (ln > 0) {
    int fid = (int)nt_int(c->nt, id, "node_file", 0);
    const char *file = nt_file_path(c->nt, fid);
    if (!file || !*file) file = c->nt->source_file;
    if (!file || !*file) file = "source.rb";
    snprintf(pos, sizeof pos, "%s:%d: ", file, ln);
  }
  const char *mname = ty && !strcmp(ty, "CallNode") ? nt_str(c->nt, id, "name") : NULL;
  if (mname) {
    int recv = nt_ref(c->nt, id, "receiver");
    int args = nt_ref(c->nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(c->nt, args, "arguments", &ac) : NULL;
    fprintf(stderr, "spinel: %sunsupported %s: node %d (%s `%s`) recv=%s/ty%d argc=%d",
            pos, what, id, ty, mname,
            recv >= 0 ? nt_type(c->nt, recv) : "-",
            recv >= 0 ? (int)comp_ntype(c, recv) : -1, ac);
    if (ac > 0 && av) fprintf(stderr, " arg0ty%d", (int)comp_ntype(c, av[0]));
    fprintf(stderr, "\n");
  }
  else
    fprintf(stderr, "spinel: %sunsupported %s: node %d (%s)\n",
            pos, what, id, ty ? ty : "?");
  /* SP_COLLECT_ERRORS: don't abort on the first gap -- longjmp back to the
     driver's per-unit recovery point (when armed) so one run surfaces every
     unsupported construct, abandoning just this unit's (discarded) output.
     `unsupported` thus never returns: it exits, or longjmps. */
  if (collect_mode() && g_unsup_armed) longjmp(g_unsup_recover, 1);
  exit(1);
}
int builtin_class_id(const char *name) {
  if (!name) return 0;
  if (!strcmp(name, "Integer"))     return -100;
  if (!strcmp(name, "Float"))       return -101;
  if (!strcmp(name, "String"))      return -102;
  if (!strcmp(name, "Symbol"))      return -103;
  if (!strcmp(name, "Array"))       return -104;
  if (!strcmp(name, "Hash"))        return -105;
  if (!strcmp(name, "Range"))       return -106;
  if (!strcmp(name, "Time"))        return -107;
  if (!strcmp(name, "Module"))      return -108;
  if (!strcmp(name, "Class"))       return -109;
  if (!strcmp(name, "NilClass"))    return -110;
  if (!strcmp(name, "TrueClass"))   return -111;
  if (!strcmp(name, "FalseClass"))  return -112;
  if (!strcmp(name, "Numeric"))     return -113;
  if (!strcmp(name, "Comparable"))  return -114;
  if (!strcmp(name, "Enumerable"))  return -115;
  if (!strcmp(name, "Object"))      return -116;
  if (!strcmp(name, "BasicObject")) return -117;
  if (!strcmp(name, "Proc"))        return -118;
  if (!strcmp(name, "Kernel"))      return -119;
  if (!strcmp(name, "IO"))          return -120;
  if (!strcmp(name, "File"))        return -121;
  if (!strcmp(name, "Exception"))   return -122;
  if (!strcmp(name, "StandardError")) return -123;
  if (!strcmp(name, "RuntimeError")) return -124;
  if (!strcmp(name, "TypeError"))   return -125;
  if (!strcmp(name, "ArgumentError")) return -126;
  if (!strcmp(name, "NameError"))   return -127;
  if (!strcmp(name, "NoMethodError")) return -128;
  if (!strcmp(name, "StopIteration")) return -129;
  if (!strcmp(name, "Math"))        return -130;
  if (!strcmp(name, "Complex"))     return -131;
  return 0;
}
const char *c_type_name(TyKind t) {
  if (ty_is_obj_array(t)) return "sp_PtrArray *";
  switch (t) {
    case TY_INT:         return "mrb_int";
    case TY_BIGINT:      return "sp_Bigint *";
    case TY_FLOAT:       return "mrb_float";
    case TY_BOOL:        return "mrb_bool";
    case TY_STRING:      return "const char *";
    case TY_SYMBOL:      return "sp_sym";
    case TY_RANGE:       return "sp_Range";
    case TY_TIME:        return "sp_Time";
    case TY_COMPLEX:     return "sp_Complex";
    case TY_RATIONAL:    return "sp_Rational";
    case TY_STRINGIO:    return "sp_StringIO *";
    case TY_STRINGSCANNER: return "sp_StringScanner *";
    case TY_MATCHDATA:   return "sp_MatchData *";
    case TY_REGEX:       return "mrb_regexp_pattern *";
    case TY_EXCEPTION:   return "sp_Exception *";
    case TY_STRBUF:      return "sp_String *";
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
    case TY_CURRY:        return "sp_Curry *";
    case TY_FIBER:        return "sp_Fiber *";
    case TY_RANDOM:       return "sp_Random *";
    case TY_METHOD:       return "sp_BoundMethod *";
    case TY_IO:           return "sp_File *";
    case TY_ARGF:         return "sp_Argf *";
    case TY_ENUMERATOR:   return "sp_Enumerator *";
    case TY_CLASS:        return "sp_Class";
    default:             return NULL;
  }
}
int is_scalar_ret(TyKind t) {
  return t == TY_INT || t == TY_BIGINT || t == TY_FLOAT || t == TY_BOOL || t == TY_STRING ||
         t == TY_SYMBOL || t == TY_RANGE || t == TY_TIME || t == TY_COMPLEX || t == TY_RATIONAL || t == TY_STRINGIO || t == TY_STRINGSCANNER || t == TY_MATCHDATA || t == TY_REGEX || t == TY_EXCEPTION ||
         t == TY_INT_ARRAY || t == TY_FLOAT_ARRAY || t == TY_STR_ARRAY ||
         t == TY_STRBUF ||
         t == TY_POLY || t == TY_POLY_ARRAY || t == TY_PROC || t == TY_CURRY || t == TY_FIBER || t == TY_RANDOM || t == TY_METHOD || t == TY_IO || t == TY_ARGF || t == TY_ENUMERATOR || t == TY_CLASS ||
         ty_is_hash(t) || ty_is_object(t) || ty_is_obj_array(t);
}
const char *ffi_c_type(const char *spec) {
  if (!spec) return "void";
  if (!strcmp(spec, "int"))    return "int";
  if (!strcmp(spec, "uint32")) return "uint32_t";
  if (!strcmp(spec, "int32"))  return "int32_t";
  if (!strcmp(spec, "uint16")) return "uint16_t";
  if (!strcmp(spec, "int16"))  return "int16_t";
  if (!strcmp(spec, "uint8"))  return "uint8_t";
  if (!strcmp(spec, "int8"))   return "int8_t";
  if (!strcmp(spec, "size_t")) return "size_t";
  if (!strcmp(spec, "long"))   return "long";
  if (!strcmp(spec, "int64"))  return "int64_t";
  if (!strcmp(spec, "float"))  return "float";
  if (!strcmp(spec, "double")) return "double";
  if (!strcmp(spec, "bool"))   return "int";
  if (!strcmp(spec, "str"))    return "const char *";
  if (!strcmp(spec, "binstr")) return "const char *";  /* bytes + sp_net_bin_len */
  if (!strcmp(spec, "ptr"))    return "void *";
  if (!strcmp(spec, "float_array")) return "const double *";
  if (!strcmp(spec, "int_array"))   return "const int64_t *";
  if (!strcmp(spec, "void"))   return "void";
  return "void";
}
const char *default_value(TyKind t) {
  switch (t) {
    case TY_INT:    return "0";
    case TY_FLOAT:  return "0.0";
    case TY_BOOL:   return "0";
    case TY_STRING: return "(&(\"\\xff\")[1])";
    case TY_SYMBOL: return "((sp_sym)-1)";
    case TY_RANGE:  return "(sp_Range){0}";
    case TY_TIME:   return "(sp_Time){0}";
    case TY_COMPLEX: return "(sp_Complex){0}";
    case TY_RATIONAL: return "(sp_Rational){0}";
    case TY_STRINGIO: return "NULL";
    case TY_STRINGSCANNER: return "NULL";
    case TY_MATCHDATA:  return "NULL";
    case TY_REGEX:      return "NULL";
    case TY_EXCEPTION: return "NULL";
    case TY_STRBUF:    return "NULL";
    case TY_INT_ARRAY:
    case TY_FLOAT_ARRAY:
    case TY_STR_ARRAY:
    case TY_POLY_ARRAY: return "NULL";
    case TY_PROC:    return "NULL";
    case TY_CURRY:   return "NULL";
    case TY_FIBER:   return "NULL";
    case TY_RANDOM:  return "NULL";
    case TY_METHOD:  return "NULL";
    case TY_IO:      return "NULL";
    case TY_ARGF:    return "NULL";
    case TY_ENUMERATOR: return "NULL";
    case TY_POLY:    return "sp_box_nil()";
    case TY_CLASS:   return "((sp_Class){-1})";
    default:        return (ty_is_hash(t) || ty_is_object(t) || ty_is_obj_array(t)) ? "NULL" : "0";
  }
}
void emit_ctype(Compiler *c, TyKind t, Buf *b) {
  if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    /* value-type classes are stored inline (sp_X); others are heap pointers */
    buf_printf(b, "sp_%s %s", c->classes[cid].name, c->classes[cid].is_value_type ? "" : "*");
  }
  else {
    const char *n = c_type_name(t);
    buf_puts(b, n ? n : "void");
  }
}
void emit_box_open(Compiler *c, TyKind t, Buf *b) {
  if (t == TY_INT)          buf_puts(b, "sp_box_int(");
  else if (t == TY_STRING)  buf_puts(b, "sp_box_str(");
  else if (t == TY_FLOAT)   buf_puts(b, "sp_box_float(");
  else if (t == TY_BOOL)    buf_puts(b, "sp_box_bool(");
  else if (t == TY_NIL)     buf_puts(b, "sp_box_nil(); (void)(");
  else if (t == TY_SYMBOL)  buf_puts(b, "sp_box_sym(");
  else if (t == TY_INT_ARRAY)   buf_puts(b, "sp_box_int_array(");
  else if (t == TY_FLOAT_ARRAY) buf_puts(b, "sp_box_float_array(");
  else if (t == TY_STR_ARRAY)   buf_puts(b, "sp_box_str_array(");
  else if (t == TY_POLY_ARRAY)  buf_puts(b, "sp_box_poly_array(");
  else if (t == TY_CLASS) buf_puts(b, "sp_box_class(");
  else if (t == TY_COMPLEX)  buf_puts(b, "sp_box_complex(");
  else if (t == TY_RATIONAL) buf_puts(b, "sp_box_rational(");
  else if (t == TY_FIBER) buf_puts(b, "sp_box_obj((void *)(");
  else if (t == TY_ENUMERATOR) buf_puts(b, "sp_box_obj((void *)(");
  else if (t == TY_IO)    buf_puts(b, "sp_box_obj((void *)(");
  else if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    buf_printf(b, "sp_box_obj((%s *)( ", c->classes[cid].name);
  }
  /* TY_POLY: already sp_RbVal, no prefix */
}
void emit_box_close(Compiler *c, TyKind t, Buf *b) {
  (void)c;
  if (t == TY_POLY || t == TY_UNKNOWN) return; /* no-op: already sp_RbVal */
  if (t == TY_FIBER) { buf_puts(b, "), SP_BUILTIN_FIBER)"); return; }
  if (t == TY_ENUMERATOR) { buf_puts(b, "), SP_BUILTIN_ENUMERATOR)"); return; }
  if (t == TY_IO)    { buf_puts(b, "), SP_BUILTIN_IO)"); return; }
  if (ty_is_object(t))        { buf_printf(b, "), %d)", ty_object_class(t)); return; }
  buf_puts(b, ")");
}
const char *array_kind(TyKind t) {
  switch (t) {
    case TY_INT_ARRAY:   return "Int";
    case TY_FLOAT_ARRAY: return "Float";
    case TY_STR_ARRAY:   return "Str";
    default:             return NULL;
  }
}
void emit_c_escaped_n(Buf *b, const char *s, size_t len) {
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
void emit_c_escaped(Buf *b, const char *s) {
  if (s) emit_c_escaped_n(b, s, strlen(s));
}
void emit_str_literal_n(Buf *b, const char *content, size_t len) {
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
void emit_str_literal(Buf *b, const char *content) {
  if (!content) { buf_puts(b, "(&(\"\\xff\")[1])"); return; }
  emit_str_literal_n(b, content, strlen(content));
}
void emit_catch_tag(Compiler *c, int id, Buf *b) {
  const char *ty = nt_type(c->nt, id);
  if (ty && !strcmp(ty, "SymbolNode")) { emit_str_literal(b, nt_str(c->nt, id, "value")); return; }
  if (ty && !strcmp(ty, "StringNode")) { emit_str_literal(b, nt_str(c->nt, id, "unescaped")); return; }
  emit_expr(c, id, b);
}
void emit_hash_key(Compiler *c, int key, TyKind kt, Buf *b) {
  TyKind actual = comp_ntype(c, key);
  /* A symbol key on a string-keyed hash (Hash.new{}'s StrPolyHash models
     symbol keys by their name) coerces to the symbol's string. A literal
     :sym becomes the name string directly; a symbol value uses sp_sym_to_s. */
  if (kt == TY_STRING && actual == TY_SYMBOL) {
    const char *kty = nt_type(c->nt, key);
    if (kty && !strcmp(kty, "SymbolNode")) {
      emit_str_literal(b, nt_str(c->nt, key, "value"));
    }
else {
      buf_puts(b, "sp_sym_to_s("); emit_expr(c, key, b); buf_puts(b, ")");
    }
    return;
  }
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
int unwrap_parens(Compiler *c, int id) {
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
const char *int_arith_fn(const char *op) {
  if (!strcmp(op, "+"))  return "sp_int_add";
  if (!strcmp(op, "-"))  return "sp_int_sub";
  if (!strcmp(op, "*"))  return "sp_int_mul";
  if (!strcmp(op, "/"))  return "sp_idiv";
  if (!strcmp(op, "%"))  return "sp_imod";
  if (!strcmp(op, "**")) return "sp_int_pow";
  return NULL;
}
const char *bigint_arith_fn(const char *op) {
  if (!strcmp(op, "+"))  return "sp_bigint_add";
  if (!strcmp(op, "-"))  return "sp_bigint_sub";
  if (!strcmp(op, "*"))  return "sp_bigint_mul";
  if (!strcmp(op, "/"))  return "sp_bigint_div";
  if (!strcmp(op, "%"))  return "sp_bigint_mod";
  return NULL;
}
const char *mc(const char *name) {
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
int scope_is_shadowed(Compiler *c, int s) {
  Scope *sc = &c->scopes[s];
  if (sc->class_id < 0 || !sc->name) return 0;
  for (int k = s + 1; k < c->nscopes; k++) {
    Scope *o = &c->scopes[k];
    if (o->class_id == sc->class_id && o->is_cmethod == sc->is_cmethod &&
        o->name && !strcmp(o->name, sc->name)) return 1;
  }
  return 0;
}
int struct_kwarg_value(Compiler *c, int kwh, const char *name) {
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
int eq_family(TyKind t) {
  if (ty_is_numeric(t)) return 1;
  if (t == TY_STRING) return 2;
  if (t == TY_BOOL) return 3;
  if (t == TY_SYMBOL) return 4;
  if (t == TY_RANGE) return 5;
  return 0;
}
int ty_matches_class(TyKind t, const char *cn, int exact) {
  const char *self_cls = NULL;
  if (t == TY_STRING) self_cls = "String";
  else if (t == TY_INT) self_cls = "Integer";
  else if (t == TY_FLOAT) self_cls = "Float";
  else if (t == TY_SYMBOL) self_cls = "Symbol";
  else if (t == TY_RANGE) self_cls = "Range";
  else if (ty_is_array(t)) self_cls = "Array";
  else if (ty_is_hash(t)) self_cls = "Hash";
  else if (t == TY_NIL) self_cls = "NilClass";
  else if (t == TY_BOOL) self_cls = "Boolean"; /* true/false split handled at call site */
  else if (t == TY_FIBER) self_cls = "Fiber";
  else if (t == TY_ENUMERATOR) self_cls = "Enumerator";
  if (!self_cls) return -1;
  if (!strcmp(cn, self_cls)) return 1;
  if (exact) return 0;
  if (!strcmp(cn, "Object") || !strcmp(cn, "BasicObject") || !strcmp(cn, "Kernel")) return 1;
  if (!strcmp(cn, "Comparable") && (t == TY_STRING || t == TY_INT || t == TY_FLOAT || t == TY_SYMBOL)) return 1;
  if (!strcmp(cn, "Numeric") && (t == TY_INT || t == TY_FLOAT)) return 1;
  if (!strcmp(cn, "Enumerable") && (ty_is_array(t) || ty_is_hash(t) || t == TY_RANGE)) return 1;
  return 0;
}
