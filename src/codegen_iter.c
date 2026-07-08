/* codegen_iter.c -- block invocation, inline-call, and iteration/loop
   lowering, split out of codegen_call.c. Pure code movement, no logic change. */

#include "codegen_internal.h"

/* Follow a chain of pure `...` forwarders (a method whose whole body is a
   single `target(...)` call, no receiver) from `mi` to the method that
   actually yields or owns the &block; return its index, else -1. A real-
   function forwarder can't pass a literal block down to a yielding target,
   so a block-bearing call is redirected straight to that final target. */
static int pure_forwarding_target(Compiler *c, int mi, int depth) {
  if (mi < 0 || depth > 16) return -1;
  Scope *m = &c->scopes[mi];
  if (m->yields || (m->blk_param && m->blk_param[0])) return mi;
  int body = m->body;
  if (body < 0 || !nt_type(c->nt, body) || !sp_streq(nt_type(c->nt, body), "StatementsNode")) return -1;
  int n = 0; const int *st = nt_arr(c->nt, body, "body", &n);
  if (n != 1) return -1;
  int call = st[0];
  const char *cty = nt_type(c->nt, call);
  if (!cty || !sp_streq(cty, "CallNode") || nt_ref(c->nt, call, "receiver") >= 0) return -1;
  int args = nt_ref(c->nt, call, "arguments");
  int ac = 0; const int *av = args >= 0 ? nt_arr(c->nt, args, "arguments", &ac) : NULL;
  if (ac != 1 || !nt_type(c->nt, av[0]) || !sp_streq(nt_type(c->nt, av[0]), "ForwardingArgumentsNode")) return -1;
  const char *tn = nt_str(c->nt, call, "name");
  if (!tn) return -1;
  int t = comp_method_index(c, tn);
  if (t < 0 && m->class_id >= 0) t = comp_method_in_chain(c, m->class_id, tn, NULL);
  return pure_forwarding_target(c, t, depth + 1);
}

/* Inline a call to a free-function yielding method `foo(args) { |bp| ... }`:
   declare the method's locals (renamed to avoid clashing with the call
   site), bind params to args, then emit the method body with yield
   expanding to the block. Returns 1 if handled. */
int emit_inline_call_x(Compiler *c, int id, Buf *b, int indent, int as_expr) {
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
    const char *cname = (rty && sp_streq(rty, "ConstantReadNode")) ? nt_str(nt, recv, "name") : NULL;
    int ci = cname ? comp_class_index(c, cname) : -1;
    if (ci >= 0) {
      /* Cls.method with a yield block: look up as a class method */
      mi = comp_cmethod_in_chain(c, ci, name, NULL);
    }
    else if (ty_is_object(rt)) {
      /* An instance receiver -- including a constant that holds an instance
         (e.g. `S = Set.new(...); S.each { }`), which is not a class name so
         falls through here rather than the class-method lookup above. */
      recv_class = ty_object_class(rt);
      mi = comp_method_in_chain(c, recv_class, name, NULL);
    }
    else return 0;
  }
  (void)implicit_self;
  if (mi < 0) return 0;
  /* `fwd(args) { block }` where fwd just forwards `target(...)`: a literal
     block can't reach a real-function forwarder, so retarget to `target`
     with this call's args + block (target then splices the block normally). */
  {
    int blk0 = nt_ref(nt, id, "block");
    if (blk0 >= 0 && nt_type(nt, blk0) && sp_streq(nt_type(nt, blk0), "BlockNode")) {
      int t = pure_forwarding_target(c, mi, 0);
      if (t >= 0) mi = t;
    }
  }
  Scope *m = &c->scopes[mi];
  if (!m->yields) return 0;
  /* A `return` inside the yielding method used to bail here -- but a bailed
     block call falls back to a plain function call against a symbol that is
     never emitted (yielding methods have no standalone function), an
     undefined-symbol link error (doom's PlayerPhysics#each_nearby_linedef:
     an early `@map.linedefs.each { |ld| yield ld }; return` branch). Inline
     anyway, funneling the method's own returns to a per-inline exit label;
     the caller block spliced at yield sites is exempted by
     emit_block_invoke, which restores the real function's funnel. */
  int m_has_ret = scope_has_return(c, mi);
  int block = nt_ref(nt, id, "block");   /* may be -1: no block passed */
  /* `inner(&)` / `inner(&block)`: a BlockArgumentNode forwards the block
     active at this (already-inlined) site, not a fresh literal. */
  if (block >= 0 && nt_type(nt, block) && sp_streq(nt_type(nt, block), "BlockArgumentNode"))
    block = g_block_id;
  if (g_nren + m->nlocals >= MAX_RENAME) return 0;
  /* Pre-check: every body local must have an emittable type. Bail BEFORE
     writing anything (a mid-emit bail would leave an unbalanced `{`). */
  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && sp_streq(lv->name, m->blk_param)) continue;
    if (!is_scalar_ret(lv->type)) return 0;
  }

  int tag = ++g_tmp;
  int saved_nren = g_nren, saved_block = g_block_id;
  const char *saved_self = g_self;
  const char *saved_bpn = g_block_param_name;
  int saved_yfb = g_yield_block_fallback;
  const char *saved_bbv = g_block_brk_var, *saved_yfbv = g_yield_blk_brk_fallback;
  const char *saved_ser = g_brk_ser_var;
  int saved_bbe = g_block_brk_ebase, saved_yfbe = g_yield_blk_brk_efallback;
  int saved_bbexc = g_block_brk_exc_base, saved_bexc = g_brk_exc_base;
  int saved_ebase = g_brk_ensure_base;
  /* Stack-local, not static: emit_inline_call_x recurses (a yielded block can
     call the same yielding method), and g_self points into this buffer. A
     shared static would be clobbered by the nested inline, so the outer frame's
     ensure/trailing-self would emit the inner receiver temp (undeclared here). */
  char selfbuf[64];
  /* Nested `yield` inside the block body should chain to the block that was
     active before this inline, not to the inner block. */
  g_yield_block_fallback = saved_block;
  g_yield_blk_brk_fallback = saved_bbv;
  g_yield_blk_brk_efallback = saved_bbe;
  /* the block being captured is caller code: record the caller's self so
     emit_block_invoke can restore it around the spliced block body. Aliasing
     g_self by pointer is safe now that selfbuf is stack-local: it names an
     ancestor frame's selfbuf, which stays live and unmodified for the whole
     nested emission (a frame only ever writes its own selfbuf). */
  const char *saved_self_fb = g_yield_self_fallback;
  const char *saved_deref_fb = g_yield_self_deref_fallback;
  g_yield_self_fallback = g_self;
  g_yield_self_deref_fallback = g_self_deref;
  g_block_id = block;
  /* the literal block binds to THIS call site's break scope; a forwarded
     BlockArgumentNode block keeps its original definition-site scope */
  g_block_brk_var = (block == saved_block) ? saved_bbv : saved_ser;
  g_block_brk_ebase = (block == saved_block) ? saved_bbe : saved_ebase;
  g_block_brk_exc_base = (block == saved_block) ? saved_bbexc : saved_bexc;
  /* the METHOD BODY's own breaks (a while inside m) never target the caller */
  g_brk_ser_var = NULL;
  g_block_param_name = m->blk_param;

  if (as_expr) buf_puts(b, "({\n");
  else { emit_indent(b, indent); buf_puts(b, "{\n"); }
  /* instance method: bind self to the receiver. A heap object is a pointer; a
     value-type receiver is a by-value struct, so copy it and dereference its
     ivars with `.` (value types are immutable, so the copy is transparent). */
  const char *saved_deref = g_self_deref;
  if (recv >= 0 && recv_class >= 0) {
    int self_is_val = c->classes[recv_class].is_value_type;
    int st = ++g_tmp;
    emit_indent(b, indent + 1);
    buf_printf(b, "sp_%s %s_t%d = ", c->classes[recv_class].c_name, self_is_val ? "" : "*", st);
    emit_expr(c, recv, b);
    buf_puts(b, ";\n");
    snprintf(selfbuf, sizeof selfbuf, "_t%d", st);
    g_self = selfbuf;
    g_self_deref = self_is_val ? "." : "->";
  }
  int din = indent + 1;

  /* declare method locals under renamed names */
  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && sp_streq(lv->name, m->blk_param)) continue;  /* virtual &block slot */
    snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", lv->name);
    snprintf(g_ren_to[g_nren], sizeof g_ren_to[0], "_y%d_%s", tag, lv->name);
    const char *rn = g_ren_to[g_nren];
    g_nren++;
    emit_indent(b, din);
    emit_ctype(c, lv->type, b);
    buf_printf(b, " lv_%s = %s;\n", rn, lv->type == TY_RANGE ? "(sp_Range){0}" : default_value(lv->type));
    if (needs_root(lv->type)) { emit_indent(b, din); buf_printf(b, lv->type == TY_POLY ? "SP_GC_ROOT_RBVAL(lv_%s);\n" : "SP_GC_ROOT(lv_%s);\n", rn); }
  }

  /* bind params to call args (args are in the call-site scope: renames off) */
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  /* `bar(...)` inside a `def foo(...)` forwarder: bind this (inlined) target's
     params from the enclosing forwarder's synth __fwd_* params, not from a
     literal ForwardingArgumentsNode (which has no value of its own). */
  Scope *fwd_encl = NULL;
  if (argc == 1 && argv && nt_type(nt, argv[0]) &&
      sp_streq(nt_type(nt, argv[0]), "ForwardingArgumentsNode"))
    fwd_encl = comp_scope_of(c, argv[0]);
  /* A trailing keyword-hash arg binds by param name, not positionally. */
  int kwh = -1, pos_argc = argc;
  if (argc > 0 && argv && nt_type(nt, argv[argc - 1]) &&
      sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) {
    kwh = argv[argc - 1]; pos_argc = argc - 1;
  }
  for (int i = 0; i < m->nparams; i++) {
    emit_indent(b, din);
    buf_printf(b, "lv__y%d_%s = ", tag, m->pnames[i]);
    /* hide THIS inline's renames only: args are call-site expressions,
       and the call site may itself be an outer inlined body whose locals
       are renamed (nested yield-method inlines) -- zeroing the whole
       table emitted the unrenamed lv_<name> (undeclared identifier, or a
       silent capture of a same-named caller local). */
    int sv = g_nren; g_nren = saved_nren;
    if (fwd_encl && i < fwd_encl->nparams) {
      LocalVar *ep = scope_local(fwd_encl, fwd_encl->pnames[i]);
      LocalVar *mp = scope_local(m, m->pnames[i]);
      TyKind et = ep ? ep->type : TY_POLY;
      TyKind mt = mp ? mp->type : TY_POLY;
      char txt[80]; snprintf(txt, sizeof txt, "lv_%s", fwd_encl->pnames[i]);
      if (mt == TY_POLY && et != TY_POLY) emit_boxed_text(c, et, txt, b);
      else buf_puts(b, txt);
    }
    else if (i < pos_argc) emit_arg_or_default(c, m, i, argv[i], b);
    else {
      int kv = kwh >= 0 ? kwh_lookup(nt, kwh, m->pnames[i]) : -1;
      emit_arg_or_default(c, m, i, kv, b);
    }
    g_nren = sv;
    buf_puts(b, ";\n");
  }

  /* per-inline return funnel (stack storage: the inliner recurses, and the
     saved outer label pointer must stay valid across a nested inline). */
  char inl_lbl[32];
  const char *sv_prl = g_method_pr_label, *sv_prv = g_method_pr_var;
  TyKind sv_prt = g_ret_type;
  int sv_prexc = g_method_pr_exc_depth;
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
    if (m_has_ret) {
      snprintf(inl_lbl, sizeof inl_lbl, "_yret%d", tag);
      g_method_pr_label = inl_lbl; g_method_pr_var = rvbuf; g_ret_type = rt;
      g_method_pr_exc_depth = g_exc_frame_depth;
      /* body in its own scope: the funnel goto then EXITS the scopes of any
         cleanup-attributed GC roots the body declares (legal, cleanups run)
         instead of jumping over them in the same scope (a C error). */
      emit_indent(b, din); buf_puts(b, "{\n");
    }
    emit_stmts_tail(c, m->body, b, m_has_ret ? din + 1 : din);
    if (m_has_ret) {
      g_method_pr_label = sv_prl; g_method_pr_var = sv_prv; g_ret_type = sv_prt;
      g_method_pr_exc_depth = sv_prexc;
      emit_indent(b, din); buf_puts(b, "}\n");
      emit_indent(b, din); buf_printf(b, "_yret%d: ;\n", tag);
    }
    g_result_var = sv_rv; g_result_poly = sp;
    emit_indent(b, din); buf_printf(b, "_t%d;\n", rtag);
  }
  else {
    if (m_has_ret) {
      snprintf(inl_lbl, sizeof inl_lbl, "_yret%d", tag);
      g_method_pr_label = inl_lbl; g_method_pr_var = NULL;
      g_method_pr_exc_depth = g_exc_frame_depth;
      emit_indent(b, din); buf_puts(b, "{\n");   /* see expr-path comment */
    }
    emit_stmts(c, m->body, b, m_has_ret ? din + 1 : din);
    if (m_has_ret) {
      g_method_pr_label = sv_prl; g_method_pr_var = sv_prv;
      g_method_pr_exc_depth = sv_prexc;
      emit_indent(b, din); buf_puts(b, "}\n");
      emit_indent(b, din); buf_printf(b, "_yret%d: ;\n", tag);
    }
  }
  if (as_expr) { emit_indent(b, indent); buf_puts(b, "})"); }
  else { emit_indent(b, indent); buf_puts(b, "}\n"); }

  g_nren = saved_nren;
  g_block_id = saved_block;
  g_block_brk_var = saved_bbv; g_yield_blk_brk_fallback = saved_yfbv;
  g_block_brk_ebase = saved_bbe; g_yield_blk_brk_efallback = saved_yfbe;
  g_block_brk_exc_base = saved_bbexc; g_brk_exc_base = saved_bexc;
  g_brk_ser_var = saved_ser; g_brk_ensure_base = saved_ebase;
  g_self = saved_self;
  g_self_deref = saved_deref;
  g_block_param_name = saved_bpn;
  g_yield_block_fallback = saved_yfb;
  g_yield_self_fallback = saved_self_fb;
  g_yield_self_deref_fallback = saved_deref_fb;
  return 1;
}

int emit_inline_call(Compiler *c, int id, Buf *b, int indent) {
  return emit_inline_call_x(c, id, b, indent, 0);
}

/* Is `id` a `<&block-param>.call(...)` invocation of the active block? */
int is_block_call(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (!g_block_param_name || !g_block_param_name[0] || g_block_id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "CallNode")) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || (!sp_streq(nm, "call") && !sp_streq(nm, "()") && !sp_streq(nm, "[]") && !sp_streq(nm, "yield"))) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "LocalVariableReadNode")) return 0;
  const char *rn = nt_str(nt, recv, "name");
  return rn && sp_streq(rn, g_block_param_name);
}

/* A `<&block-param>.call(...)` on the inlined method's block param while NO
   block is supplied at this site (g_block_id<0). This arises when a real-
   function forwarder with no block of its own inlines a target that calls its
   &block: the path is dead for any real caller (a block-requiring method
   invoked without one raises), but must still compile. Caller emits nil. */
int is_blockless_block_param_call(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (!g_block_param_name || !g_block_param_name[0] || g_block_id >= 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "CallNode")) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || (!sp_streq(nm, "call") && !sp_streq(nm, "()") && !sp_streq(nm, "[]"))) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "LocalVariableReadNode")) return 0;
  const char *rn = nt_str(nt, recv, "name");
  return rn && sp_streq(rn, g_block_param_name);
}

/* Expand the active block's body, binding its params to the given call
   args. Shared by YieldNode and `block.call`. `as_expr` wraps in ({...}). */
void emit_block_invoke(Compiler *c, int args_node, Buf *b, int indent, int as_expr) {
  const NodeTable *nt = c->nt;
  int blk = g_block_id;
  int bbody = nt_ref(nt, blk, "body");
  int yc = 0;
  const int *yargs = args_node >= 0 ? nt_arr(nt, args_node, "arguments", &yc) : NULL;
  Scope *bsc = comp_scope_of(c, blk);
  /* `yield(*arr)`: a single splat spreads the array across the block params
     (auto-splat). Evaluate it once into a rooted temp and bind each param (and
     any rest param) from its elements rather than from the splat AST node. */
  int splat_tmp = -1; TyKind splat_at = TY_UNKNOWN;
  if (yc == 1 && yargs && nt_type(nt, yargs[0]) &&
      sp_streq(nt_type(nt, yargs[0]), "SplatNode")) {
    int inner = nt_ref(nt, yargs[0], "expression");
    TyKind at = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
    if (ty_is_array(at) || at == TY_POLY_ARRAY) {
      splat_at = at;
      splat_tmp = ++g_tmp;
      Buf sb; memset(&sb, 0, sizeof sb); emit_expr(c, inner, &sb);
      emit_indent(g_pre, g_indent);
      emit_ctype(c, at, g_pre);
      buf_printf(g_pre, " _t%d = %s;\n", splat_tmp, sb.p ? sb.p : "");
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", splat_tmp);
      free(sb.p);
    }
  }
  if (as_expr) buf_puts(b, "({ ");
  for (int k = 0; ; k++) {
    const char *bp = block_param_name(c, blk, k);
    if (!bp) break;
    /* When inside an inlined method, block params may be renamed (e.g. x →
       _y3_x); apply the rename table so we write the right C variable. */
    const char *bpr = rename_local(bp);
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "lv_%s = ", bpr);
    if (splat_tmp >= 0) {
      /* element k of the splatted array, guarded: when the array is shorter
         than the param list the surplus params bind nil (CRuby auto-splat),
         using the same per-slot default the non-splat under-fill path does. */
      LocalVar *bl = bsc ? scope_local(bsc, bp) : NULL;
      TyKind bt = bl ? bl->type : TY_UNKNOWN;
      TyKind et = ty_array_elem(splat_at);
      Buf eb; memset(&eb, 0, sizeof eb);
      emit_array_elem_at(splat_at, splat_tmp, k, &eb);
      buf_printf(b, "(%d < (_t%d ? _t%d->len : 0) ? ", k, splat_tmp, splat_tmp);
      if (bt == TY_POLY && et != TY_POLY && et != TY_UNKNOWN)
        emit_boxed_text(c, et, eb.p ? eb.p : "0", b);
      else if (et == TY_POLY && bt != TY_POLY && bt != TY_UNKNOWN)
        emit_unbox_text(c, bt, eb.p ? eb.p : "", b);
      else
        buf_puts(b, eb.p ? eb.p : "");
      buf_printf(b, " : %s)", bt == TY_RANGE ? "(sp_Range){0}" : default_value(bt));
      free(eb.p);
    }
    else if (k < yc) {
      LocalVar *bl = bsc ? scope_local(bsc, bp) : NULL;
      TyKind bt = bl ? bl->type : TY_UNKNOWN;
      TyKind at = comp_ntype(c, yargs[k]);
      if (bt == TY_POLY && at != TY_POLY && at != TY_UNKNOWN)
        emit_boxed(c, yargs[k], b);
      else if (at == TY_POLY && bt != TY_POLY && bt != TY_UNKNOWN) {
        /* a poly yield value into a scalar (e.g. int, non-widened) block param:
           unbox down to the slot type (the reverse of the box arm above). */
        Buf yb; memset(&yb, 0, sizeof yb); emit_expr(c, yargs[k], &yb);
        emit_unbox_text(c, bt, yb.p ? yb.p : "", b); free(yb.p);
      }
      else
        emit_expr(c, yargs[k], b);
    }
    else {
      LocalVar *bl = scope_local(bsc, bp);
      TyKind bt = bl ? bl->type : TY_INT;
      buf_puts(b, bt == TY_RANGE ? "(sp_Range){0}" : default_value(bt));
    }
    buf_puts(b, as_expr ? "; " : ";\n");
  }
  /* A trailing rest parameter (`|*a|`) collects the yielded arguments past the
     requireds into a fresh array. */
  const char *brest = block_rest_name(c, blk);
  if (brest) {
    const char *brestr = rename_local(brest);
    int nreq = 0; while (block_param_name(c, blk, nreq)) nreq++;
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "lv_%s = sp_PolyArray_new();%s", brestr, as_expr ? " " : "\n");
    if (splat_tmp >= 0) {
      /* collect splat elements past the required params at runtime */
      TyKind et = ty_array_elem(splat_at);
      int jj = ++g_tmp;
      if (!as_expr) emit_indent(b, indent);
      buf_printf(b, "for (mrb_int _t%d = %d; _t%d && _t%d < _t%d->len; _t%d++) sp_PolyArray_push(lv_%s, ",
                 jj, nreq, splat_tmp, jj, splat_tmp, jj, brestr);
      char acc[96];
      if (splat_at == TY_POLY_ARRAY) snprintf(acc, sizeof acc, "sp_PolyArray_get(_t%d, _t%d)", splat_tmp, jj);
      else snprintf(acc, sizeof acc, "sp_%sArray_get(_t%d, _t%d)", array_kind(splat_at) ? array_kind(splat_at) : "Int", splat_tmp, jj);
      if (splat_at == TY_POLY_ARRAY) buf_puts(b, acc);
      else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, et, acc, &bx); buf_puts(b, bx.p ? bx.p : acc); free(bx.p); }
      buf_puts(b, as_expr ? "); " : ");\n");
    }
    else for (int j = nreq; j < yc; j++) {
      if (!as_expr) emit_indent(b, indent);
      buf_printf(b, "sp_PolyArray_push(lv_%s, ", brestr);
      emit_boxed(c, yargs[j], b);
      buf_puts(b, as_expr ? "); " : ");\n");
    }
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
  /* the block body executes in its DEFINITION site's break scope: a
     top-level break targets the call that received the block, not whatever
     loop/iterator surrounds this yield inside the method body */
  const char *svser = g_brk_ser_var; g_brk_ser_var = g_block_brk_var;
  int svebase = g_brk_ensure_base; g_brk_ensure_base = g_block_brk_ebase;
  int svbexc = g_brk_exc_base; g_brk_exc_base = g_block_brk_exc_base;
  const char *svbbv = g_block_brk_var; g_block_brk_var = g_yield_blk_brk_fallback;
  int svbbe = g_block_brk_ebase; g_block_brk_ebase = g_yield_blk_brk_efallback;
  /* The block body lexically belongs to the REAL enclosing function: a
     `return` inside it exits that method, not the inlined region -- so the
     inline funnel (if one is active) is suspended in favor of the real
     function's own return funnel. */
  const char *sv_bl = g_method_pr_label, *sv_bv = g_method_pr_var;
  TyKind sv_bt = g_ret_type;
  int sv_bexc = g_method_pr_exc_depth;
  g_method_pr_label = g_fn_pr_label; g_method_pr_var = g_fn_pr_var;
  g_ret_type = g_fn_ret_type;
  g_method_pr_exc_depth = 0;   /* the real function's funnel sits at depth 0 */
  /* likewise, the block body's `self` is the CALLER's (an ivar read inside
     the block must not resolve against the inlined method's receiver). */
  const char *sv_bself = g_self, *sv_bderef = g_self_deref;
  if (g_yield_self_fallback) { g_self = g_yield_self_fallback; g_self_deref = g_yield_self_deref_fallback; }
  /* A `next` in a yielded block leaves the BLOCK with its value -- but this
     body is spliced inline (no _proc_ function, no loop), so a bare
     `continue` is invalid C. Only when the body owns a `next`, wrap the
     splice in do{}while(0) and route the value through a temp via the
     inline-each next-var machinery; blocks without `next` keep their exact
     previous emission. */
  int nx_own = subtree_has_own_next(nt, bbody);
  const char *sv_nx2 = g_ie_next_var; int sv_poly2 = g_ie_res_poly;
  int sv_lexc2 = g_loop_exc_base;
  char nxbuf[32]; int nx_tmp = 0;
  int bn3 = 0; const int *bd3 = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn3) : NULL;
  TyKind nx_bt = TY_NIL; int nx_tail_stmt = 0;
  if (nx_own) {
    g_loop_exc_base = g_exc_frame_depth;
    g_c_loop_depth++;
    if (as_expr) {
      nx_bt = bn3 > 0 ? comp_ntype(c, bd3[bn3 - 1]) : TY_NIL;
      if (bn3 > 0) {
        const char *tty3 = nt_type(nt, bd3[bn3 - 1]);
        nx_tail_stmt = tty3 && (sp_streq(tty3, "IfNode") || sp_streq(tty3, "CaseNode") ||
                                sp_streq(tty3, "WhileNode") || sp_streq(tty3, "UntilNode") ||
                                sp_streq(tty3, "BeginNode") || sp_streq(tty3, "NextNode") ||
                                sp_streq(tty3, "ReturnNode"));
      }
      nx_tmp = ++g_tmp;
      snprintf(nxbuf, sizeof nxbuf, "_t%d", nx_tmp);
      g_ie_next_var = nxbuf;
      g_ie_res_poly = (nx_bt == TY_POLY);
      if (nx_bt == TY_POLY) buf_printf(b, "sp_RbVal _t%d = sp_box_nil(); ", nx_tmp);
      else if (nx_bt == TY_INT || nx_bt == TY_BOOL || nx_bt == TY_SYMBOL)
        buf_printf(b, "mrb_int _t%d = SP_INT_NIL; ", nx_tmp);
      else if (proc_slot_is_ptr(nx_bt)) {
        emit_ctype(c, nx_bt, b); buf_printf(b, " _t%d = NULL; ", nx_tmp);
      }
      else {
        /* type-opaque tail (e.g. the body IS the `next`): ride the mrb_int
           carrier with the nil sentinel; the next-var stays active so a
           valued `next` still delivers. */
        buf_printf(b, "mrb_int _t%d = SP_INT_NIL; ", nx_tmp);
      }
      buf_puts(b, "do { ");
    }
    else {
      g_ie_next_var = NULL; g_ie_res_poly = 0;
      emit_indent(b, indent); buf_puts(b, "do {\n");
    }
  }
  if (nx_own && as_expr && g_ie_next_var && !nx_tail_stmt && bn3 > 0) {
    for (int k3 = 0; k3 < bn3 - 1; k3++) emit_stmt(c, bd3[k3], b, 0);
    buf_printf(b, "%s = ", nxbuf);
    if (g_ie_res_poly) emit_boxed(c, bd3[bn3 - 1], b);
    else emit_expr(c, bd3[bn3 - 1], b);
    buf_puts(b, "; ");
  }
  else
    emit_stmts(c, bbody, b, as_expr ? 0 : (nx_own ? indent + 1 : indent));
  if (nx_own) {
    g_c_loop_depth--;
    g_loop_exc_base = sv_lexc2;
    if (as_expr) buf_printf(b, "} while(0); %s; ", g_ie_next_var ? nxbuf : "(void)0");
    else { emit_indent(b, indent); buf_puts(b, "} while(0);\n"); }
    g_ie_next_var = sv_nx2; g_ie_res_poly = sv_poly2;
  }
  g_self = sv_bself; g_self_deref = sv_bderef;
  g_method_pr_label = sv_bl; g_method_pr_var = sv_bv; g_ret_type = sv_bt;
  g_method_pr_exc_depth = sv_bexc;
  g_brk_ser_var = svser; g_brk_ensure_base = svebase; g_brk_exc_base = svbexc;
  g_block_brk_var = svbbv; g_block_brk_ebase = svbbe;
  g_block_id = svb; g_block_param_name = svbpn;
  if (as_expr) {
    /* `{ return e }`: the block exits the enclosing function, so the
       statement-expr's tail is unreachable — but C still needs a value
       expression there (a trailing `return;` makes the ({...}) void). */
    int bn2 = 0; const int *bd2 = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn2) : NULL;
    if (bn2 > 0 && nt_type(nt, bd2[bn2 - 1]) &&
        sp_streq(nt_type(nt, bd2[bn2 - 1]), "ReturnNode")) {
      int ra = nt_ref(nt, bd2[bn2 - 1], "arguments");
      int rn = 0; const int *rv = ra >= 0 ? nt_arr(nt, ra, "arguments", &rn) : NULL;
      TyKind rt2 = rn > 0 ? comp_ntype(c, rv[0]) : TY_INT;
      buf_printf(b, " %s;", default_value(is_scalar_ret(rt2) ? rt2 : TY_INT));
    }
    buf_puts(b, "})");
  }
}

/* Inline a yielding method call in expression position: ({ ...; value; }).
   The method must return a usable value (its body's last statement). */
int emit_inline_expr(Compiler *c, int id, Buf *b) {
  /* only when a value is actually produced (scalar return) */
  TyKind rt = comp_ntype(c, id);
  if (!is_scalar_ret(rt)) return 0;
  return emit_inline_call_x(c, id, b, g_indent + 1, 1);
}

/* Block iteration lowered to an inline C for-loop. Handles n.times,
   array.each, range.each, n.upto/downto. Returns 1 if handled. */
/* Emit `lv_<p0> = <expr_src>` boxing if p0 is poly and src is concrete. */
void emit_iter_param_assign(Compiler *c, int block, const char *p0_orig,
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

/* Does the subtree contain a `redo` that belongs to THIS loop, i.e. one not
   nested inside a deeper loop/block/def (which would own it instead)? */
int subtree_has_own_redo(const NodeTable *nt, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "RedoNode")) return 1;
  /* nested scope/loop boundaries: a redo inside binds to that inner loop */
  if (sp_streq(ty, "DefNode") || sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode") ||
      sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode") || sp_streq(ty, "ForNode") ||
      sp_streq(ty, "LambdaNode"))
    return 0;
  if (sp_streq(ty, "CallNode") && nt_ref(nt, id, "block") >= 0) return 0;  /* nested iteration */
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) if (subtree_has_own_redo(nt, nt_ref_at(nt, id, i))) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) if (subtree_has_own_redo(nt, ids[k])) return 1;
  }
  return 0;
}

/* Does the subtree contain a `next` that belongs to THIS block, i.e. one not
   nested inside a deeper loop/block/def (which would own it instead)? Same
   ownership rule as subtree_has_own_redo. */
int subtree_has_own_next(const NodeTable *nt, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "NextNode")) return 1;
  if (sp_streq(ty, "DefNode") || sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode") ||
      sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode") || sp_streq(ty, "ForNode") ||
      sp_streq(ty, "LambdaNode"))
    return 0;
  if (sp_streq(ty, "CallNode") && nt_ref(nt, id, "block") >= 0) return 0;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) if (subtree_has_own_next(nt, nt_ref_at(nt, id, i))) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) if (subtree_has_own_next(nt, ids[k])) return 1;
  }
  return 0;
}

/* Emit a loop body, prefixing a `_redo_N:` label (and pushing it on the redo
   stack) when the body contains a `redo` that targets this loop. The label
   sits at the body top so `redo` re-runs the body without advancing. */
void emit_loop_body(Compiler *c, int body, Buf *b, int indent) {
  /* break/next inside this body exit THIS C loop: record the live
     begin/rescue frame depth at loop entry so their emission can pop the
     frames opened inside the body (mirrors emit_return's accounting). */
  int sv_lexc = g_loop_exc_base;
  g_loop_exc_base = g_exc_frame_depth;
  g_c_loop_depth++;
  int has_redo = subtree_has_own_redo(c->nt, body);
  int lbl = 0;
  if (has_redo) {
    lbl = ++g_tmp;
    if (g_redo_depth < (int)(sizeof g_redo_stack / sizeof g_redo_stack[0]))
      g_redo_stack[g_redo_depth++] = lbl;
    else has_redo = 0;
  }
  if (has_redo) { emit_indent(b, indent); buf_printf(b, "_redo_%d: ;\n", lbl); }
  /* Safepoint poll at the loop back-edge: a threaded program's worker checks
     here whether a GC stop-the-world wants it to park, so a long-running loop
     cannot starve the collector. SP_SAFEPOINT_POLL() (sp_sched.h) is a relaxed
     atomic load of sp_safepoint_flag under SP_THREADS -- the collector writes
     the flag from another thread -- and a plain load otherwise. Emitted only
     when the program uses threads; a non-threaded program is byte-identical.
     At N=1 the flag is never set -- a predicted-not-taken load. */
  if (g_uses_threads) { emit_indent(b, indent); buf_puts(b, "if (SP_UNLIKELY(SP_SAFEPOINT_POLL())) sp_safepoint();\n"); }
  emit_stmts(c, body, b, indent);
  if (has_redo) g_redo_depth--;
  g_c_loop_depth--;
  g_loop_exc_base = sv_lexc;
}

/* `recv.tap { |x| body }` / `recv.then { |x| body }` (alias yield_self) in
   expression position. tap runs the block for its side effect and yields the
   (unchanged) receiver; then yields the block's value. The loop body emits into
   the statement prelude (g_pre); the result temp is the expression value. */
int emit_tap_then_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int is_tap = sp_streq(name, "tap");
  int is_then = sp_streq(name, "then") || sp_streq(name, "yield_self");
  if (!is_tap && !is_then) return 0;
  int block = nt_ref(nt, id, "block");
  if (block < 0 || !nt_type(nt, block) || !sp_streq(nt_type(nt, block), "BlockNode")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind et = comp_ntype(c, recv);
  if (et == TY_UNKNOWN) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0;
  const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (is_then && bn < 1) return 0;  /* then must yield a value */
  const char *p0 = block_param_name(c, block, 0);
  if (p0) p0 = rename_local(p0);

  int tr = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", tr, rb.p ? rb.p : ""); free(rb.p);
  if (needs_root(et)) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tr); }

  /* a then result temp is declared outside the (optional) shadow block so the
     block value escapes it. */
  int tres = 0; TyKind rett = TY_VOID;
  if (is_then) {
    rett = comp_ntype(c, id);
    tres = ++g_tmp;
    emit_indent(g_pre, g_indent); emit_ctype(c, rett, g_pre);
    buf_printf(g_pre, " _t%d = %s;\n", tres, default_value(rett));
    if (needs_root(rett)) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres); }
  }

  /* pin the block param to the receiver type if inference widened it */
  Scope *tsc = p0 ? comp_scope_of(c, block) : NULL;
  LocalVar *tlv0 = (tsc && p0) ? scope_local(tsc, p0) : NULL;
  TyKind tsaved0 = tlv0 ? tlv0->type : TY_UNKNOWN;
  int use_shadow = tlv0 && tlv0->type != et && et != TY_UNKNOWN;
  int din = g_indent;
  if (use_shadow) {
    tlv0->type = et;
    for (int j = 0; j < bn; j++) infer_type(c, bb[j]);
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "{\n");
    din = g_indent + 1;
    emit_indent(g_pre, din); emit_ctype(c, et, g_pre);
    buf_printf(g_pre, " lv_%s = _t%d;\n", p0, tr);
  }
  else if (p0) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "lv_%s = _t%d;\n", p0, tr); }

  int sv = g_indent; g_indent = din;
  int last = is_tap ? bn : bn - 1;
  for (int j = 0; j < last; j++) emit_stmt(c, bb[j], g_pre, din);
  if (is_then) {
    Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb);
    TyKind vt = comp_ntype(c, bb[bn - 1]);
    emit_indent(g_pre, din); buf_printf(g_pre, "_t%d = ", tres);
    if (rett == TY_POLY && vt != TY_POLY) { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, vt, vb.p ? vb.p : "", &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p); }
    else buf_puts(g_pre, vb.p ? vb.p : "");
    buf_puts(g_pre, ";\n"); free(vb.p);
  }
  g_indent = sv;
  if (use_shadow) { emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n"); }
  if (tlv0) tlv0->type = tsaved0;

  buf_printf(b, "_t%d", is_tap ? tr : tres);
  return 1;
}

/* The nil sentinel a block param of type `pt` receives when an auto-splat
   source array has no item at the param's index. Mirrors the proc-literal
   convention in codegen.c (a missing arg binds nil, not a typed zero).
   `pt` is only ever TY_POLY or a scalar slot type (int/bool/float/symbol/
   string) here: these params are bound from poly-container elements, and a
   value-type (Range/Time/Complex/Rational/object value-type) boxes to poly
   inside a container, so it never arrives as a typed struct slot. */
static void emit_block_param_nil(Compiler *c, TyKind pt, Buf *b) {
  (void)c;
  if (pt == TY_POLY)                      buf_puts(b, "sp_box_nil()");
  else if (pt == TY_INT || pt == TY_BOOL) buf_puts(b, "SP_INT_NIL");
  else if (pt == TY_FLOAT)                buf_puts(b, "sp_float_nil()");
  else if (pt == TY_SYMBOL)               buf_puts(b, "((sp_sym)-1)");
  else                                    buf_puts(b, "NULL");  /* string / heap ptr */
}

/* Bind block param `pname` (already renamed) of type `pt` from a boxed
   sp_RbVal source `src`: a poly param takes the box directly; a scalar param
   unboxes down to its slot type. As in emit_block_param_nil, `pt` is TY_POLY
   or a scalar slot type only -- a value-type element is boxed to poly in its
   container, so emit_unbox_text is never asked for a struct-by-value slot. */
static void emit_block_param_from_boxed(Compiler *c, const char *pname, TyKind pt,
                                        const char *src, Buf *b) {
  buf_printf(b, "lv_%s = ", pname);
  if (pt == TY_POLY) buf_puts(b, src);
  else emit_unbox_text(c, pt, src, b);
  buf_puts(b, ";\n");
}

int emit_iteration_stmt(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name) return 0;

  /* loop { ... } -- infinite loop, exited by break */
  if (recv < 0 && sp_streq(name, "loop")) {
    int lbody = nt_ref(nt, block, "body");
    /* Kernel#loop rescues StopIteration to terminate normally (e.g. an external
       Enumerator's #next at the end). Wrap the loop in a setjmp handler; a
       StopIteration falls through, any other exception re-raises. */
    int gcl = ++g_tmp;
    emit_indent(b, indent); buf_printf(b, "int _gcb%d = sp_gc_nroots; (void)_gcb%d;\n", gcl, gcl);
    emit_indent(b, indent); buf_puts(b, "sp_exc_top++;\n");
    emit_indent(b, indent); buf_puts(b, "if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {\n");
    emit_indent(b, indent + 1); buf_puts(b, "for (;;) {\n");
    emit_loop_body(c, lbody, b, indent + 2);
    emit_indent(b, indent + 1); buf_puts(b, "}\n");
    emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "else {\n");
    emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
    emit_indent(b, indent + 1); buf_printf(b, "sp_gc_nroots = _gcb%d;\n", gcl);
    /* a non-local unwind (throw / valued break) lands here only because this
       frame sits between the thrower and its target -- pass it through, like
       every begin/rescue handler does. */
    emit_indent(b, indent + 1);
    buf_puts(b, "if (sp_unwind_kind != SP_UNWIND_NONE) sp_unwind_resume();\n");
    emit_indent(b, indent + 1);
    buf_puts(b, "if (!sp_exc_cls_matches((const char *)sp_last_exc_cls, \"StopIteration\")) sp_raise_cls(sp_exc_cls[sp_exc_top], sp_exc_msg[sp_exc_top]);\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  if (recv < 0) return 0;
  int body = nt_ref(nt, block, "body");
  const char *p0_orig = block_param_name(c, block, 0);
  const char *p0 = p0_orig ? rename_local(p0_orig) : NULL;
  TyKind rt = comp_ntype(c, recv);

  /* (range).step(k) { |x| ... } -- materialise the stepped values (shared with
     the no-block path so they match exactly) and walk them; the element type
     follows the array, int or float. */
  if (sp_streq(name, "step") && rt == TY_RANGE) {
    int args = nt_ref(nt, id, "arguments"); int sargc = 0;
    if (args >= 0) nt_arr(nt, args, "arguments", &sargc);
    if (sargc < 1) return 0;
    int t = ++g_tmp, ti = ++g_tmp;
    Buf ab; memset(&ab, 0, sizeof ab);
    TyKind at = emit_range_step_array(c, id, &ab);
    const char *aty = at == TY_FLOAT_ARRAY ? "sp_FloatArray" : "sp_IntArray";
    TyKind et = at == TY_FLOAT_ARRAY ? TY_FLOAT : TY_INT;
    emit_indent(b, indent);
    buf_printf(b, "%s *_t%d = %s; SP_GC_ROOT(_t%d);\n", aty, t, ab.p ? ab.p : "", t);
    free(ab.p);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, t, ti);
    if (p0) {
      char elem[64]; snprintf(elem, sizeof elem, "_t%d->data[_t%d]", t, ti);
      emit_iter_param_assign(c, block, p0_orig, p0, et, elem, b, indent + 1);
    }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* n.times { |i| ... } */
  if (sp_streq(name, "times") && rt == TY_INT) {
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb);
    emit_expr(c, recv, &rb);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < ", t, t);
    buf_puts(b, rb.p); buf_printf(b, "; _t%d++) {\n", t);
    if (p0) { char ts[32]; snprintf(ts, sizeof ts, "_t%d", t); emit_iter_param_assign(c, block, p0_orig, p0, TY_INT, ts, b, indent + 1); }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    free(rb.p);
    return 1;
  }

  /* num.step(limit[, step]) { [|i|] ... } -- stepping loop. A float receiver
     or a float limit/step makes it a float walk (yielding floats), computed
     by iteration count to avoid floating-point drift (CRuby semantics). */
  if (sp_streq(name, "step") && (rt == TY_INT || rt == TY_FLOAT)) {
    int args = nt_ref(nt, id, "arguments");
    int sargc = 0;
    const int *sargv = args >= 0 ? nt_arr(nt, args, "arguments", &sargc) : NULL;
    if (sargc < 1) return 0;
    int is_float = (rt == TY_FLOAT) || comp_ntype(c, sargv[0]) == TY_FLOAT ||
                   (sargc >= 2 && comp_ntype(c, sargv[1]) == TY_FLOAT);
    if (!is_float) {
      int t = ++g_tmp, tl = ++g_tmp, ts = ++g_tmp;
      emit_indent(b, indent); buf_printf(b, "mrb_int _t%d = ", tl); emit_int_expr(c, sargv[0], b); buf_puts(b, ";\n");
      emit_indent(b, indent); buf_printf(b, "mrb_int _t%d = ", ts);
      if (sargc >= 2) emit_expr(c, sargv[1], b); else buf_puts(b, "1");
      buf_puts(b, ";\n");
      emit_indent(b, indent);
      buf_printf(b, "for (mrb_int _t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; _t%d >= 0 ? _t%d <= _t%d : _t%d >= _t%d; _t%d += _t%d) {\n",
                 ts, t, tl, t, tl, t, ts);
      if (p0) { char ts2[32]; snprintf(ts2, sizeof ts2, "_t%d", t); emit_iter_param_assign(c, block, p0_orig, p0, TY_INT, ts2, b, indent + 1); }
      emit_loop_body(c, body, b, indent + 1);
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
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* hash.each / each_pair { |k, v| ... } */
  if ((sp_streq(name, "each") || sp_streq(name, "each_pair")) && ty_is_hash(rt)) {
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
      /* The param may be poly (a name shared across hashes of differing element
         types); box a concrete key into the poly slot. */
      const char *raw0 = block_param_name(c, block, 0);
      LocalVar *pv0 = raw0 ? scope_local(comp_scope_of(c, block), raw0) : NULL;
      TyKind want0 = ty_hash_key(rt);
      int box0 = pv0 && pv0->type == TY_POLY && want0 != TY_POLY;
      char src0[256];
      if (rt == TY_POLY_POLY_HASH)
        snprintf(src0, sizeof src0, "%s->keys[%s->order[_t%d]]", rb.p, rb.p, t);
      else
        snprintf(src0, sizeof src0, "%s->order[_t%d]", rb.p, t);
      emit_indent(b, indent + 1);
      buf_printf(b, "lv_%s = ", p0);
      if (box0) emit_boxed_text(c, want0, src0, b); else buf_puts(b, src0);
      buf_puts(b, ";\n");
    }
    if (p1) {
      const char *raw1 = block_param_name(c, block, 1);
      LocalVar *pv1 = raw1 ? scope_local(comp_scope_of(c, block), raw1) : NULL;
      TyKind want1 = ty_hash_val(rt);
      int box1 = pv1 && pv1->type == TY_POLY && want1 != TY_POLY;
      char src1[256];
      if (rt == TY_POLY_POLY_HASH)
        snprintf(src1, sizeof src1, "%s->vals[%s->order[_t%d]]", rb.p, rb.p, t);
      else
        snprintf(src1, sizeof src1, "sp_%sHash_get(%s, %s->order[_t%d])", hn, rb.p, rb.p, t);
      emit_indent(b, indent + 1);
      buf_printf(b, "lv_%s = ", p1);
      if (box1) emit_boxed_text(c, want1, src1, b); else buf_puts(b, src1);
      buf_puts(b, ";\n");
    }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    free(rb.p);
    return 1;
  }

  /* hash.each_value { |v| ... } / each_key { |k| ... } -- single param */
  if ((sp_streq(name, "each_value") || sp_streq(name, "each_key")) && ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    if (!hn) return 0;
    int is_val = sp_streq(name, "each_value");
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
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    free(rb.p);
    return 1;
  }

  /* hash.delete_if / reject! / select! / filter! / keep_if { |k, v| cond } */
  if ((sp_streq(name, "delete_if") || sp_streq(name, "reject!") || sp_streq(name, "select!") ||
       sp_streq(name, "filter!") || sp_streq(name, "keep_if")) && ty_is_hash(rt) && block >= 0) {
    const char *hn = ty_hash_cname(rt);
    if (hn && rt != TY_POLY_POLY_HASH) {
      int is_rej = (sp_streq(name, "delete_if") || sp_streq(name, "reject!"));
      const char *p0_raw = block_param_name(c, block, 0);
      const char *p1_raw = block_param_name(c, block, 1);
      const char *kp = p0_raw ? rename_local(p0_raw) : NULL;
      const char *vp = p1_raw ? rename_local(p1_raw) : NULL;
      int body2 = nt_ref(nt, block, "body");
      int bn2 = 0; const int *bb2 = body2 >= 0 ? nt_arr(nt, body2, "body", &bn2) : NULL;
      if (bn2 >= 1) {
        Scope *hs2 = comp_scope_of(c, block);
        TyKind hkt = ty_hash_key(rt), hvt = ty_hash_val(rt);
        /* Temporarily set block param types to hash key/value types so
           the block body is emitted with the right concrete types.
           Refresh the ntype cache with the new types. */
        LocalVar *klv = (kp && hs2) ? scope_local(hs2, p0_raw) : NULL;
        LocalVar *vlv = (vp && hs2) ? scope_local(hs2, p1_raw) : NULL;
        TyKind ksaved = klv ? klv->type : TY_UNKNOWN;
        TyKind vsaved = vlv ? vlv->type : TY_UNKNOWN;
        if (klv) klv->type = hkt;
        if (vlv) vlv->type = hvt;
        for (int j = 0; j < bn2; j++) infer_type(c, bb2[j]);
        int tr2 = ++g_tmp, ti2 = ++g_tmp;
        Buf rb2; memset(&rb2, 0, sizeof rb2); emit_expr(c, recv, &rb2);
        emit_indent(b, indent); emit_ctype(c, rt, b);
        buf_printf(b, "_t%d = %s;\n", tr2, rb2.p ? rb2.p : "NULL"); free(rb2.p);
        emit_indent(b, indent);
        buf_printf(b, "if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash();\n", tr2);
        emit_indent(b, indent);
        buf_printf(b, "for (mrb_int _t%d = 0; _t%d && _t%d < _t%d->len; ) {\n",
                   ti2, tr2, ti2, tr2);
        /* declare + assign key param with hash key type (shadows outer lv_k) */
        if (kp) {
          emit_indent(b, indent + 1); emit_ctype(c, hkt, b);
          buf_printf(b, " lv_%s = _t%d->order[_t%d];\n", kp, tr2, ti2);
        }
        /* declare + assign value param with hash value type (shadows outer lv_v) */
        if (vp) {
          emit_indent(b, indent + 1); emit_ctype(c, hvt, b);
          buf_printf(b, " lv_%s = sp_%sHash_get(_t%d, _t%d->order[_t%d]);\n",
                     vp, hn, tr2, tr2, ti2);
        }
        /* block body stmts except last */
        for (int j = 0; j < bn2 - 1; j++) emit_stmt(c, bb2[j], b, indent + 1);
        /* condition: save g_pre so pre-effects land inside the loop body */
        Buf *sp2 = g_pre; int si2 = g_indent;
        Buf cpre2; memset(&cpre2, 0, sizeof cpre2); g_pre = &cpre2; g_indent = indent + 1;
        Buf cexpr2; memset(&cexpr2, 0, sizeof cexpr2);
        emit_expr(c, bb2[bn2 - 1], &cexpr2);
        g_pre = sp2; g_indent = si2;
        if (cpre2.p) { buf_puts(b, cpre2.p); free(cpre2.p); }
        emit_indent(b, indent + 1);
        if (is_rej) buf_printf(b, "if (%s) {\n", cexpr2.p ? cexpr2.p : "0");
        else        buf_printf(b, "if (!(%s)) {\n", cexpr2.p ? cexpr2.p : "0");
        free(cexpr2.p);
        emit_indent(b, indent + 2);
        buf_printf(b, "sp_%sHash_delete(_t%d, _t%d->order[_t%d]);\n", hn, tr2, tr2, ti2);
        emit_indent(b, indent + 1); buf_puts(b, "} else {\n");
        emit_indent(b, indent + 2); buf_printf(b, "_t%d++;\n", ti2);
        emit_indent(b, indent + 1); buf_puts(b, "}\n");
        emit_indent(b, indent); buf_puts(b, "}\n");
        /* restore block param types */
        if (klv) klv->type = ksaved;
        if (vlv) vlv->type = vsaved;
        return 1;
      }
    }
  }

  /* array.each_with_index { |x, i| ... } */
  if (sp_streq(name, "each_with_index") && ty_is_array(rt)) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) return 0;
    const char *p1 = block_param_name(c, block, 1); if (p1) p1 = rename_local(p1);
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    Scope *cs_ewi = comp_scope_of(c, id);
    LocalVar *clv_ewi_p1 = (p1 && cs_ewi) ? scope_local(cs_ewi, p1) : NULL;
    LocalVar *clv_ewi_p0 = (p0 && cs_ewi) ? scope_local(cs_ewi, p0) : NULL;
    TyKind ewi_et = ty_array_elem(rt);
    int p0_box_poly = clv_ewi_p0 && clv_ewi_p0->type == TY_POLY && ewi_et != TY_POLY;
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
      if (p0_box_poly) {
        char src[512]; snprintf(src, sizeof src, "sp_%sArray_get(%s, _t%d)", k, rb.p ? rb.p : "NULL", t);
        buf_printf(b, "lv_%s = ", p0); emit_boxed_text(c, ewi_et, src, b); buf_puts(b, ";\n");
      }
      else {
        buf_printf(b, "lv_%s = sp_%sArray_get(", p0, k);
        buf_puts(b, rb.p); buf_printf(b, ", _t%d);\n", t);
      }
    }
    if (p1) {
      emit_indent(b, indent + 1);
      if (p1_box_poly) buf_printf(b, "lv_%s = sp_box_int(_t%d);\n", p1, t);
      else buf_printf(b, "lv_%s = _t%d;\n", p1, t);
    }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    /* Restore outer variables */
    if (p0 && ts_p0 > 0) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p0, ts_p0); }
    if (p1 && ts_p1 > 0) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p1, ts_p1); }
    free(rb.p);
    return 1;
  }

  /* array.zip(other) { |a, b| ... } — block form, returns nil */
  if (sp_streq(name, "zip") && ty_is_array(rt) && block >= 0) {
    int zargs_n = nt_ref(nt, id, "arguments");
    int zargc = 0; const int *zargv = zargs_n >= 0 ? nt_arr(nt, zargs_n, "arguments", &zargc) : NULL;
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (k && zargc == 1 && zargv) {
      TyKind a0t = comp_ntype(c, zargv[0]);
      const char *k2 = ty_is_array(a0t) ? ((a0t == TY_POLY_ARRAY) ? "Poly" : array_kind(a0t)) : NULL;
      if (!k2) k2 = k;
      TyKind et = ty_array_elem(rt);
      TyKind et2 = ty_is_array(a0t) ? ty_array_elem(a0t) : et;
      const char *p1n = block_param_name(c, block, 1); if (p1n) p1n = rename_local(p1n);
      int t = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
      Buf ob; memset(&ob, 0, sizeof ob); emit_expr(c, zargv[0], &ob);
      Scope *zs = comp_scope_of(c, id);
      LocalVar *zlv0 = (p0 && zs) ? scope_local(zs, p0) : NULL;
      LocalVar *zlv1 = (p1n && zs) ? scope_local(zs, p1n) : NULL;
      int zs0 = 0, zs1 = 0;
      if (p0 && zlv0) {
        zs0 = ++g_tmp; Buf ot; memset(&ot, 0, sizeof ot); emit_ctype(c, zlv0->type, &ot);
        emit_indent(b, indent); buf_printf(b, "%s _t%d = lv_%s;\n", ot.p ? ot.p : "sp_RbVal", zs0, p0); free(ot.p);
      }
      if (p1n && zlv1) {
        zs1 = ++g_tmp; Buf ot; memset(&ot, 0, sizeof ot); emit_ctype(c, zlv1->type, &ot);
        emit_indent(b, indent); buf_printf(b, "%s _t%d = lv_%s;\n", ot.p ? ot.p : "sp_RbVal", zs1, p1n); free(ot.p);
      }
      emit_indent(b, indent);
      buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(%s); _t%d++) {\n",
                 t, t, k, rb.p ? rb.p : "NULL", t);
      if (p0 && zlv0) {
        char src[512]; snprintf(src, sizeof src, "sp_%sArray_get(%s, _t%d)", k, rb.p ? rb.p : "NULL", t);
        int box0 = zlv0->type == TY_POLY && et != TY_POLY;
        emit_indent(b, indent + 1); buf_printf(b, "lv_%s = ", p0);
        if (box0) emit_boxed_text(c, et, src, b);
        else buf_puts(b, src);
        buf_puts(b, ";\n");
      }
      if (p1n && zlv1 && ob.p) {
        char src2[512]; snprintf(src2, sizeof src2, "sp_%sArray_get(%s, _t%d)", k2, ob.p, t);
        int box1 = zlv1->type == TY_POLY && et2 != TY_POLY;
        emit_indent(b, indent + 1); buf_printf(b, "lv_%s = ", p1n);
        if (box1) emit_boxed_text(c, et2, src2, b);
        else buf_puts(b, src2);
        buf_puts(b, ";\n");
      }
      emit_loop_body(c, body, b, indent + 1);
      emit_indent(b, indent); buf_puts(b, "}\n");
      if (p0 && zs0 > 0) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p0, zs0); }
      if (p1n && zs1 > 0) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p1n, zs1); }
      free(rb.p); free(ob.p);
      return 1;
    }
  }

  /* poly_val.each { |v| ... }: runtime-dispatch over a boxed array or hash */
  if (sp_streq(name, "each") && rt == TY_POLY && block >= 0) {
    int ta = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); buf_printf(b, "sp_RbVal _t%d = %s;\n", ta, rb.p ? rb.p : "sp_box_nil()"); free(rb.p);
    /* Root the boxed receiver so a GC fired by the loop body doesn't free a
       freshly-built collection held only by this temp. */
    emit_indent(b, indent); buf_printf(b, "SP_GC_ROOT_RBVAL(_t%d);\n", ta);
    emit_indent(b, indent); buf_printf(b, "mrb_int _t%d = sp_poly_arr_len_ex(_t%d);\n", tn, ta);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) {\n", ti, ti, tn, ti);
    /* multi-param: auto-splat each poly element into params. Ruby splats only
       when the element is itself an Array (sp_poly_each_elem already renders a
       hash pair as a 2-element array, so |k, v| over a hash still splats); a
       non-array element binds param 0 with the rest nil. */
    int npp_poly = 0; while (block_param_name(c, block, npp_poly)) npp_poly++;
    if (npp_poly >= 2) {
      Scope *blk_pv = comp_scope_of(c, block);
      int telem = ++g_tmp;
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_RbVal _t%d = sp_poly_each_elem(_t%d, _t%d);\n", telem, ta, ti);
      emit_indent(b, indent + 1);
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && SP_IS_BUILTIN_ARRAY(_t%d.cls_id)) {\n", telem, telem);
      for (int pj = 0; pj < npp_poly; pj++) {
        const char *pnj = block_param_name(c, block, pj);
        if (!pnj) break;
        LocalVar *plv = blk_pv ? scope_local(blk_pv, pnj) : NULL;
        TyKind pt = plv ? plv->type : TY_POLY;
        char src[64]; snprintf(src, sizeof src, "sp_poly_arr_get(_t%d, %d)", telem, pj);
        emit_indent(b, indent + 2);
        emit_block_param_from_boxed(c, rename_local(pnj), pt, src, b);
      }
      emit_indent(b, indent + 1); buf_puts(b, "} else {\n");
      for (int pj = 0; pj < npp_poly; pj++) {
        const char *pnj = block_param_name(c, block, pj);
        if (!pnj) break;
        LocalVar *plv = blk_pv ? scope_local(blk_pv, pnj) : NULL;
        TyKind pt = plv ? plv->type : TY_POLY;
        emit_indent(b, indent + 2);
        if (pj == 0) {
          char src[32]; snprintf(src, sizeof src, "_t%d", telem);
          emit_block_param_from_boxed(c, rename_local(pnj), pt, src, b);
        } else {
          buf_printf(b, "lv_%s = ", rename_local(pnj));
          emit_block_param_nil(c, pt, b);
          buf_puts(b, ";\n");
        }
      }
      emit_indent(b, indent + 1); buf_puts(b, "}\n");
    }
    else if (p0) {
      emit_indent(b, indent + 1);
      buf_printf(b, "lv_%s = sp_poly_each_elem(_t%d, _t%d);\n", p0, ta, ti);
    }
    /* a paramless block (`each { ... }`) binds nothing; the loop still runs the
       body once per element for its side effect. */
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* array.each { |x| ... } */
  if (sp_streq(name, "each") && rt == TY_POLY_ARRAY) {
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
    /* Root the receiver: a freshly-built array referenced only by this temp
       is otherwise freed if the loop body triggers GC mid-iteration, leaving
       the next element fetch dangling. */
    emit_indent(b, indent);
    buf_printf(b, "SP_GC_ROOT(_t%d);\n", ta);
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
      /* Poly-param auto-splat: a 2+ param block whose params weren't proven to
         be a typed inner array (so they're poly/unknown). Ruby auto-splats each
         element ONLY when it is itself an Array -- destructure item k into param
         k (missing item -> nil); a non-array element binds param 0, rest nil. */
      if (!did_destruct && npp >= 2) {
        Scope *blk_sp2 = comp_scope_of(c, block);
        int telem = ++g_tmp;
        emit_indent(b, indent + 1);
        buf_printf(b, "sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);\n", telem, ta, t);
        emit_indent(b, indent + 1);
        buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && SP_IS_BUILTIN_ARRAY(_t%d.cls_id)) {\n", telem, telem);
        for (int pj = 0; pj < npp; pj++) {
          const char *pnj = block_param_name(c, block, pj);
          if (!pnj) break;
          LocalVar *plv = blk_sp2 ? scope_local(blk_sp2, pnj) : NULL;
          TyKind pt = plv ? plv->type : TY_POLY;
          char src[64]; snprintf(src, sizeof src, "sp_poly_arr_get(_t%d, %d)", telem, pj);
          emit_indent(b, indent + 2);
          emit_block_param_from_boxed(c, rename_local(pnj), pt, src, b);
        }
        emit_indent(b, indent + 1); buf_puts(b, "} else {\n");
        for (int pj = 0; pj < npp; pj++) {
          const char *pnj = block_param_name(c, block, pj);
          if (!pnj) break;
          LocalVar *plv = blk_sp2 ? scope_local(blk_sp2, pnj) : NULL;
          TyKind pt = plv ? plv->type : TY_POLY;
          emit_indent(b, indent + 2);
          if (pj == 0) {
            char src[32]; snprintf(src, sizeof src, "_t%d", telem);
            emit_block_param_from_boxed(c, rename_local(pnj), pt, src, b);
          } else {
            buf_printf(b, "lv_%s = ", rename_local(pnj));
            emit_block_param_nil(c, pt, b);
            buf_puts(b, ";\n");
          }
        }
        emit_indent(b, indent + 1); buf_puts(b, "}\n");
        did_destruct = 1;
      }
      if (!did_destruct) {
        emit_indent(b, indent + 1);
        buf_printf(b, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", p0, ta, t);
      }
    }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    if (outer_pa) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p0, ts_pa); }
    return 1;
  }
  if ((sp_streq(name, "each") || sp_streq(name, "each_entry") || sp_streq(name, "reverse_each")) &&
      ty_is_array(rt)) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) return 0;
    int rev = sp_streq(name, "reverse_each");
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
      if (np >= 2 && sp_streq(k, "Poly") && bp0_type != TY_POLY && bp0_type != TY_UNKNOWN) {
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
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    if (outer) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p0, ts); }
    free(rb.p);
    return 1;
  }

  /* int_array.combination(k)/permutation(k) { |c| ... } -- yield each k-element
     sub-array as a fresh int_array. permutation also accepts the argless
     (full-length) form. */
  if ((sp_streq(name, "combination") || sp_streq(name, "permutation")) && rt == TY_INT_ARRAY) {
    int is_perm = sp_streq(name, "permutation");
    const char *genfn = is_perm ? "sp_IntArray_permutation" : "sp_IntArray_combination";
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (ac != 1 && !(is_perm && ac == 0)) return 0;
    int ta = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); buf_printf(b, "{ sp_IntArray *_t%d = ", ta); buf_puts(b, rb.p ? rb.p : ""); buf_puts(b, ";\n"); free(rb.p);
    emit_indent(b, indent + 1); buf_printf(b, "sp_PtrArray *_t%d = %s(_t%d, ", tc, genfn, ta);
    if (ac == 1) emit_expr(c, av[0], b); else buf_printf(b, "_t%d ? _t%d->len : 0", ta, ta);
    buf_puts(b, "); SP_GC_ROOT(_t"); buf_printf(b, "%d);\n", tc);
    emit_indent(b, indent + 1); buf_printf(b, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, tc, ti);
    if (p0) {
      Scope *cbsc = comp_scope_of(c, block);
      LocalVar *clv = cbsc ? scope_local(cbsc, p0) : NULL;
      TyKind cpt = clv ? clv->type : TY_UNKNOWN;
      emit_indent(b, indent + 2);
      if (cpt == TY_POLY || cpt == TY_UNKNOWN)
        buf_printf(b, "lv_%s = sp_box_obj((sp_IntArray *)_t%d->data[_t%d], SP_BUILTIN_INT_ARRAY);\n", p0, tc, ti);
      else
        buf_printf(b, "lv_%s = (sp_IntArray *)_t%d->data[_t%d];\n", p0, tc, ti);
    }
    emit_loop_body(c, body, b, indent + 2);
    emit_indent(b, indent + 1); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* array.each_cons(n) { |a, b, ...| } -- sliding window of n consecutive
     elements; a single param binds the n-element sub-array, multiple params
     destructure the window */
  if (sp_streq(name, "each_cons") && ty_is_array(rt)) {
    int args = nt_ref(nt, id, "arguments");
    int ec = 0; const int *eav = args >= 0 ? nt_arr(nt, args, "arguments", &ec) : NULL;
    if (ec != 1) return 0;
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) return 0;
    int np = 0; while (block_param_name(c, block, np)) np++;
    int ta = ++g_tmp, tnn = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); emit_ctype(c, rt, b); buf_printf(b, " _t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
    emit_indent(b, indent); buf_printf(b, "mrb_int _t%d = ", tnn); emit_int_expr(c, eav[0], b); buf_puts(b, ";\n");
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
        emit_loop_body(c, body, b, indent + 2);
        emit_indent(b, indent + 1); buf_puts(b, "}\n");
        clv_ec->type = csaved_ec;
      }
      else {
        emit_indent(b, indent + 1);
        buf_printf(b, "lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n", rpn, k, ta, ti, tnn);
        emit_loop_body(c, body, b, indent + 1);
      }
    }
    else {
      for (int pj = 0; pj < np; pj++) {
        const char *pn = block_param_name(c, block, pj);
        emit_indent(b, indent + 1);
        buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d + %d);\n", rename_local(pn), k, ta, ti, pj);
      }
      emit_loop_body(c, body, b, indent + 1);
    }
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* ("a".."e").each { |s| ... } -- a string-endpoint range has no int sp_Range
     representation, so materialize the succ-sequence as a StrArray and loop over
     it. The block param is shadow-typed String for the body. */
  if (sp_streq(name, "each") && rt == TY_RANGE && p0) {
    int rnode = unwrap_parens(c, recv);
    if (rnode >= 0 && nt_type(nt, rnode) && sp_streq(nt_type(nt, rnode), "RangeNode")) {
      int lo = nt_ref(nt, rnode, "left"), hi = nt_ref(nt, rnode, "right");
      if (lo >= 0 && hi >= 0 && comp_ntype(c, lo) == TY_STRING && comp_ntype(c, hi) == TY_STRING) {
        int excl = (int)(nt_int(nt, rnode, "flags", 0) & 4) ? 1 : 0;
        int ta = ++g_tmp, ti = ++g_tmp;
        emit_indent(b, indent);
        buf_printf(b, "sp_StrArray *_t%d = sp_StrArray_from_string_range(", ta);
        emit_expr(c, lo, b); buf_puts(b, ", "); emit_expr(c, hi, b); buf_printf(b, ", %d);\n", excl);
        /* Root the materialized array: the loop body can allocate (and trigger
           GC), which would otherwise sweep it out from under sp_StrArray_get. */
        emit_indent(b, indent); buf_printf(b, "SP_GC_ROOT(_t%d);\n", ta);
        Scope *ssc = comp_scope_of(c, block);
        LocalVar *slv = (ssc && p0_orig) ? scope_local(ssc, p0_orig) : NULL;
        TyKind saved = slv ? slv->type : TY_UNKNOWN;
        int use_shadow = slv && slv->type != TY_STRING;
        emit_indent(b, indent);
        buf_printf(b, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, ta, ti);
        if (use_shadow) {
          int sbn = 0; const int *sbb = body >= 0 ? nt_arr(nt, body, "body", &sbn) : NULL;
          slv->type = TY_STRING;
          for (int j = 0; j < sbn; j++) infer_type(c, sbb[j]);
          emit_indent(b, indent + 1);
          buf_printf(b, "const char *lv_%s = sp_StrArray_get(_t%d, _t%d);\n", p0, ta, ti);
          emit_loop_body(c, body, b, indent + 1);
          slv->type = saved;
        }
        else {
          emit_indent(b, indent + 1);
          buf_printf(b, "lv_%s = sp_StrArray_get(_t%d, _t%d);\n", p0, ta, ti);
          emit_loop_body(c, body, b, indent + 1);
        }
        emit_indent(b, indent); buf_puts(b, "}\n");
        return 1;
      }
    }
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent);
    buf_printf(b, "sp_Range _t%d = ", t); buf_puts(b, rb.p ? rb.p : ""); buf_puts(b, ";\n");
    free(rb.p);
    /* Under --int-overflow=promote the loop var is widened to poly; drive the
       loop with a fresh mrb_int temp and re-box the counter each iteration
       (mirrors emit_for's poly-counter arm). */
    LocalVar *clv = p0_orig ? scope_local(comp_scope_of(c, block), p0_orig) : NULL;
    /* Direction-aware bounds: a descending range (n.downto(m)) walks by its
       negative step, which the plain ascending loop would skip entirely. */
    int ts = ++g_tmp, te = ++g_tmp;
    emit_indent(b, indent);
    buf_printf(b, "mrb_int _t%d = sp_range_step(_t%d); mrb_int _t%d = _t%d.last - (_t%d.excl ? (_t%d > 0 ? 1 : -1) : 0);\n",
               ts, t, te, t, t, ts);
    if (clv && clv->type == TY_POLY) {
      int tc = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "for (mrb_int _t%d = _t%d.first; _t%d > 0 ? _t%d <= _t%d : _t%d >= _t%d; _t%d += _t%d) {\n",
                 tc, t, ts, tc, te, tc, te, tc, ts);
      emit_indent(b, indent + 1);
      buf_printf(b, "lv_%s = sp_box_int(_t%d);\n", p0, tc);
      emit_loop_body(c, body, b, indent + 1);
      emit_indent(b, indent); buf_puts(b, "}\n");
      return 1;
    }
    emit_indent(b, indent);
    buf_printf(b, "for (lv_%s = _t%d.first; _t%d > 0 ? lv_%s <= _t%d : lv_%s >= _t%d; lv_%s += _t%d) {\n",
               p0, t, ts, p0, te, p0, te, p0, ts);
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* n.upto(m) / n.downto(m) { [|i|] ... } -- a fresh temp drives the loop and
     the block param (if any) is rebound from it each iteration, like n.times.
     A blockless-param form (`1.upto(5) { body }`) must still run the body. */
  if ((sp_streq(name, "upto") || sp_streq(name, "downto")) && rt == TY_INT) {
    int up = sp_streq(name, "upto");
    int args = nt_ref(nt, id, "arguments");
    int argc = 0;
    const int *argv = NULL;
    if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
    if (argc != 1) return 0;
    Buf lo; memset(&lo, 0, sizeof lo); emit_expr(c, recv, &lo);
    Buf hi; memset(&hi, 0, sizeof hi); emit_expr(c, argv[0], &hi);
    int ti = ++g_tmp;
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = ", ti); buf_puts(b, lo.p);
    buf_printf(b, "; _t%d %s ", ti, up ? "<=" : ">="); buf_puts(b, hi.p);
    buf_printf(b, "; _t%d%s) {\n", ti, up ? "++" : "--");
    if (p0) { char ts[32]; snprintf(ts, sizeof ts, "_t%d", ti); emit_iter_param_assign(c, block, p0_orig, p0, TY_INT, ts, b, indent + 1); }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    free(lo.p); free(hi.p);
    return 1;
  }

  /* "a".upto("e") { |c| ... } -- string succ-sequence loop, mirrors
     sp_StrArray_from_string_range semantics (inclusive, 4096-cap) */
  if (sp_streq(name, "upto") && rt == TY_STRING && p0) {
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
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent + 1); buf_printf(b, "if (_t%d == 0) break;\n", tcmp);
    emit_indent(b, indent + 1); buf_printf(b, "_t%d = sp_str_succ(_t%d);\n", tc, tc);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* recv.tap { |p| body } -- run block for side effects, preserve outer var */
  if (sp_streq(name, "tap") && recv >= 0) {
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
      emit_loop_body(c, body, b, indent + 1);
      emit_indent(b, indent); buf_puts(b, "}\n");
      tlv0->type = tsaved0;
    }
    else {
      if (p0) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p0, tr); }
      emit_loop_body(c, body, b, indent);
    }
    return 1;
  }

  /* array.cycle(n) { |p| body } -- repeat n times over the array */
  if (sp_streq(name, "cycle") && ty_is_array(rt)) {
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
      emit_loop_body(c, body, b, innerIndent);
      innerIndent--;
      emit_indent(b, innerIndent); buf_puts(b, "}\n");
      clv0->type = csaved0;
    }
    else {
      if (p0) { emit_indent(b, innerIndent); buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, ta, tj); }
      emit_loop_body(c, body, b, innerIndent);
    }
    emit_indent(b, indent + 1); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* array.each_slice(n) { |p| body } -- yield subarrays of size n */
  if (sp_streq(name, "each_slice") && ty_is_array(rt)) {
    int args = nt_ref(nt, id, "arguments");
    int es_argc = 0; const int *es_argv = args >= 0 ? nt_arr(nt, args, "arguments", &es_argc) : NULL;
    if (es_argc != 1) return 0;
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) return 0;
    int np_es = 0; while (block_param_name(c, block, np_es)) np_es++;
    Scope *csc = p0 ? comp_scope_of(c, block) : NULL;
    LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
    TyKind csaved0 = clv0 ? clv0->type : TY_UNKNOWN;
    int use_shadow_es = np_es == 1 && clv0 && clv0->type != rt && rt != TY_UNKNOWN;
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
    if (np_es > 1) {
      /* multi-param: destructure slice elements into individual params */
      for (int pj = 0; pj < np_es; pj++) {
        const char *pn = block_param_name(c, block, pj);
        if (!pn) break;
        emit_indent(b, bodyIndent);
        buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d + %d);\n", rename_local(pn), k, ta, ti, pj);
      }
      emit_loop_body(c, body, b, bodyIndent);
    }
    else if (use_shadow_es) {
      int esb_bn = 0; const int *esb_bb = body >= 0 ? nt_arr(nt, body, "body", &esb_bn) : NULL;
      clv0->type = rt;
      for (int j = 0; j < esb_bn; j++) infer_type(c, esb_bb[j]);
      emit_indent(b, bodyIndent); buf_puts(b, "{\n"); bodyIndent++;
      emit_indent(b, bodyIndent); emit_ctype(c, rt, b);
      buf_printf(b, " lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n", p0, k, ta, ti, ts);
      emit_loop_body(c, body, b, bodyIndent);
      bodyIndent--;
      emit_indent(b, bodyIndent); buf_puts(b, "}\n");
      clv0->type = csaved0;
    }
    else {
      if (p0) { emit_indent(b, bodyIndent); buf_printf(b, "lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n", p0, k, ta, ti, ts); }
      emit_loop_body(c, body, b, bodyIndent);
    }
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* str.scan(/re/) { |m| body } -- iterate over regex matches */
  if (sp_streq(name, "scan") && rt == TY_STRING) {
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
      emit_loop_body(c, body, b, bodyIndent);
      bodyIndent--;
      emit_indent(b, bodyIndent); buf_puts(b, "}\n");
      clv0->type = csaved0;
    }
    else {
      if (p0) { emit_indent(b, bodyIndent); buf_printf(b, "lv_%s = sp_StrArray_get(_t%d, _t%d);\n", p0, tm, ti); }
      emit_loop_body(c, body, b, bodyIndent);
    }
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  return 0;
}

/* ---- interpolation ---- */

