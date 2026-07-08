#include "codegen_internal.h"

/* Defined lower in this file; declared here so the collecting emitters above
   its definition (the hash block-walk binder, flat_map) can route a block's
   next-aware value into a caller-declared lvalue. */
static void emit_block_value_into(Compiler *c, int block, const char *dest,
                                  int want_poly, int indent);

int resolve_forwarded_block(Compiler *c, int block) {
  const NodeTable *nt = c->nt;
  if (block < 0) return block;
  const char *type = nt_type(nt, block);
  if (!type || !sp_streq(type, "BlockArgumentNode")) return block;
  int fwd_expr = nt_ref(nt, block, "expression");
  int forwards_param = 0;
  if (fwd_expr < 0) {
    forwards_param = 1;  /* anonymous `&` */
  } else if (g_block_param_name) {
    const char *fwd_type = nt_type(nt, fwd_expr);
    if (fwd_type && sp_streq(fwd_type, "LocalVariableReadNode")) {
      const char *en = nt_str(nt, fwd_expr, "name");
      forwards_param = en && sp_streq(en, g_block_param_name);
    }
  }
  /* g_block_id is -1 when the caller passed no block, so a forwarded nil block
     falls through to a NULL argument (the callee's own nil-check handles it). */
  return forwards_param ? g_block_id : block;
}

void emit_method_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int mi = comp_method_index(c, name);
  Scope *m = mi >= 0 ? &c->scopes[mi] : NULL;
  buf_printf(b, "sp_%s(", mc(name));
  emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), "", b);
  /* pass &block as sp_Proc * when the callee has a blk_param and isn't inlined */
  if (m && m->blk_param && m->blk_param[0] && !m->yields) {
    /* A forwarded `&blk` can't be materialized directly by emit_proc_literal;
       resolve it to the caller's inlined block. Without this, forwarding `&blk`
       into a callee that keeps a real proc param (e.g. one that nil-checks the
       block) is rejected as "proc literal without a block". */
    int blk_node = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
    int wrote_args = m->nparams > 0;
    if (wrote_args) buf_puts(b, ", ");
    if (blk_node >= 0) {
      int blk_tmp = ++g_tmp;
      Buf pb; memset(&pb, 0, sizeof pb);
      emit_proc_literal(c, blk_node, &pb);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Proc *_t%d = %s;\n", blk_tmp, pb.p ? pb.p : "NULL");
      free(pb.p);
      buf_printf(b, "_t%d", blk_tmp);
    }
    else {
      buf_puts(b, "NULL");
    }
  }
  buf_puts(b, ")");
}

int patch_lv_reads(Compiler *c, int id, const char *nm, TyKind ty,
                           int *ids_out, TyKind *ty_out, int cap);

/* Bind a hash-iteration block's parameters to C locals for entry `ti` of the
   materialized hash temp `_t<trecv>` (type rt, runtime cname hn), emit the
   block's leading statements into g_pre at g_indent+1, evaluate its final
   expression, then restore every temporary shadow-type and rename change.
   Returns the final expression's text (caller frees) and its inferred type via
   *out_bret. `p0_solo_is_value` selects map-style single-parameter binding (the
   lone parameter receives the value) over select-style (it receives the key).
   The caller emits the loop header and consumes the returned text; this routine
   owns the intricate |k, v| binding shared by every hash block walk. */
static char *emit_hash_block_eval(Compiler *c, int block, TyKind rt, const char *hn,
                                  int trecv, int ti, int p0_solo_is_value, TyKind *out_bret) {
  const NodeTable *nt = c->nt;
  TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
  const char *p0_orig = block_param_name(c, block, 0);
  const char *p1_orig = block_param_name(c, block, 1);
  const char *p0 = p0_orig ? rename_local(p0_orig) : NULL;
  const char *p1 = p1_orig ? rename_local(p1_orig) : NULL;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;

  Scope *pscope = comp_scope_of(c, block);
  TyKind p0_actual = p1_orig ? kt : vt;   /* p0 is key if 2 params, else value */
  TyKind p1_actual = vt;
  LocalVar *p0_lv = p0_orig ? scope_local(pscope, p0_orig) : NULL;
  LocalVar *p1_lv = p1_orig ? scope_local(pscope, p1_orig) : NULL;
  TyKind p0_decl = p0_lv ? p0_lv->type : TY_UNKNOWN;
  TyKind p1_decl = p1_lv ? p1_lv->type : TY_UNKNOWN;
  int ns0 = p0_orig && p0_actual != TY_UNKNOWN && p0_decl != TY_UNKNOWN && p0_decl != p0_actual;
  int ns1 = p1_orig && p1_actual != TY_UNKNOWN && p1_decl != TY_UNKNOWN && p1_decl != p1_actual;
  int st0 = -1, sri0 = -1, srn0 = 0; char sro0[112]; sro0[0] = '\0';
  int st1 = -1, sri1 = -1, srn1 = 0; char sro1[112]; sro1[0] = '\0';
  /* p0 reads the key for a 2-param block, or for select-style solo binding. */
  int p0_is_key = p1_orig || !p0_solo_is_value;

  if (p0_orig) {
    emit_indent(g_pre, g_indent + 1);
    if (ns0) {
      st0 = ++g_tmp; emit_ctype(c, p0_actual, g_pre);
      if (p0_is_key) {
        if (rt == TY_POLY_POLY_HASH)
          buf_printf(g_pre, " lv__bp%d = _t%d->keys[_t%d->order[_t%d]];\n", st0, trecv, trecv, ti);
        else
          buf_printf(g_pre, " lv__bp%d = _t%d->order[_t%d];\n", st0, trecv, ti);
      }
      else {
        if (rt == TY_POLY_POLY_HASH)
          buf_printf(g_pre, " lv__bp%d = _t%d->vals[_t%d->order[_t%d]];\n", st0, trecv, trecv, ti);
        else
          buf_printf(g_pre, " lv__bp%d = sp_%sHash_get(_t%d, _t%d->order[_t%d]);\n", st0, hn, trecv, trecv, ti);
      }
      for (int ri = 0; ri < g_nren; ri++) {
        if (sp_streq(g_ren_from[ri], p0_orig)) {
          sri0 = ri; strncpy(sro0, g_ren_to[ri], sizeof sro0 - 1);
          snprintf(g_ren_to[ri], sizeof g_ren_to[0], "_bp%d", st0); break;
        }
      }
      if (sri0 < 0) { sri0 = g_nren; srn0 = 1;
        snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", p0_orig);
        snprintf(g_ren_to[g_nren++], sizeof g_ren_to[0], "_bp%d", st0);
      }
    }
    else {
      if (p0_is_key) {
        if (rt == TY_POLY_POLY_HASH)
          buf_printf(g_pre, "lv_%s = _t%d->keys[_t%d->order[_t%d]];\n", p0, trecv, trecv, ti);
        else
          buf_printf(g_pre, "lv_%s = _t%d->order[_t%d];\n", p0, trecv, ti);
      }
      else {
        if (rt == TY_POLY_POLY_HASH)
          buf_printf(g_pre, "lv_%s = _t%d->vals[_t%d->order[_t%d]];\n", p0, trecv, trecv, ti);
        else
          buf_printf(g_pre, "lv_%s = sp_%sHash_get(_t%d, _t%d->order[_t%d]);\n", p0, hn, trecv, trecv, ti);
      }
    }
  }
  if (p1_orig) {
    emit_indent(g_pre, g_indent + 1);
    if (ns1) {
      st1 = ++g_tmp; emit_ctype(c, p1_actual, g_pre);
      if (rt == TY_POLY_POLY_HASH)
        buf_printf(g_pre, " lv__bp%d = _t%d->vals[_t%d->order[_t%d]];\n", st1, trecv, trecv, ti);
      else
        buf_printf(g_pre, " lv__bp%d = sp_%sHash_get(_t%d, _t%d->order[_t%d]);\n", st1, hn, trecv, trecv, ti);
      for (int ri = 0; ri < g_nren; ri++) {
        if (sp_streq(g_ren_from[ri], p1_orig)) {
          sri1 = ri; strncpy(sro1, g_ren_to[ri], sizeof sro1 - 1);
          snprintf(g_ren_to[ri], sizeof g_ren_to[0], "_bp%d", st1); break;
        }
      }
      if (sri1 < 0) { sri1 = g_nren; srn1 = 1;
        snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", p1_orig);
        snprintf(g_ren_to[g_nren++], sizeof g_ren_to[0], "_bp%d", st1);
      }
    }
    else {
      if (rt == TY_POLY_POLY_HASH)
        buf_printf(g_pre, "lv_%s = _t%d->vals[_t%d->order[_t%d]];\n", p1, trecv, trecv, ti);
      else
        buf_printf(g_pre, "lv_%s = sp_%sHash_get(_t%d, _t%d->order[_t%d]);\n", p1, hn, trecv, trecv, ti);
    }
  }
  if (ns0 && p0_lv) p0_lv->type = p0_actual;
  if (ns1 && p1_lv) p1_lv->type = p1_actual;
  TyKind bret = (bb && bn > 0) ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
  /* A value-carrying next widens the block value past the tail, so the temp is
     boxed when a next yields a different type than the tail expression. */
  TyKind bnt = ie_block_break_next_ty(c, body);
  if (bnt != TY_UNKNOWN) bret = (bret == TY_UNKNOWN) ? bnt : ty_unify(bret, bnt);
  if (bret == TY_UNKNOWN) bret = (ns1 ? p1_actual : p0_actual);
  /* Collect the block's value next-aware into a temp: a tail or interior
     `next <v>` assigns the temp and falls through to the caller's collection
     rather than dropping the entry as a bare continue would. */
  int tvv = ++g_tmp; char tvvb[24]; snprintf(tvvb, sizeof tvvb, "_t%d", tvv);
  int want_poly = (bret == TY_POLY);
  emit_indent(g_pre, g_indent + 1);
  if (want_poly) buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil();\n", tvv);
  else { emit_ctype(c, bret, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tvv, default_value(bret)); }
  emit_block_value_into(c, block, tvvb, want_poly, g_indent + 1);
  if (ns0 && p0_lv) p0_lv->type = p0_decl;
  if (ns1 && p1_lv) p1_lv->type = p1_decl;
  if (sri1 >= 0) { if (srn1) g_nren = sri1; else strncpy(g_ren_to[sri1], sro1, sizeof g_ren_to[0]-1); }
  if (sri0 >= 0) { if (srn0) g_nren = sri0; else strncpy(g_ren_to[sri0], sro0, sizeof g_ren_to[0]-1); }
  if (out_bret) *out_bret = bret;
  Buf rv; memset(&rv, 0, sizeof rv); buf_puts(&rv, tvvb); return rv.p;
}

/* hash.map / collect { |k, v| ... } as an expression -> an array of the block
   values, built via a loop over the hash entries in the statement prelude. */
int emit_hash_collect_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  TyIterShape shp = ty_iter_shape(name);
  int is_sel = shp == TY_ITER_SELECT;
  int is_rej = shp == TY_ITER_REJECT;
  int is_map = shp == TY_ITER_MAP;
  if (!is_map && !is_sel && !is_rej) return 0;
  TyKind rt = comp_ntype(c, recv);
  const char *hn = ty_hash_cname(rt);
  if (!hn) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; if (body >= 0) nt_arr(nt, body, "body", &bn);
  if (bn < 1) return 0;

  int trecv = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;
  { Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = ", trecv); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p); }

  if (is_sel || is_rej) {
    /* select/reject: produce a same-type hash with matching pairs */
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = sp_%sHash_new();\n", tres, hn);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
    char *vb = emit_hash_block_eval(c, block, rt, hn, trecv, ti, 0, NULL);
    emit_indent(g_pre, g_indent + 1);
    TyKind vtt = ty_hash_val(rt);
    buf_printf(g_pre, "if (%s(%s)) { ", is_rej ? "!" : "", vb ? vb : "0"); free(vb);
    if (rt == TY_POLY_POLY_HASH) {
      buf_printf(g_pre, "sp_%sHash_set(_t%d, _t%d->keys[_t%d->order[_t%d]], _t%d->vals[_t%d->order[_t%d]]); }",
                 hn, tres, trecv, trecv, ti, trecv, trecv, ti);
    }
    else {
      buf_printf(g_pre, "sp_%sHash_set(_t%d, _t%d->order[_t%d], sp_%sHash_%s(_t%d, _t%d->order[_t%d])); }",
                 hn, tres, trecv, ti, hn, vtt == TY_INT ? "get_opt" : "get", trecv, trecv, ti);
    }
    buf_puts(g_pre, "\n");
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  }
  else {
    /* map: produce a result array */
    TyKind restype = comp_ntype(c, id);
    int res_poly = (restype == TY_POLY_ARRAY);
    const char *rk = res_poly ? "Poly" : array_kind(restype);
    if (!rk) return 0;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk, tres, rk);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
    TyKind bret;
    char *vb = emit_hash_block_eval(c, block, rt, hn, trecv, ti, 1, &bret);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_%sArray_push(_t%d, ", rk, tres);
    if (res_poly && bret != TY_POLY) {
      Buf bx; memset(&bx, 0, sizeof bx);
      emit_boxed_text(c, bret, vb ? vb : "", &bx);
      buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p);
    }
    else buf_puts(g_pre, vb ? vb : "");
    buf_puts(g_pre, ");\n"); free(vb);
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  }
  buf_printf(b, "_t%d", tres);
  return 1;
}

/* Emit, into g_pre at `indent`, `_t<dest> = sp_PolyArray_new();` then push entry
   `ti`'s boxed key and value. `dest` must already be a rooted sp_PolyArray*. */
static void emit_hash_pair_at(TyKind rt, const char *hn,
                              int trecv, int ti, int dest, int indent) {
  TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
  emit_indent(g_pre, indent);
  buf_printf(g_pre, "_t%d = sp_PolyArray_new();", dest);
  if (kt == TY_SYMBOL)
    buf_printf(g_pre, " sp_PolyArray_push(_t%d, sp_box_sym(_t%d->order[_t%d]));", dest, trecv, ti);
  else if (kt == TY_STRING)
    buf_printf(g_pre, " sp_PolyArray_push(_t%d, sp_box_str(_t%d->order[_t%d]));", dest, trecv, ti);
  else if (kt == TY_INT)
    buf_printf(g_pre, " sp_PolyArray_push(_t%d, sp_box_int(_t%d->order[_t%d]));", dest, trecv, ti);
  else
    buf_printf(g_pre, " sp_PolyArray_push(_t%d, _t%d->keys[_t%d->order[_t%d]]);", dest, trecv, trecv, ti);
  if (rt == TY_POLY_POLY_HASH)
    buf_printf(g_pre, " sp_PolyArray_push(_t%d, _t%d->vals[_t%d->order[_t%d]]);", dest, trecv, trecv, ti);
  else if (vt == TY_POLY)
    buf_printf(g_pre, " sp_PolyArray_push(_t%d, sp_%sHash_get(_t%d, _t%d->order[_t%d]));", dest, hn, trecv, trecv, ti);
  else if (vt == TY_INT)
    buf_printf(g_pre, " sp_PolyArray_push(_t%d, sp_box_int(sp_%sHash_get(_t%d, _t%d->order[_t%d])));", dest, hn, trecv, trecv, ti);
  else
    buf_printf(g_pre, " sp_PolyArray_push(_t%d, sp_box_str(sp_%sHash_get(_t%d, _t%d->order[_t%d])));", dest, hn, trecv, trecv, ti);
  buf_puts(g_pre, "\n");
}

/* hash.min_by / max_by / find / detect { |k, v| ... } -> the winning [k, v]
   pair, or nil when no entry qualifies. Emitted as prelude statements that
   produce a result temp, like the collect walk. */
int emit_hash_reduce_search_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int argc = 0; { int ar = nt_ref(nt, id, "arguments"); if (ar >= 0) nt_arr(nt, ar, "arguments", &argc); }
  int is_min = sp_streq(name, "min_by"), is_max = sp_streq(name, "max_by");
  int is_find = sp_streq(name, "find") || sp_streq(name, "detect");
  if ((!is_min && !is_max && !is_find) || argc != 0) return 0;
  int recv = nt_ref(nt, id, "receiver");
  TyKind rt = comp_ntype(c, recv);
  const char *hn = ty_hash_cname(rt);
  if (!hn) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; if (body >= 0) nt_arr(nt, body, "body", &bn);
  if (bn < 1) return 0;

  int trecv = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp, tbest = ++g_tmp, twin = ++g_tmp;
  { Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = ", trecv); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p); }
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", trecv);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = NULL; SP_GC_ROOT(_t%d);\n", tres, tres);
  /* Index of the winning entry, or -1 if none qualified. The pair is built
     once after the loop, so no result array is allocated until the walk is
     done (and never at all for a no-match find, which renders as nil). */
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = -1;\n", twin);
  if (is_min || is_max) {
    emit_indent(g_pre, g_indent);
    /* The best-so-far block value is held across allocating iterations, so it
       must be rooted; SP_GC_ROOT_RBVAL roots by address, so the later
       reassignment is tracked without a re-root. Seed it nil so the root scan
       sees a valid value before the first assignment. */
    buf_printf(g_pre, "sp_RbVal _bk%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_bk%d);\n", tbest, tbest);
  }
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
  TyKind bret;
  char *vb = emit_hash_block_eval(c, block, rt, hn, trecv, ti, 0, &bret);
  if (is_find) {
    emit_indent(g_pre, g_indent + 1);
    if (bret == TY_POLY)
      buf_printf(g_pre, "if (sp_poly_truthy(%s)) {\n", vb ? vb : "sp_box_nil()");
    else
      buf_printf(g_pre, "if (%s) {\n", vb ? vb : "0");
    emit_indent(g_pre, g_indent + 2); buf_printf(g_pre, "_t%d = _t%d; break;\n", twin, ti);
    emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
  }
  else {
    /* box the block's value so any comparable type sorts uniformly */
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_RbVal _kv%d = ", ti);
    if (bret == TY_POLY) buf_puts(g_pre, vb ? vb : "sp_box_nil()");
    else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, bret, vb ? vb : "", &bx);
           buf_puts(g_pre, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
    buf_puts(g_pre, ";\n");
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "if (_t%d == -1 || sp_poly_%s(_kv%d, _bk%d)) {\n",
               twin, is_min ? "lt" : "gt", ti, tbest);
    emit_indent(g_pre, g_indent + 2); buf_printf(g_pre, "_t%d = _t%d; _bk%d = _kv%d;\n", twin, ti, tbest, ti);
    emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
  }
  free(vb);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "if (_t%d >= 0) {\n", twin);
  emit_hash_pair_at(rt, hn, trecv, twin, tres, g_indent + 1);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tres);
  return 1;
}

/* hash.sort_by { |k, v| ... } -> the [k, v] pairs ordered by the block's value.
   Builds [sort_key, pair] tuples in the prelude, then sorts and projects them. */
int emit_hash_sort_by_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!sp_streq(name, "sort_by")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  TyKind rt = comp_ntype(c, recv);
  const char *hn = ty_hash_cname(rt);
  if (!hn) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; if (body >= 0) nt_arr(nt, body, "body", &bn);
  if (bn < 1) return 0;

  int trecv = ++g_tmp, ttmp = ++g_tmp, ti = ++g_tmp, tup = ++g_tmp, tpair = ++g_tmp;
  { Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = ", trecv); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p); }
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", trecv);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", ttmp, ttmp);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
  TyKind bret;
  char *vb = emit_hash_block_eval(c, block, rt, hn, trecv, ti, 0, &bret);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tup, tup);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", tup);
  if (bret == TY_POLY) buf_puts(g_pre, vb ? vb : "sp_box_nil()");
  else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, bret, vb ? vb : "", &bx);
         buf_puts(g_pre, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
  buf_puts(g_pre, ");\n");
  free(vb);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_PolyArray *_t%d = NULL; SP_GC_ROOT(_t%d);\n", tpair, tpair);
  emit_hash_pair_at(rt, hn, trecv, ti, tpair, g_indent + 1);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));\n", tup, tpair);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));\n", ttmp, tup);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "sp_PolyArray_sort_by_first(_t%d)", ttmp);
  return 1;
}

/* hash.sum(init) / count / all? / any? { |k, v| ... } -> a scalar reduction.
   sum accumulates the block value (boxed, via sp_poly_add); count tallies truthy
   results; all?/any? short-circuit to a boolean. */
int emit_hash_reduce_scalar_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int is_sum = sp_streq(name, "sum"), is_count = sp_streq(name, "count");
  int is_all = sp_streq(name, "all?"), is_any = sp_streq(name, "any?");
  if (!is_sum && !is_count && !is_all && !is_any) return 0;
  int argc = 0; { int ar = nt_ref(nt, id, "arguments"); if (ar >= 0) nt_arr(nt, ar, "arguments", &argc); }
  if (is_sum ? argc > 1 : argc != 0) return 0;
  int recv = nt_ref(nt, id, "receiver");
  TyKind rt = comp_ntype(c, recv);
  const char *hn = ty_hash_cname(rt);
  if (!hn) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; if (body >= 0) nt_arr(nt, body, "body", &bn);
  if (bn < 1) return 0;

  int sum_init = -1;
  if (is_sum && argc == 1) {
    int ar = nt_ref(nt, id, "arguments"); int an = 0;
    const int *aa = ar >= 0 ? nt_arr(nt, ar, "arguments", &an) : NULL;
    if (aa && an >= 1) sum_init = aa[0];
  }

  int trecv = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp;
  { Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = ", trecv); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p); }
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", trecv);
  emit_indent(g_pre, g_indent);
  if (is_sum) {
    buf_printf(g_pre, "sp_RbVal _t%d = ", tacc);
    if (sum_init >= 0) emit_boxed(c, sum_init, g_pre);
    else buf_puts(g_pre, "sp_box_int(0)");
    buf_puts(g_pre, ";\n");
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", tacc);
  }
  else if (is_count)
    buf_printf(g_pre, "mrb_int _t%d = 0;\n", tacc);
  else
    buf_printf(g_pre, "mrb_bool _t%d = %s;\n", tacc, is_all ? "TRUE" : "FALSE");

  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
  TyKind bret;
  char *vb = emit_hash_block_eval(c, block, rt, hn, trecv, ti, 0, &bret);
  if (is_sum) {
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "_t%d = sp_poly_add(_t%d, ", tacc, tacc);
    if (bret == TY_POLY) buf_puts(g_pre, vb ? vb : "sp_box_nil()");
    else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, bret, vb ? vb : "", &bx);
           buf_puts(g_pre, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
    buf_puts(g_pre, ");\n");
  }
  else {
    /* truthiness of the block result, formatted into a dynamic Buf so a long
       boxed expression can never be silently truncated */
    Buf cond; memset(&cond, 0, sizeof cond);
    if (bret == TY_BOOL) buf_printf(&cond, "(%s)", vb ? vb : "0");
    else if (bret == TY_POLY) buf_printf(&cond, "sp_poly_truthy(%s)", vb ? vb : "sp_box_nil()");
    else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, bret, vb ? vb : "", &bx);
           buf_printf(&cond, "sp_poly_truthy(%s)", bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
    const char *cs = cond.p ? cond.p : "0";
    emit_indent(g_pre, g_indent + 1);
    if (is_count) buf_printf(g_pre, "if (%s) _t%d++;\n", cs, tacc);
    else if (is_all) buf_printf(g_pre, "if (!(%s)) { _t%d = FALSE; break; }\n", cs, tacc);
    else buf_printf(g_pre, "if (%s) { _t%d = TRUE; break; }\n", cs, tacc);
    free(cond.p);
  }
  free(vb);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tacc);
  return 1;
}

/* hash.flat_map / filter_map / partition { |k, v| ... } -> a poly array.
   flat_map concatenates the per-entry block arrays; filter_map keeps truthy
   block values; partition returns [matching_pairs, remaining_pairs]. */
int emit_hash_transform_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int is_flat = sp_streq(name, "flat_map") || sp_streq(name, "collect_concat");
  int is_fmap = sp_streq(name, "filter_map");
  int is_part = sp_streq(name, "partition");
  if (!is_flat && !is_fmap && !is_part) return 0;
  int argc = 0; { int ar = nt_ref(nt, id, "arguments"); if (ar >= 0) nt_arr(nt, ar, "arguments", &argc); }
  if (argc != 0) return 0;
  int recv = nt_ref(nt, id, "receiver");
  TyKind rt = comp_ntype(c, recv);
  const char *hn = ty_hash_cname(rt);
  if (!hn) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; if (body >= 0) nt_arr(nt, body, "body", &bn);
  if (bn < 1) return 0;

  int trecv = ++g_tmp, ti = ++g_tmp;
  { Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = ", trecv); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p); }
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", trecv);

  /* Per-entry temporaries are declared and GC-rooted once, before the loop, and
     reassigned each iteration (the root tracks the stack slot, not the value).
     _tp is the [k, v] pair (partition); _tbv is the boxed block value
     (filter_map / flat_map). */
  int tm = is_part ? ++g_tmp : 0, tr = is_part ? ++g_tmp : 0, tres = ++g_tmp;
  int tp = is_part ? ++g_tmp : 0;
  int tbv = is_part ? 0 : ++g_tmp;
  if (is_part) {
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tm, tm);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tr, tr);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = NULL; SP_GC_ROOT(_t%d);\n", tp, tp);
  }
  else {
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres, tres);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);\n", tbv, tbv);
  }
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
  TyKind bret;
  char *vb = emit_hash_block_eval(c, block, rt, hn, trecv, ti, 0, &bret);
  if (is_part) {
    char cond[256];
    if (bret == TY_BOOL) snprintf(cond, sizeof cond, "(%s)", vb ? vb : "0");
    else if (bret == TY_POLY) snprintf(cond, sizeof cond, "sp_poly_truthy(%s)", vb ? vb : "sp_box_nil()");
    else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, bret, vb ? vb : "", &bx);
           snprintf(cond, sizeof cond, "sp_poly_truthy(%s)", bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
    emit_hash_pair_at(rt, hn, trecv, ti, tp, g_indent + 1);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "if (%s) sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d)); else sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));\n",
               cond, tm, tp, tr, tp);
  }
  else {  /* filter_map / flat_map: snapshot the block value into _tbv first */
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "_t%d = ", tbv);
    if (bret == TY_POLY) buf_puts(g_pre, vb ? vb : "sp_box_nil()");
    else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, bret, vb ? vb : "", &bx);
           buf_puts(g_pre, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
    buf_puts(g_pre, ";\n");
    if (is_fmap) {
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "if (sp_poly_truthy(_t%d)) sp_PolyArray_push(_t%d, _t%d);\n", tbv, tres, tbv);
    }
    else if (ty_is_array(bret) || bret == TY_POLY_ARRAY) {  /* flat_map, array value */
      int tj = ++g_tmp;
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_poly_arr_len(_t%d); _t%d++) sp_PolyArray_push(_t%d, sp_poly_arr_get(_t%d, _t%d));\n",
                 tj, tj, tbv, tj, tres, tbv, tj);
    }
    else {  /* flat_map, scalar value */
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "sp_PolyArray_push(_t%d, _t%d);\n", tres, tbv);
    }
  }
  free(vb);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  if (is_part) {
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres, tres);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d)); sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));\n",
               tres, tm, tres, tr);
  }
  buf_printf(b, "_t%d", tres);
  return 1;
}

/* hash.group_by { |k, v| ... } -> a hash from each block value to the array of
   [k, v] pairs that produced it (poly keys, poly-array values). */
int emit_hash_group_by_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!sp_streq(name, "group_by")) return 0;
  int argc = 0; { int ar = nt_ref(nt, id, "arguments"); if (ar >= 0) nt_arr(nt, ar, "arguments", &argc); }
  if (argc != 0) return 0;
  int recv = nt_ref(nt, id, "receiver");
  TyKind rt = comp_ntype(c, recv);
  const char *hn = ty_hash_cname(rt);
  if (!hn) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; if (body >= 0) nt_arr(nt, body, "body", &bn);
  if (bn < 1) return 0;

  int trecv = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp, tk = ++g_tmp, tp = ++g_tmp, tg = ++g_tmp, tex = ++g_tmp;
  { Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = ", trecv); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p); }
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", trecv);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyPolyHash *_t%d = sp_PolyPolyHash_new(); SP_GC_ROOT(_t%d);\n", tres, tres);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
  TyKind bret;
  char *vb = emit_hash_block_eval(c, block, rt, hn, trecv, ti, 0, &bret);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_RbVal _t%d = ", tk);
  if (bret == TY_POLY) buf_puts(g_pre, vb ? vb : "sp_box_nil()");
  else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, bret, vb ? vb : "", &bx);
         buf_puts(g_pre, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
  buf_puts(g_pre, ";\n");
  free(vb);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", tk);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_PolyArray *_t%d = NULL; SP_GC_ROOT(_t%d);\n", tp, tp);
  emit_hash_pair_at(rt, hn, trecv, ti, tp, g_indent + 1);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_PolyArray *_t%d = NULL; SP_GC_ROOT(_t%d);\n", tg, tg);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_RbVal _t%d = sp_PolyPolyHash_get(_t%d, _t%d);\n", tex, tres, tk);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "if (_t%d.tag == SP_TAG_NIL) { _t%d = sp_PolyArray_new(); sp_PolyPolyHash_set(_t%d, _t%d, sp_box_poly_array(_t%d)); }\n",
             tex, tg, tres, tk, tg);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "else { _t%d = (sp_PolyArray *)_t%d.v.p; }\n", tg, tex);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));\n", tg, tp);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tres);
  return 1;
}

/* Recursively patch c->ntype[id]=ty for every LocalVariableReadNode named nm
   within the subtree at id. Saved old values in ids_out/ty_out (max cap).
   Returns count of patched nodes. */
int patch_lv_reads(Compiler *c, int id, const char *nm, TyKind ty,
                           int *ids_out, TyKind *ty_out, int cap) {
  if (id < 0 || id >= c->nt->count || cap <= 0) return 0;
  int n = 0;
  const char *node_ty = nt_type(c->nt, id);
  if (node_ty && sp_streq(node_ty, "LocalVariableReadNode")) {
    const char *vname = nt_str(c->nt, id, "name");
    if (vname && sp_streq(vname, nm)) {
      ids_out[0] = id; ty_out[0] = c->ntype[id]; c->ntype[id] = ty;
      return 1;
    }
  }
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr && n < cap; i++) {
    int r = nt_ref_at(c->nt, id, i);
    n += patch_lv_reads(c, r, nm, ty, ids_out + n, ty_out + n, cap - n);
  }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na && n < cap; i++) {
    int cn = 0; const int *ids = nt_arr_at(c->nt, id, i, &cn);
    for (int j = 0; j < cn && n < cap; j++)
      n += patch_lv_reads(c, ids[j], nm, ty, ids_out + n, ty_out + n, cap - n);
  }
  return n;
}

/* Scan nodes with IDs in [min_id, max_id) for LocalVariableReadNode with the
   given name in the given scope, and patch c->ntype[id] to `new_ty`.
   Prism assigns IDs in pre-order, so all descendants of `block` have id > block.
   Passing min_id=block+1 restricts patching to the block's own subtree.
   Stores original types in saved[] (caller must free). Returns the count. */
int patch_lv_read_ntype(Compiler *c, int scope_idx, const char *name,
                                TyKind new_ty, int min_id,
                                int **saved_ids, TyKind **saved_tys) {
  int n = 0, cap = 8;
  *saved_ids = malloc(sizeof(int) * (size_t)cap);
  *saved_tys = malloc(sizeof(TyKind) * (size_t)cap);
  for (int i = min_id; i < c->nt->count; i++) {
    const char *ty = nt_type(c->nt, i);
    if (!ty || !sp_streq(ty, "LocalVariableReadNode")) continue;
    if (c->nscope[i] != scope_idx) continue;
    const char *nm = nt_str(c->nt, i, "name");
    if (!nm || !sp_streq(nm, name)) continue;
    if (c->ntype[i] == new_ty) continue;
    if (n >= cap) { cap *= 2; *saved_ids = realloc(*saved_ids, sizeof(int) * (size_t)cap); *saved_tys = realloc(*saved_tys, sizeof(TyKind) * (size_t)cap); }
    (*saved_ids)[n] = i;
    (*saved_tys)[n] = c->ntype[i];
    c->ntype[i] = new_ty;
    n++;
  }
  return n;
}

void restore_lv_read_ntype(Compiler *c, int *saved_ids, TyKind *saved_tys, int n) {
  for (int i = 0; i < n; i++) c->ntype[saved_ids[i]] = saved_tys[i];
  free(saved_ids); free(saved_tys);
}

/* hash.transform_keys { |k| nk } / transform_values { |v| nv }: rebuild the
   hash applying the block to every key (or value), keeping the other half.
   Returns 1 if handled. */
int emit_transform_hash_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || (!sp_streq(name, "transform_keys") && !sp_streq(name, "transform_values"))) return 0;
  int keys = sp_streq(name, "transform_keys");
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  const char *shn = ty_hash_cname(rt);
  if (!shn) return 0;
  TyKind dt = comp_ntype(c, id);
  const char *dhn = ty_hash_cname(dt);
  if (!dhn) return 0;
  const char *p0_orig = block_param_name(c, block, 0);
  const char *p0 = p0_orig ? rename_local(p0_orig) : NULL;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  /* Empty block: transform_values { } → all values become nil (keep key set) */
  if (bn < 1) {
    if (keys) return 0;
    int ts2 = ++g_tmp, td2 = ++g_tmp, ti2 = ++g_tmp, tk2 = ++g_tmp;
    Buf rb2; memset(&rb2, 0, sizeof rb2); emit_expr(c, recv, &rb2);
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = ", ts2); buf_puts(g_pre, rb2.p ? rb2.p : ""); buf_puts(g_pre, ";\n"); free(rb2.p);
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);\n", td2, shn, td2);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti2, ti2, ts2, ti2);
    emit_indent(g_pre, g_indent + 1); emit_ctype(c, ty_hash_key(rt), g_pre);
    if (rt == TY_POLY_POLY_HASH)
      buf_printf(g_pre, " _t%d = _t%d->keys[_t%d->order[_t%d]];\n", tk2, ts2, ts2, ti2);
    else
      buf_printf(g_pre, " _t%d = _t%d->order[_t%d];\n", tk2, ts2, ti2);
    emit_indent(g_pre, g_indent + 1);
    { TyKind vt2 = ty_hash_val(rt);
      const char *nil_v = (vt2 == TY_INT) ? "SP_INT_NIL" :
                          (vt2 == TY_POLY) ? "sp_box_nil()" : "NULL";
      buf_printf(g_pre, "sp_%sHash_set(_t%d, _t%d, %s);\n", shn, td2, tk2, nil_v); }
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
    buf_printf(b, "_t%d", td2);
    return 1;
  }
  TyKind skt = ty_hash_key(rt), svt = ty_hash_val(rt);
  TyKind dvt = ty_hash_val(dt);
  /* When the scope declares v as TY_POLY but the hash has typed (non-poly) values,
     box the value on assignment so the poly variable receives sp_RbVal. */
  Scope *pscope_tv = comp_scope_of(c, block);
  LocalVar *p0_lv_tv = p0_orig ? scope_local(pscope_tv, p0_orig) : NULL;
  TyKind p0_scope_ty = p0_lv_tv ? p0_lv_tv->type : TY_UNKNOWN;
  int needs_box_assign = (p0_scope_ty == TY_POLY && (!keys ? svt != TY_POLY : skt != TY_POLY));
  int ts = ++g_tmp, td = ++g_tmp, ti = ++g_tmp, tk = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);  /* recv preludes flush to g_pre first */
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", ts); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
  emit_indent(g_pre, g_indent); emit_ctype(c, dt, g_pre); buf_printf(g_pre, " _t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);\n", td, dhn, td);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, ts, ti);
  emit_indent(g_pre, g_indent + 1); emit_ctype(c, skt, g_pre);
  if (rt == TY_POLY_POLY_HASH)
    buf_printf(g_pre, " _t%d = _t%d->keys[_t%d->order[_t%d]];\n", tk, ts, ts, ti);
  else
    buf_printf(g_pre, " _t%d = _t%d->order[_t%d];\n", tk, ts, ti);
  if (p0) {
    emit_indent(g_pre, g_indent + 1);
    if (keys) {
      if (needs_box_assign) {
        Buf bx; memset(&bx, 0, sizeof bx); char gk[64]; snprintf(gk, sizeof gk, "_t%d", tk);
        emit_boxed_text(c, skt, gk, &bx);
        buf_printf(g_pre, "lv_%s = %s;\n", p0, bx.p ? bx.p : ""); free(bx.p);
      }
      else buf_printf(g_pre, "lv_%s = _t%d;\n", p0, tk);
    }
    else {
      if (needs_box_assign) {
        char gv[128]; snprintf(gv, sizeof gv, "sp_%sHash_get(_t%d, _t%d)", shn, ts, tk);
        Buf bx; memset(&bx, 0, sizeof bx);
        emit_boxed_text(c, svt, gv, &bx);
        buf_printf(g_pre, "lv_%s = %s;\n", p0, bx.p ? bx.p : ""); free(bx.p);
      }
      else buf_printf(g_pre, "lv_%s = sp_%sHash_get(_t%d, _t%d);\n", p0, shn, ts, tk);
    }
  }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
  int save = g_indent; g_indent++;
  Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = save;
  TyKind bret = comp_ntype(c, bb[bn - 1]);
  TyKind dkt = ty_hash_key(dt);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_%sHash_set(_t%d, ", dhn, td);
  if (keys) {
    /* new key = block result; unbox if block returned poly but key type is typed */
    const char *vbp = vb.p ? vb.p : "0";
    if (dkt == TY_STRING && (bret == TY_POLY || bret == TY_UNKNOWN))
      buf_printf(g_pre, "sp_poly_to_s(%s)", vbp);
    else if (dkt == TY_INT && (bret == TY_POLY || bret == TY_UNKNOWN))
      buf_printf(g_pre, "sp_poly_to_i(%s)", vbp);
    else
      buf_puts(g_pre, vbp);
    buf_puts(g_pre, ", ");
    if (rt == TY_POLY_POLY_HASH)
      buf_printf(g_pre, "_t%d->vals[_t%d->order[_t%d]]", ts, ts, ti);
    else if (dvt == TY_POLY && svt != TY_POLY) { Buf bx; memset(&bx, 0, sizeof bx); char g[64]; snprintf(g, sizeof g, "sp_%sHash_get(_t%d, _t%d)", shn, ts, tk); emit_boxed_text(c, svt, g, &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p); }
    else buf_printf(g_pre, "sp_%sHash_get(_t%d, _t%d)", shn, ts, tk);
  }
  else {
    /* key carried over; new value = block result (box/unbox to match dest type) */
    buf_printf(g_pre, "_t%d, ", tk);
    if (dvt == TY_POLY && bret != TY_POLY) {
      Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, bret, vb.p ? vb.p : "", &bx);
      buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p);
    }
    else if (dvt == TY_STRING && (bret == TY_POLY || bret == TY_UNKNOWN)) {
      buf_printf(g_pre, "sp_poly_to_s(%s)", vb.p ? vb.p : "sp_box_nil()");
    }
    else if (dvt == TY_INT && (bret == TY_POLY || bret == TY_UNKNOWN)) {
      buf_printf(g_pre, "sp_poly_to_i(%s)", vb.p ? vb.p : "sp_box_nil()");
    }
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
int emit_bsearch_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "bsearch")) return 0;
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
/* Emit `src` (a poly sp_RbVal C-expression) coerced to scalar type `dst`. */
static void flatmap_coerce_from_poly(TyKind dst, const char *src, Buf *out) {
  if (dst == TY_INT || dst == TY_BOOL) buf_printf(out, "sp_poly_to_i(%s)", src);
  else if (dst == TY_FLOAT) buf_printf(out, "sp_poly_to_f(%s)", src);
  else buf_puts(out, src);  /* poly (or other): pass through */
}

/* CRuby proc auto-splat: bind each of the block's params to a positional element
   of the poly sub-array held in temp `_t<elem_temp>` (an sp_RbVal), coerced to
   each param's pinned type. Used where a multi-param block iterates a poly array
   whose elements are themselves arrays (map/select/reject/sort_by). */
static void emit_autosplat_params(Compiler *c, int block, int np, int elem_temp, int indent) {
  Scope *asc = comp_scope_of(c, block);
  for (int pj = 0; pj < np; pj++) {
    const char *pn = block_param_name(c, block, pj); if (!pn) continue;
    const char *pnr = rename_local(pn);
    LocalVar *lvp = asc ? scope_local(asc, pn) : NULL;
    TyKind pty = lvp ? lvp->type : TY_POLY;
    char src[96]; snprintf(src, sizeof src, "sp_poly_index_poly(_t%d, sp_box_int(%d))", elem_temp, pj);
    emit_indent(g_pre, indent); buf_printf(g_pre, "lv_%s = ", pnr);
    Buf cv; memset(&cv, 0, sizeof cv); flatmap_coerce_from_poly(pty, src, &cv);
    buf_puts(g_pre, cv.p ? cv.p : src); free(cv.p); buf_puts(g_pre, ";\n");
  }
}

/* `array.flat_map { |params| block-returning-array }` as an expression: map each
   element through the block and concatenate the returned arrays (flatten one
   level). Handles a single block param (bound to the element) and a flat
   multi-param destructure of a poly-array element. Returns 1 if handled. */
int emit_flat_map_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || (!sp_streq(name, "flat_map") && !sp_streq(name, "collect_concat"))) return 0;
  int block = nt_ref(nt, id, "block");
  int recv = nt_ref(nt, id, "receiver");
  if (block < 0 || recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  /* A poly receiver whose array-ness is only known at runtime (e.g. a recursive
     param, or a `case ... end` whose arms mix arrays and scalars): coerce it to
     a poly array at runtime and run the ordinary poly path over it. */
  int poly_recv = (rt == TY_POLY);
  if (poly_recv) rt = TY_POLY_ARRAY;
  if (!ty_is_array(rt)) return 0;
  const char *rk = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!rk) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  /* Identity block `{ |x| x }` over an array of arrays is a one-level flatten,
     equivalent to `flatten(1)`: each sub-array element is unboxed inline and
     scalars pass through. The block returns the bare element (a poly element,
     not statically an array), so the array-returning path below cannot handle
     it; emit the runtime flatten directly. */
  if (bn == 1 && rt == TY_POLY_ARRAY && !poly_recv) {
    const char *p0 = block_param_name(c, block, 0);
    const char *sty = nt_type(nt, bb[0]);
    if (p0 && !block_param_name(c, block, 1) && sty &&
        sp_streq(sty, "LocalVariableReadNode") && nt_str(nt, bb[0], "name") &&
        sp_streq(nt_str(nt, bb[0], "name"), p0)) {
      buf_puts(b, "sp_PolyArray_flatten_n(");
      emit_expr(c, recv, b);
      buf_puts(b, ", 1)");
      return 1;
    }
  }
  TyKind bret = comp_ntype(c, bb[bn - 1]);
  /* A block whose value is not statically an array (a bare poly, or a mix of
     array and scalar as in `... ? sub_array : scalar`) is handled per CRuby:
     an array value is spliced one level, a scalar is appended as-is. The result
     is then a poly array. Gate on the inferred result being TY_POLY_ARRAY so a
     statically-scalar return (e.g. `Int`, inferred as a typed IntArray) is not
     forced into a sp_PolyArray and mistyped -- it falls through as before. */
  int poly_ret = !ty_is_array(bret) && comp_ntype(c, id) == TY_POLY_ARRAY;
  const char *bk = poly_ret ? "Poly" : ((bret == TY_POLY_ARRAY) ? "Poly" : array_kind(bret));
  if (!bk) return 0;
  int np = 0; while (block_param_name(c, block, np)) np++;
  if (np > 1 && rt != TY_POLY_ARRAY) return 0;  /* destructure needs poly elements */
  TyKind et = ty_array_elem(rt);
  int ta = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
  if (poly_recv) buf_printf(g_pre, " _t%d = sp_poly_to_poly_array(%s); SP_GC_ROOT(_t%d);\n", ta, rb.p ? rb.p : "sp_box_nil()", ta);
  else buf_printf(g_pre, " _t%d = %s;\n", ta, rb.p ? rb.p : "");
  free(rb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);\n", bk, tres, bk, tres);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, rk, ta, ti);
  if (np <= 1) {
    const char *p0 = block_param_name(c, block, 0);
    if (p0) {
      const char *p0r = rename_local(p0);
      LocalVar *lv = scope_local(comp_scope_of(c, block), p0);
      TyKind pt = lv ? lv->type : et;
      emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = ", p0r);
      char src[64]; snprintf(src, sizeof src, "sp_%sArray_get(_t%d, _t%d)", rk, ta, ti);
      if (pt == et) buf_puts(g_pre, src);
      else if (et == TY_POLY) { Buf cv; memset(&cv,0,sizeof cv); flatmap_coerce_from_poly(pt, src, &cv); buf_puts(g_pre, cv.p?cv.p:src); free(cv.p); }
      else buf_puts(g_pre, src);
      buf_puts(g_pre, ";\n");
    }
  }
  else {
    int te = ++g_tmp;
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);\n", te, ta, ti);
    for (int pj = 0; pj < np; pj++) {
      const char *pn = block_param_name(c, block, pj); if (!pn) break;
      const char *pnr = rename_local(pn);
      LocalVar *lv = scope_local(comp_scope_of(c, block), pn);
      TyKind pt = lv ? lv->type : TY_POLY;
      char src[96]; snprintf(src, sizeof src, "sp_poly_index_poly(_t%d, sp_box_int(%d))", te, pj);
      emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = ", pnr);
      Buf cv; memset(&cv, 0, sizeof cv); flatmap_coerce_from_poly(pt, src, &cv);
      buf_puts(g_pre, cv.p ? cv.p : src); free(cv.p); buf_puts(g_pre, ";\n");
    }
  }
  int tbv = ++g_tmp;
  char tbvb[24]; snprintf(tbvb, sizeof tbvb, "_t%d", tbv);
  if (poly_ret) {
    /* collect the block's (boxed) value, then splice one level: an array value
       has its elements appended, a scalar is pushed as-is (CRuby flat_map). */
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", tbv, default_value(TY_POLY));
    emit_block_value_into(c, block, tbvb, 1, g_indent + 1);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_PolyArray_flatten_into_n(_t%d, _t%d, 1);\n", tres, tbv);
  } else {
    int tj = ++g_tmp;
    /* collect the block's value (next-aware) into the per-iteration array temp,
       then splat its elements -- so `next [..]` flattens its array like the tail. */
    emit_indent(g_pre, g_indent + 1); emit_ctype(c, bret, g_pre);
    buf_printf(g_pre, " _t%d = %s;\n", tbv, default_value(bret));
    emit_block_value_into(c, block, tbvb, bret == TY_POLY, g_indent + 1);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d));\n",
               tj, tj, bk, tbv, tj, bk, tres, bk, tbv, tj);
  }
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tres);
  return 1;
}

/* recv.filter_map { |x| body } -- map, then drop falsy results. The result is a
   poly array (the body is often a nilable `expr if cond`); each truthy boxed
   value is kept. Output matches CRuby for both nilable and concrete bodies. */
int emit_filter_map_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "filter_map")) return 0;
  int block = nt_ref(nt, id, "block");
  int recv = nt_ref(nt, id, "receiver");
  if (block < 0 || !nt_type(nt, block) || !sp_streq(nt_type(nt, block), "BlockNode") || recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;  /* range filter_map: a later slice */
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0;
  const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  const char *p0 = block_param_name(c, block, 0);
  if (p0) p0 = rename_local(p0);

  int ta = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb);
  emit_expr(c, recv, &rb);
  TyKind et = ty_array_elem(rt);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", ta);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres, tres);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, k, ta, ti);

  Scope *csc = p0 ? comp_scope_of(c, block) : NULL;
  LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
  TyKind csaved0 = clv0 ? clv0->type : TY_UNKNOWN;
  int use_shadow = clv0 && clv0->type != et && et != TY_UNKNOWN;
  int din = g_indent + 1;
  if (use_shadow) {
    clv0->type = et;
    for (int j = 0; j < bn; j++) infer_type(c, bb[j]);
    emit_indent(g_pre, din); buf_puts(g_pre, "{\n"); din++;
    emit_indent(g_pre, din); emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, ta, ti);
  }
  else if (p0) { emit_indent(g_pre, din); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, ta, ti); }

  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, din);
  int save = g_indent; g_indent = din;
  Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = save;
  TyKind vt = comp_ntype(c, bb[bn - 1]);
  int tv = ++g_tmp;
  emit_indent(g_pre, din); buf_printf(g_pre, "sp_RbVal _t%d = ", tv);
  if (vt == TY_POLY) buf_puts(g_pre, vb.p ? vb.p : "sp_box_nil()");
  else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, vt, vb.p ? vb.p : "", &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p); }
  buf_puts(g_pre, ";\n"); free(vb.p);
  emit_indent(g_pre, din); buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", tv);
  emit_indent(g_pre, din); buf_printf(g_pre, "if (sp_poly_truthy(_t%d)) sp_PolyArray_push(_t%d, _t%d);\n", tv, tres, tv);
  if (use_shadow) { din--; emit_indent(g_pre, din); buf_puts(g_pre, "}\n"); }
  if (clv0) clv0->type = csaved0;
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tres);
  return 1;
}

int emit_minmax_by_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int is_max = sp_streq(name, "max_by"), is_min = sp_streq(name, "min_by");
  int is_minmax = sp_streq(name, "minmax_by");
  if (!is_max && !is_min && !is_minmax) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  int is_range = (rt == TY_RANGE);  /* a finite int range materializes to an int array */
  if (!is_range && !ty_is_array(rt)) return 0;
  const char *k = is_range ? "Int" : (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind et = is_range ? TY_INT : ty_array_elem(rt);
  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  TyKind bvt = infer_type(c, bb[bn - 1]);
  if (bvt != TY_INT && bvt != TY_FLOAT && bvt != TY_POLY) return 0;
  /* 2+-param block over a poly array of sub-arrays: auto-splat each element
     across the params. The winning element is stored from an element temp
     rather than the (now per-position) block param. */
  int np_mb = 0; while (block_param_name(c, block, np_mb)) np_mb++;
  int autosplat = (np_mb >= 2 && rt == TY_POLY_ARRAY && !block_param_is_multi(c, block, 0));

  /* Count form: min_by(n) / max_by(n) { |x| key } -> the n smallest/largest
     elements by key, as a generic (poly) Array. Sort indices by the boxed key
     then take the first n (min, ascending) or last n reversed (max,
     descending). */
  int mb_args = nt_ref(nt, id, "arguments");
  int mb_argc = 0; const int *mb_argv = mb_args >= 0 ? nt_arr(nt, mb_args, "arguments", &mb_argc) : NULL;
  if ((is_min || is_max) && mb_argc == 1 && p0 && !autosplat) {
    int trv = ++g_tmp, tn = ++g_tmp, tkeys = ++g_tmp, tidx = ++g_tmp, ti = ++g_tmp,
        tres = ++g_tmp, tcnt = ++g_tmp, ttake = ++g_tmp, tg = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = %s;\n", trv, rb.p ? rb.p : ""); free(rb.p);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", trv);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = sp_%sArray_length(_t%d);\n", tn, k, trv);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tkeys, tkeys);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);\n", tidx, tidx);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) {\n", ti, ti, tn, ti);
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trv, ti);
    for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
    int save = g_indent; g_indent++;
    Buf kb; memset(&kb, 0, sizeof kb); emit_expr(c, bb[bn - 1], &kb); g_indent = save;
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", tkeys);
    if (bvt == TY_POLY) buf_puts(g_pre, kb.p ? kb.p : "sp_box_nil()");
    else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, bvt, kb.p ? kb.p : "0", &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p); }
    buf_puts(g_pre, ");\n"); free(kb.p);
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_IntArray_push(_t%d, _t%d);\n", tidx, ti);
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_sort_idx_by_poly(_t%d->data + _t%d->start, _t%d->data, _t%d);\n", tidx, tidx, tkeys, tn);
    /* a negative count is an ArgumentError in CRuby, then clamp to the element count */
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = ", tcnt); emit_int_expr(c, mb_argv[0], g_pre); buf_puts(g_pre, ";\n");
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative size\");\n", tcnt);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = _t%d < _t%d ? _t%d : _t%d;\n", ttake, tcnt, tn, tcnt, tn);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres, tres);
    if (is_min) {
      emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)\n", tg, tg, ttake, tg);
    } else {
      emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = _t%d - 1; _t%d >= _t%d - _t%d; _t%d--)\n", tg, tn, tg, tn, ttake, tg);
    }
    emit_indent(g_pre, g_indent + 1);
    {
      Buf eb; memset(&eb, 0, sizeof eb);
      char getexpr[96];
      snprintf(getexpr, sizeof getexpr, "sp_%sArray_get(_t%d, sp_IntArray_get(_t%d, _t%d))", k, trv, tidx, tg);
      if (rt == TY_POLY_ARRAY) buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s);\n", tres, getexpr);
      else { emit_boxed_text(c, et, getexpr, &eb); buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s);\n", tres, eb.p ? eb.p : "sp_box_nil()"); }
      free(eb.p);
    }
    buf_printf(b, "_t%d", tres);
    return 1;
  }
  /* No other argument shape is supported for min_by/max_by/minmax_by; reject
     loudly rather than silently returning a single winner. */
  if (mb_argc != 0) return 0;

  if (is_minmax) {
    /* track the min-keyed and max-keyed elements in one pass; yield a fresh
       same-kind [min, max] array. Strict comparisons keep the first occurrence
       of a tied key, matching Ruby. */
    int trecv = ++g_tmp, tmin = ++g_tmp, tmax = ++g_tmp, tbvmin = ++g_tmp, tbvmax = ++g_tmp,
        tf = ++g_tmp, ti = ++g_tmp, tcur = ++g_tmp, tout = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", trecv); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
    const char *edflt = et == TY_RANGE ? "(sp_Range){0}" : default_value(et);
    emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tmin, edflt);
    emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tmax, edflt);
    emit_indent(g_pre, g_indent); emit_ctype(c, bvt, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tbvmin, default_value(bvt));
    emit_indent(g_pre, g_indent); emit_ctype(c, bvt, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tbvmax, default_value(bvt));
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "int _t%d = 1;\n", tf);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, k, trecv, ti);
    char mmelem[24];
    if (autosplat) {
      int telem = ++g_tmp;
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);\n", telem, trecv, ti);
      emit_autosplat_params(c, block, np_mb, telem, g_indent + 1);
      snprintf(mmelem, sizeof mmelem, "_t%d", telem);
    } else {
      if (p0) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti); }
      snprintf(mmelem, sizeof mmelem, "lv_%s", p0 ? p0 : "");
    }
    for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
    Scope *mmsc = (p0 && !autosplat) ? comp_scope_of(c, block) : NULL;
    LocalVar *mmlv = (mmsc && p0) ? scope_local(mmsc, p0) : NULL;
    TyKind mmpt = mmlv ? mmlv->type : TY_UNKNOWN;
    if (mmlv) mmlv->type = et;
    int save = g_indent; g_indent++;
    Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = save;
    if (mmlv) mmlv->type = mmpt;
    emit_indent(g_pre, g_indent + 1); emit_ctype(c, bvt, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tcur, vb.p ? vb.p : default_value(bvt)); free(vb.p);
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "if (_t%d) { _t%d = %s; _t%d = %s; _t%d = _t%d; _t%d = _t%d; _t%d = 0; }\n",
               tf, tmin, mmelem, tmax, mmelem, tbvmin, tcur, tbvmax, tcur, tf);
    emit_indent(g_pre, g_indent + 1);
    if (bvt == TY_POLY) {
      buf_printf(g_pre, "else { if (sp_poly_lt(_t%d, _t%d)) { _t%d = %s; _t%d = _t%d; } if (sp_poly_gt(_t%d, _t%d)) { _t%d = %s; _t%d = _t%d; } }\n",
                 tcur, tbvmin, tmin, mmelem, tbvmin, tcur, tcur, tbvmax, tmax, mmelem, tbvmax, tcur);
    }
    else {
      buf_printf(g_pre, "else { if (_t%d < _t%d) { _t%d = %s; _t%d = _t%d; } if (_t%d > _t%d) { _t%d = %s; _t%d = _t%d; } }\n",
                 tcur, tbvmin, tmin, mmelem, tbvmin, tcur, tcur, tbvmax, tmax, mmelem, tbvmax, tcur);
    }
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
    /* Yield a poly [min, max] (CRuby returns a generic Array). An empty receiver
       yields [nil, nil] -- which a typed array cannot represent -- so the result
       is always a poly array regardless of the receiver kind. */
    char minref[24], maxref[24];
    snprintf(minref, sizeof minref, "_t%d", tmin);
    snprintf(maxref, sizeof maxref, "_t%d", tmax);
    Buf bmin; memset(&bmin, 0, sizeof bmin); emit_boxed_text(c, et, minref, &bmin);
    Buf bmax; memset(&bmax, 0, sizeof bmax); emit_boxed_text(c, et, maxref, &bmax);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tout, tout);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "if (_t%d) { sp_PolyArray_push(_t%d, sp_box_nil()); sp_PolyArray_push(_t%d, sp_box_nil()); }\n", tf, tout, tout);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "else { sp_PolyArray_push(_t%d, %s); sp_PolyArray_push(_t%d, %s); }\n",
               tout, bmin.p ? bmin.p : "sp_box_nil()", tout, bmax.p ? bmax.p : "sp_box_nil()");
    free(bmin.p); free(bmax.p);
    buf_printf(b, "_t%d", tout);
    return 1;
  }
  int trecv = ++g_tmp, tbest = ++g_tmp, tbv = ++g_tmp, tf = ++g_tmp, ti = ++g_tmp, tcur = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);  /* recv value; its own preludes flow to g_pre */
  if (is_range) {
    int trng = ++g_tmp;
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_Range _t%d = %s;\n", trng, rb.p ? rb.p : "");
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_IntArray *_t%d = sp_range_to_ia(_t%d);\n", trecv, trng);
    /* freshly allocated and held only here; root it before the block walk,
       whose body may allocate and trigger a collection */
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", trecv);
  }
  else { emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", trecv); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); }
  free(rb.p);
  emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tbest, et == TY_RANGE ? "(sp_Range){0}" : default_value(et));
  emit_indent(g_pre, g_indent); emit_ctype(c, bvt, g_pre); buf_printf(g_pre, " _t%d = %s; int _t%d = 1;\n", tbv, default_value(bvt), tf);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, k, trecv, ti);
  char mmelem[24];
  if (autosplat) {
    int telem = ++g_tmp;
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);\n", telem, trecv, ti);
    emit_autosplat_params(c, block, np_mb, telem, g_indent + 1);
    snprintf(mmelem, sizeof mmelem, "_t%d", telem);
  } else {
    if (p0) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti); }
    snprintf(mmelem, sizeof mmelem, "lv_%s", p0 ? p0 : "");
  }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
  Scope *mbsc = (p0 && !autosplat) ? comp_scope_of(c, block) : NULL;
  LocalVar *mlv0 = (mbsc && p0) ? scope_local(mbsc, p0) : NULL;
  TyKind mpt0 = mlv0 ? mlv0->type : TY_UNKNOWN;
  if (mlv0) mlv0->type = et;
  int save = g_indent; g_indent++;
  Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = save;
  if (mlv0) mlv0->type = mpt0;
  emit_indent(g_pre, g_indent + 1); emit_ctype(c, bvt, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tcur, vb.p ? vb.p : default_value(bvt)); free(vb.p);
  emit_indent(g_pre, g_indent + 1);
  if (bvt == TY_POLY)
    buf_printf(g_pre, "if (_t%d || sp_poly_%s(_t%d, _t%d)) { _t%d = %s; _t%d = _t%d; _t%d = 0; }\n",
               tf, is_max ? "gt" : "lt", tcur, tbv, tbest, mmelem, tbv, tcur, tf);
  else
    buf_printf(g_pre, "if (_t%d || _t%d %s _t%d) { _t%d = %s; _t%d = _t%d; _t%d = 0; }\n",
               tf, tcur, is_max ? ">" : "<", tbv, tbest, mmelem, tbv, tcur, tf);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tbest);
  return 1;
}

/* poly `uniq`/`uniq!` with a block, as an expression: the receiver boxes an
   array; keep the first element for each distinct block-key value (compared with
   sp_poly_eq), and for the bang form write the survivors back in place. Yields
   the (boxed) array. Returns 1 if handled. */
int emit_poly_uniq_block(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || (!sp_streq(name, "uniq") && !sp_streq(name, "uniq!"))) return 0;
  int recv = nt_ref(nt, id, "receiver");
  int block = nt_ref(nt, id, "block");
  if (recv < 0 || block < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; if (args >= 0) nt_arr(nt, args, "arguments", &argc);
  if (argc != 0) return 0;
  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (!p0 || bn < 1) return 0;
  int bang = sp_streq(name, "uniq!");

  /* Typed or poly array receiver (sp_<K>Array): dedup keeping the same element
     type, using the block's return value as the uniqueness key. */
  const char *rk = array_kind(rt);
  if (!rk && rt == TY_POLY_ARRAY) rk = "Poly";
  if (rk) {
    TyKind et = ty_array_elem(rt);
    /* If the block param was widened to a wider type than the receiver's
       element type (e.g. the same name is poly in another block in this scope),
       its lv_ is declared at the wider C type. Shadow it with an et-typed local
       inside a fresh C block and re-infer the body, so the typed-array get
       assigns into a matching lvalue. Mirrors emit_filter_map/emit_partition. */
    Scope *csc = p0 ? comp_scope_of(c, block) : NULL;
    LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
    TyKind csaved0 = clv0 ? clv0->type : TY_UNKNOWN;
    int use_shadow = clv0 && clv0->type != et && et != TY_UNKNOWN;
    int trecv = ++g_tmp, tseen = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent);
    emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : "NULL"); free(rb.p);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", trecv);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tseen, tseen);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);\n", rk, tres, rk, tres);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
    int din = g_indent + 1;
    if (use_shadow) {
      clv0->type = et;
      for (int j = 0; j < bn; j++) infer_type(c, bb[j]);
      emit_indent(g_pre, din); buf_puts(g_pre, "{\n"); din++;
      emit_indent(g_pre, din); emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, rk, trecv, ti);
    }
    else { emit_indent(g_pre, din); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, rk, trecv, ti); }
    for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, din);
    int tkey = ++g_tmp, tdup = ++g_tmp, tj = ++g_tmp;
    int save = g_indent; g_indent = din;
    Buf kb; memset(&kb, 0, sizeof kb); emit_boxed(c, bb[bn - 1], &kb); g_indent = save;
    emit_indent(g_pre, din); buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", tkey, kb.p ? kb.p : "sp_box_nil()"); free(kb.p);
    emit_indent(g_pre, din);
    buf_printf(g_pre, "int _t%d = 0; for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) if (sp_poly_eq(_t%d->data[_t%d], _t%d)) { _t%d = 1; break; }\n",
               tdup, tj, tj, tseen, tj, tseen, tj, tkey, tdup);
    emit_indent(g_pre, din);
    buf_printf(g_pre, "if (!_t%d) { sp_PolyArray_push(_t%d, _t%d); sp_%sArray_push(_t%d, lv_%s); }\n", tdup, tseen, tkey, rk, tres, p0);
    if (use_shadow) { din--; emit_indent(g_pre, din); buf_puts(g_pre, "}\n"); }
    if (clv0) clv0->type = csaved0;
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
    if (bang) {
      int tm = ++g_tmp, tn = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "mrb_int _t%d = _t%d->len; _t%d->len = 0; for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d));\n",
                 tn, tres, trecv, tm, tm, tn, tm, rk, trecv, rk, tres, tm);
      buf_printf(b, "_t%d", trecv);
    } else {
      buf_printf(b, "_t%d", tres);
    }
    return 1;
  }

  if (rt != TY_POLY) return 0;
  int trecv = ++g_tmp, tarr = ++g_tmp, tseen = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", trecv, rb.p ? rb.p : "sp_box_nil()"); free(rb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = (sp_PolyArray *)_t%d.v.p;\n", tarr, trecv);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tseen, tseen);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres, tres);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, tarr, ti);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = _t%d->data[_t%d];\n", p0, tarr, ti);
  int save = g_indent; g_indent++;
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent);
  int tkey = ++g_tmp, tdup = ++g_tmp, tj = ++g_tmp;
  Buf kb; memset(&kb, 0, sizeof kb); emit_boxed(c, bb[bn - 1], &kb); g_indent = save;
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", tkey, kb.p ? kb.p : "sp_box_nil()"); free(kb.p);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "int _t%d = 0; for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) if (sp_poly_eq(_t%d->data[_t%d], _t%d)) { _t%d = 1; break; }\n",
             tdup, tj, tj, tseen, tj, tseen, tj, tkey, tdup);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "if (!_t%d) { sp_PolyArray_push(_t%d, _t%d); sp_PolyArray_push(_t%d, lv_%s); }\n", tdup, tseen, tkey, tres, p0);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  if (bang) {
    int tm = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "_t%d->len = 0; for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) sp_PolyArray_push(_t%d, _t%d->data[_t%d]);\n",
               tarr, tm, tm, tres, tm, tarr, tres, tm);
    buf_printf(b, "_t%d", trecv);
  }
  else buf_printf(b, "sp_box_poly_array(_t%d)", tres);
  return 1;
}

/* "str".gsub(/re/) { |m| repl } / sub as an expression: iterate the matches
   of a regex literal, binding the block param to each matched substring and
   appending its return value as the replacement. sub replaces only the first
   match. Anchored patterns (^/$) are matched per-remainder, so this targets
   the unanchored block forms. Returns 1 if handled. */
int emit_gsub_block_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || (!sp_streq(name, "gsub") && !sp_streq(name, "sub"))) return 0;
  int once = sp_streq(name, "sub");
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
int emit_sum_block_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "sum")) return 0;
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
  /* a float initial value promotes the whole sum to Float, even when the block
     yields integers (matches analyze and CRuby): accumulate in floating point
     rather than truncating the init into an integer accumulator. */
  if (argc == 1 && argv && comp_ntype(c, argv[0]) == TY_FLOAT) acct = TY_FLOAT;
  int ta = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp;
  /* Float accumulation uses Kahan-Babuska-Neumaier compensation (matches
     CRuby's Array#sum), so it needs a running compensation temp plus
     per-iteration x/t temps. Integer sums use none of them. */
  int tc = -1, tx = -1, tt = -1;
  if (acct == TY_FLOAT) { tc = ++g_tmp; tx = ++g_tmp; tt = ++g_tmp; }
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
  if (acct == TY_FLOAT) buf_printf(b, "; mrb_float _t%d = 0.0", tc);
  buf_printf(b, "; for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) { ", ti, ti, tn, ti);
  if (p0) buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d); ", p0, k, ta, ti);
  /* The block's value expression may spill setup statements to g_pre (e.g.
     a nested count loop). Those must run per iteration: redirect g_pre into
     a local buffer while emitting the value, then splice it into the loop
     body ahead of the accumulation. */
  {
    Buf inner; memset(&inner, 0, sizeof inner);
    Buf valb; memset(&valb, 0, sizeof valb);
    Buf *saved_pre = g_pre; g_pre = &inner;
    emit_expr(c, bb[bn - 1], &valb);
    g_pre = saved_pre;
    if (inner.p) buf_puts(b, inner.p);
    if (acct == TY_INT) {
      buf_printf(b, "_t%d = sp_int_add(_t%d, %s)", tacc, tacc, valb.p ? valb.p : "0");
    }
    else {
      /* KBN step: fold the low-order bits dropped by _tacc + _tx into _tc. */
      buf_printf(b, "mrb_float _t%d = %s; mrb_float _t%d = _t%d + _t%d; "
                    "if (fabs(_t%d) >= fabs(_t%d)) _t%d += (_t%d - _t%d) + _t%d; "
                    "else _t%d += (_t%d - _t%d) + _t%d; _t%d = _t%d",
                 tx, valb.p ? valb.p : "0.0", tt, tacc, tx,
                 tacc, tx, tc, tacc, tt, tx,
                 tc, tx, tt, tacc, tacc, tt);
    }
    free(inner.p); free(valb.p);
  }
  if (acct == TY_FLOAT) buf_printf(b, "; } _t%d + _t%d; })", tacc, tc);
  else buf_printf(b, "; } _t%d; })", tacc);
  return 1;
}

/* int_array.slice_when { |a, b| cond }[.to_a].inspect  or
   int_array.chunk { |x| key }[.to_a].inspect  ->  inspect string.
   Emits setup to g_pre and the result variable to b. Returns 1 if handled. */
int emit_slice_when_chunk_inspect_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "inspect")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  /* allow .to_a wrapper */
  if (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "to_a"))
    recv = nt_ref(nt, recv, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) return 0;
  const char *m = nt_str(nt, recv, "name");
  if (!m) return 0;
  int is_sw = sp_streq(m, "slice_when");
  int is_ck = sp_streq(m, "chunk");
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

/* int_array.chunk_while { |a, b| cond }.to_a -> array of runs. Adjacent elements
   stay in one run while the block is true; a boundary falls where it is false
   (the inverse of slice_when). Materialized as a poly array of boxed int arrays
   so the result is first-class -- `p`, indexing, and further iteration work.
   Emits setup to g_pre and the result var to b. Returns 1 if handled. */
int emit_chunk_while_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "to_a")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) return 0;
  const char *m = nt_str(nt, recv, "name");
  if (!m || !sp_streq(m, "chunk_while")) return 0;
  int block = nt_ref(nt, recv, "block");
  if (block < 0) return 0;
  int pr = nt_ref(nt, recv, "receiver");
  if (pr < 0 || comp_ntype(c, pr) != TY_INT_ARRAY) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  const char *p0n = block_param_name(c, block, 0);
  const char *p1n = block_param_name(c, block, 1);
  if (!p0n || !p1n) return 0;
  const char *p0 = rename_local(p0n), *p1 = rename_local(p1n);

  int ta = ++g_tmp, tout = ++g_tmp, tcur = ++g_tmp, ti = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, pr, &rb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_IntArray *_t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", ta);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tout, tout);
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
  buf_printf(g_pre, "if (!(%s)) {\n", cb.p ? cb.p : "0"); free(cb.p);
  emit_indent(g_pre, g_indent + 3);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_box_int_array(_t%d));\n", tout, tcur);
  emit_indent(g_pre, g_indent + 3);
  buf_printf(g_pre, "_t%d = sp_IntArray_new();\n", tcur);
  emit_indent(g_pre, g_indent + 2); buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "if (sp_IntArray_length(_t%d) > 0) sp_PolyArray_push(_t%d, sp_box_int_array(_t%d));\n", tcur, tout, tcur);
  buf_printf(b, "_t%d", tout);
  return 1;
}

/* int_array.product(int_array)[.to_a].inspect -> the Cartesian product
   rendered as a nested-array string. The product result has no first-class
   type, so only this inline inspect chain is supported. Returns 1 if handled. */
int emit_product_inspect_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "inspect")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  /* allow an intervening .to_a */
  if (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "to_a"))
    recv = nt_ref(nt, recv, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) return 0;
  const char *m = nt_str(nt, recv, "name");
  if (!m) return 0;
  int is_product = sp_streq(m, "product");
  int is_slice = sp_streq(m, "slice_before") || sp_streq(m, "slice_after");
  if (!is_product && !is_slice) return 0;
  int pr = nt_ref(nt, recv, "receiver");
  int pargs = nt_ref(nt, recv, "arguments");
  int pac = 0; const int *pav = pargs >= 0 ? nt_arr(nt, pargs, "arguments", &pac) : NULL;
  if (pr < 0 || pac != 1) return 0;
  if (comp_ntype(c, pr) != TY_INT_ARRAY) return 0;
  if (is_product) {
    /* the other operand is an int_array or an empty array literal */
    TyKind at = comp_ntype(c, pav[0]);
    int empty_lit = nt_type(nt, pav[0]) && sp_streq(nt_type(nt, pav[0]), "ArrayNode") &&
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
int emit_step_array_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  if (nt_ref(nt, id, "block") >= 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "step")) return 0;
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
int emit_inject_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || (!sp_streq(name, "inject") && !sp_streq(name, "reduce"))) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  /* empty array literal `[]` has TY_UNKNOWN; treat as TY_INT_ARRAY */
  if (rt == TY_UNKNOWN && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode")) {
    int en = 0; nt_arr(nt, recv, "elements", &en);
    if (en == 0) rt = TY_INT_ARRAY;
  }
  /* `[[ints],...].inject(&:&|:||:-)`: fold the inner int arrays with a set op.
     Handled before the typed-array path since poly arrays have no array_kind. */
  if (rt == TY_POLY_ARRAY && comp_is_nested_int_array_literal(c, recv)) {
    int sblk = nt_ref(nt, id, "block");
    const char *sop = NULL;
    if (sblk >= 0 && nt_type(nt, sblk) && sp_streq(nt_type(nt, sblk), "BlockArgumentNode")) {
      int ex = nt_ref(nt, sblk, "expression");
      if (ex >= 0 && nt_type(nt, ex) && sp_streq(nt_type(nt, ex), "SymbolNode")) sop = nt_str(nt, ex, "value");
    }
    if (sop && (sp_streq(sop, "&") || sp_streq(sop, "|") || sp_streq(sop, "-"))) {
      const char *sfn = sp_streq(sop, "&") ? "sp_IntArray_intersect"
                      : sp_streq(sop, "|") ? "sp_IntArray_union" : "sp_IntArray_difference";
      int ta = ++g_tmp, tn = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = ", ta); emit_expr(c, recv, b);
      buf_printf(b, "; mrb_int _t%d = sp_PolyArray_length(_t%d); ", tn, ta);
      buf_printf(b, "sp_IntArray *_t%d = _t%d > 0 ? (sp_IntArray *)sp_PolyArray_get(_t%d, 0).v.p : sp_IntArray_new(); ", tacc, tn, ta);
      buf_printf(b, "for (mrb_int _t%d = 1; _t%d < _t%d; _t%d++) _t%d = %s(_t%d, (sp_IntArray *)sp_PolyArray_get(_t%d, _t%d).v.p); ",
                 ti, ti, tn, ti, tacc, sfn, tacc, ta, ti);
      buf_printf(b, "_t%d; })", tacc);
      return 1;
    }
  }
  if (!ty_is_array(rt)) return 0;
  const char *k = array_kind(rt);
  if (!k) return 0;
  TyKind et = ty_array_elem(rt);

  /* find the operator symbol (from a &:op block or a trailing :op arg) and
     any explicit initial value */
  const char *op = NULL; int init = -1;
  int block = nt_ref(nt, id, "block");
  if (block >= 0 && nt_type(nt, block) && sp_streq(nt_type(nt, block), "BlockArgumentNode")) {
    int ex = nt_ref(nt, block, "expression");
    if (ex >= 0 && nt_type(nt, ex) && sp_streq(nt_type(nt, ex), "SymbolNode")) op = nt_str(nt, ex, "value");
  }
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (!op && argc >= 1) {
    int last = argv[argc - 1];
    if (nt_type(nt, last) && sp_streq(nt_type(nt, last), "SymbolNode")) {
      op = nt_str(nt, last, "value");
      if (argc == 2) init = argv[0];
    }
  }
  if (!op) return 0;

  const char *ifn = (et == TY_INT) ? int_arith_fn(op) : NULL;
  /* bitwise ops on integers: &, |, ^, <<, >> -- use operator directly */
  int int_bitop = (et == TY_INT) && !ifn &&
                  (sp_streq(op, "&") || sp_streq(op, "|") || sp_streq(op, "^") ||
                   sp_streq(op, "<<") || sp_streq(op, ">>"));
  int float_op = (et == TY_FLOAT) && (sp_streq(op, "+") || sp_streq(op, "-") ||
                                      sp_streq(op, "*") || sp_streq(op, "/"));
  int str_op = (et == TY_STRING) && sp_streq(op, "+");
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
int emit_reduce_block_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || (!sp_streq(name, "inject") && !sp_streq(name, "reduce"))) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  /* `[[ints],...].inject { |a, b| a & b }`: the inner int arrays are boxed in a
     poly array; fold them as int arrays (unboxing each element). The poly array
     itself has no array_kind, so detect this before the typed-array bail. */
  int nested = (rt == TY_POLY_ARRAY && comp_is_nested_int_array_literal(c, recv));
  const char *k = array_kind(rt);
  if (!k && !nested) return 0;
  if (nested) k = "Poly";  /* length via sp_PolyArray_length; elements unboxed below */
  TyKind et = nested ? TY_INT_ARRAY : ty_array_elem(rt);
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *bty = nt_type(nt, block);
  if (!bty || !sp_streq(bty, "BlockNode")) return 0;
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
  /* An int seed folded over floats accumulates float (matches the reduce
     return-type promotion in infer_type); keep the C accumulator type in step.
     Only a numeric body promotes -- a poly body keeps the seed type (codegen
     re-types the params and re-infers the body under the shadow below). */
  { TyKind bt = comp_ntype(c, bb[bn - 1]); if (ty_is_numeric(bt)) acc_ty = ty_promote_numeric(acc_ty, bt); }
  int ta = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp;
  buf_puts(b, "({ ");
  emit_ctype(c, rt, b); buf_printf(b, " _t%d = ", ta); emit_expr(c, recv, b); buf_puts(b, "; ");
  emit_ctype(c, acc_ty, b); buf_printf(b, " _t%d = ", tacc);
  int start;
  if (init >= 0) { emit_expr(c, init, b); buf_puts(b, "; "); start = 0; }
  else if (nested) { buf_printf(b, "sp_PolyArray_length(_t%d) > 0 ? (sp_IntArray *)sp_PolyArray_get(_t%d, 0).v.p : sp_IntArray_new(); ", ta, ta); start = 1; }
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
  if (nested) { emit_ctype(c, et, b); buf_printf(b, " lv_%s = (sp_IntArray *)sp_PolyArray_get(_t%d, _t%d).v.p; ", p1, ta, ti); }
  else { emit_ctype(c, et, b); buf_printf(b, " lv_%s = sp_%sArray_get(_t%d, _t%d); ", p1, k, ta, ti); }
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

/* arr.each.with_index(off).<terminal> { ... } : `each.with_index` is a blockless
   enumerator yielding [element, index] pairs, and the chained terminal folds or
   consumes them. The block binds the pair either as two params |v, i| (auto-split),
   a destructured |(v, i)|, or a single |pair| (a 2-element array). inject also
   takes the accumulator as its first param: |acc, (v, i)| / |acc, pair|.
   (matz/spinel#1481 inject/reduce; #1483 map/to_a/select/count/any?/...)

   Returns 0 unless the exact each.with_index chain shape matches, so no other
   call path is affected. */

/* Recognise the chain; fill *out_arr (the source array node) and *out_off (the
   with_index offset arg node, or -1). Returns 1 on match. */
static int ewi_chain(Compiler *c, int id, int *out_arr, int *out_off) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) return 0;
  if (nt_ref(nt, recv, "block") >= 0) return 0;
  const char *rn = nt_str(nt, recv, "name");
  if (!rn) return 0;
  /* `arr.each_with_index` is the same [elem, index] pair enumerator (offset 0). */
  if (sp_streq(rn, "each_with_index")) {
    int arr = nt_ref(nt, recv, "receiver");
    if (arr < 0) return 0;
    *out_arr = arr; *out_off = -1;
    return 1;
  }
  /* `arr.each.with_index(off)` */
  if (!sp_streq(rn, "with_index")) return 0;
  int wir = nt_ref(nt, recv, "receiver");
  if (wir < 0 || !nt_type(nt, wir) || !sp_streq(nt_type(nt, wir), "CallNode")) return 0;
  const char *en = nt_str(nt, wir, "name");
  if (!en || !sp_streq(en, "each") || nt_ref(nt, wir, "block") >= 0) return 0;
  int arr = nt_ref(nt, wir, "receiver");
  if (arr < 0) return 0;
  int wargs = nt_ref(nt, recv, "arguments");
  int wargc = 0; const int *wargv = wargs >= 0 ? nt_arr(nt, wargs, "arguments", &wargc) : NULL;
  *out_arr = arr;
  *out_off = (wargc > 0 && wargv) ? wargv[0] : -1;
  return 1;
}

int emit_each_with_index_chain(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int is_inject = sp_streq(name, "inject") || sp_streq(name, "reduce");
  if (!is_inject) return 0;  /* other terminals handled in a later pass */

  int arr = -1, off = -1;
  if (!ewi_chain(c, id, &arr, &off)) return 0;
  TyKind rt = comp_ntype(c, arr);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind elem_t = ty_array_elem(rt);

  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *p0o = block_param_name(c, block, 0);   /* accumulator */
  if (!p0o) return 0;
  /* pair binding lives in param 1: either a |(v,i)| multi-target or a |pair| name */
  int multi = block_param_is_multi(c, block, 1);
  const char *vo = NULL, *io = NULL, *pairo = NULL;
  if (multi) {
    if (block_param_multi_count(c, block, 1) < 2) return 0;
    vo = block_param_multi_leaf(c, block, 1, 0);
    io = block_param_multi_leaf(c, block, 1, 1);
    if (!vo || !io) return 0;
  }
  else {
    pairo = block_param_name(c, block, 1);
    if (!pairo) return 0;
  }
  int bbody = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
  if (bn == 0) return 0;

  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int init = (argc > 0 && argv) ? argv[0] : -1;
  TyKind acc_ty = elem_t;
  if (init >= 0) { TyKind it = comp_ntype(c, init); if (it != TY_UNKNOWN) acc_ty = it; }
  { TyKind bt = comp_ntype(c, bb[bn - 1]); if (ty_is_numeric(bt)) acc_ty = ty_promote_numeric(acc_ty, bt); }

  const char *p0 = rename_local(p0o);
  TyKind pair_ty = (elem_t == TY_INT) ? TY_INT_ARRAY : TY_POLY_ARRAY;
  const char *pk = (elem_t == TY_INT) ? "Int" : "Poly";

  int ta = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp, tidx = ++g_tmp;
  buf_puts(b, "({ ");
  emit_ctype(c, rt, b); buf_printf(b, " _t%d = ", ta); emit_expr(c, arr, b); buf_puts(b, "; ");
  emit_ctype(c, acc_ty, b); buf_printf(b, " _t%d = ", tacc);
  if (init >= 0) emit_expr(c, init, b); else buf_puts(b, "0");
  buf_puts(b, "; ");
  buf_printf(b, "mrb_int _t%d = ", tidx);
  if (off >= 0) emit_expr(c, off, b); else buf_puts(b, "0");
  buf_puts(b, "; ");

  /* Override block-param types so the body expression types correctly, then
     re-infer (same pattern as emit_reduce_block_expr). */
  Scope *rsc = comp_scope_of(c, block);
  LocalVar *lacc = rsc ? scope_local(rsc, p0o) : NULL;
  TyKind sacc = lacc ? lacc->type : TY_UNKNOWN; if (lacc) lacc->type = acc_ty;
  LocalVar *lv = NULL, *li = NULL, *lp = NULL; TyKind sv = TY_UNKNOWN, si = TY_UNKNOWN, sp = TY_UNKNOWN;
  if (multi) {
    lv = rsc ? scope_local(rsc, vo) : NULL; li = rsc ? scope_local(rsc, io) : NULL;
    sv = lv ? lv->type : TY_UNKNOWN; si = li ? li->type : TY_UNKNOWN;
    if (lv) lv->type = elem_t; if (li) li->type = TY_INT;
  }
  else {
    lp = rsc ? scope_local(rsc, pairo) : NULL; sp = lp ? lp->type : TY_UNKNOWN;
    if (lp) lp->type = pair_ty;
  }
  for (int j = 0; j < bn; j++) infer_type(c, bb[j]);

  buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++, _t%d++) { ",
             ti, ti, k, ta, ti, tidx);
  buf_puts(b, "{ ");
  emit_ctype(c, acc_ty, b); buf_printf(b, " lv_%s = _t%d; ", p0, tacc);
  if (multi) {
    emit_ctype(c, elem_t, b); buf_printf(b, " lv_%s = sp_%sArray_get(_t%d, _t%d); ", rename_local(vo), k, ta, ti);
    buf_printf(b, "mrb_int lv_%s = _t%d; ", rename_local(io), tidx);
  }
  else {
    buf_printf(b, "sp_%sArray *lv_%s = sp_%sArray_new(); ", pk, rename_local(pairo), pk);
    if (elem_t == TY_INT) {
      buf_printf(b, "sp_IntArray_push(lv_%s, sp_%sArray_get(_t%d, _t%d)); sp_IntArray_push(lv_%s, _t%d); ",
                 rename_local(pairo), k, ta, ti, rename_local(pairo), tidx);
    }
    else {
      buf_printf(b, "sp_PolyArray_push(lv_%s, ", rename_local(pairo));
      char src[96]; snprintf(src, sizeof src, "sp_%sArray_get(_t%d, _t%d)", k, ta, ti);
      Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, elem_t, src, &bx); buf_puts(b, bx.p ? bx.p : ""); free(bx.p);
      buf_printf(b, "); sp_PolyArray_push(lv_%s, sp_box_int(_t%d)); ", rename_local(pairo), tidx);
    }
  }
  for (int j = 0; j < bn - 1; j++) { emit_stmt(c, bb[j], b, 0); buf_puts(b, " "); }
  buf_printf(b, "_t%d = ", tacc); emit_expr(c, bb[bn - 1], b); buf_puts(b, "; } } ");
  buf_printf(b, "_t%d; })", tacc);

  if (lacc) lacc->type = sacc;
  if (lv) lv->type = sv; if (li) li->type = si; if (lp) lp->type = sp;
  return 1;
}

/* defined below, before emit_collect_expr; used by the each-chain terminals */
static void emit_block_value_into(Compiler *c, int block, const char *dest,
                                  int want_poly, int indent);

/* The non-fold terminals over arr.each.with_index / arr.each_with_index:
   map/collect (collect block value), select/filter & reject & to_a/entries
   (collect the [v,i] pair), count, any?/all?/none? (scalar), each (side effect,
   returns the receiver). Emits the loop into g_pre; the result tmp lands in `b`.
   Returns 1 if handled. (matz/spinel#1483) */
int emit_each_with_index_terminal(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int is_map = sp_streq(name, "map") || sp_streq(name, "collect");
  int is_fmap = sp_streq(name, "filter_map");
  int is_sel = sp_streq(name, "select") || sp_streq(name, "filter");
  int is_rej = sp_streq(name, "reject");
  int is_toa = sp_streq(name, "to_a") || sp_streq(name, "entries");
  int is_cnt = sp_streq(name, "count");
  int is_any = sp_streq(name, "any?"), is_all = sp_streq(name, "all?"), is_none = sp_streq(name, "none?");
  int is_each = sp_streq(name, "each");
  int is_toh = sp_streq(name, "to_h");
  if (!(is_map || is_fmap || is_sel || is_rej || is_toa || is_cnt || is_any || is_all || is_none || is_each || is_toh)) return 0;

  int arr = -1, off = -1;
  if (!ewi_chain(c, id, &arr, &off)) return 0;
  TyKind rt = comp_ntype(c, arr);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind elem_t = ty_array_elem(rt);

  int block = nt_ref(nt, id, "block");
  if (is_toh && block >= 0) return 0;   /* block-form to_h maps each pair; not this {elem => index} lowering */
  if (!is_toa && !is_toh && block < 0) return 0;   /* only to_a/entries/to_h work without a block */

  /* each.with_index yields (element, index) as multiple values, so block-arg
     semantics are ordinary: |v, i| binds both; a single |x| binds the element
     (index discarded); a single destructure |(v,i)| would destructure the
     element (an MRI corner, v=elem/i=nil) -- not worth it, so bail on it. */
  /* Only the unambiguous |v, i| two-param form is handled; a single |x| param
     has method-dependent yield semantics in MRI (map binds the element,
     select binds the pair), so leave those to other rules. */
  const char *vo = NULL, *io = NULL;
  if (block >= 0) {
    if (block_param_is_multi(c, block, 0)) return 0;
    vo = block_param_name(c, block, 0);
    io = block_param_name(c, block, 1);
    if (!vo || !io) return 0;
  }

  int collect_pair = is_sel || is_rej || is_toa;   /* select/reject/to_a collect the [elem,index] pair */
  int need_pair = collect_pair;
  const char *pk = (elem_t == TY_INT) ? "Int" : "Poly";
  TyKind pair_ty = (elem_t == TY_INT) ? TY_INT_ARRAY : TY_POLY_ARRAY;

  int ta = ++g_tmp, ti = ++g_tmp, tidx = ++g_tmp;
  int tres = 0, tcnt = 0, tflag = 0;
  TyKind toh_ht = TY_UNKNOWN; const char *toh_hcn = NULL;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, arr, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", ta);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = ", tidx);
  if (off >= 0) emit_expr(c, off, g_pre); else buf_puts(g_pre, "0");
  buf_puts(g_pre, ";\n");

  const char *rk = NULL;
  if (is_map || is_fmap) {
    TyKind restype = comp_ntype(c, id);
    rk = (restype == TY_POLY_ARRAY) ? "Poly" : array_kind(restype);
    if (!rk) rk = "Poly";
    tres = ++g_tmp;
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);\n", rk, tres, rk, tres);
  }
  else if (collect_pair) {
    tres = ++g_tmp;
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres, tres);
  }
  else if (is_toh) {
    /* each_with_index.to_h builds {element => index}; the key type is the
       element type, the value type is the index (int). */
    toh_ht = comp_ntype(c, id);
    if (!ty_is_hash(toh_ht)) toh_ht = TY_POLY_POLY_HASH;
    toh_hcn = ty_hash_cname(toh_ht);
    tres = ++g_tmp;
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);\n", toh_hcn, tres, toh_hcn, tres);
  }
  else if (is_cnt) {
    tcnt = ++g_tmp; emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = 0;\n", tcnt);
  }
  else if (is_any || is_all || is_none) {
    tflag = ++g_tmp; emit_indent(g_pre, g_indent); buf_printf(g_pre, "int _t%d = %d;\n", tflag, (is_all || is_none) ? 1 : 0);
  }

  /* override block-param types so the body expression types correctly */
  Scope *bsc = block >= 0 ? comp_scope_of(c, block) : NULL;
  LocalVar *lv = NULL, *li = NULL; TyKind sv = TY_UNKNOWN, si = TY_UNKNOWN;
  if (block >= 0) {
    lv = scope_local(bsc, vo); sv = lv ? lv->type : TY_UNKNOWN; if (lv) lv->type = elem_t;
    li = scope_local(bsc, io); si = li ? li->type : TY_UNKNOWN; if (li) li->type = TY_INT;
    int body = nt_ref(nt, block, "body");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    for (int j = 0; j < bn; j++) infer_type(c, bb[j]);
  }

  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++, _t%d++) {\n", ti, ti, k, ta, ti, tidx);
  int din = g_indent + 1;

  int tpair = 0;
  if (need_pair) {
    tpair = ++g_tmp;
    emit_indent(g_pre, din); buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);\n", pk, tpair, pk, tpair);
    if (elem_t == TY_INT) {
      emit_indent(g_pre, din);
      buf_printf(g_pre, "sp_IntArray_push(_t%d, sp_%sArray_get(_t%d, _t%d)); sp_IntArray_push(_t%d, _t%d);\n", tpair, k, ta, ti, tpair, tidx);
    }
    else {
      emit_indent(g_pre, din); buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", tpair);
      char src[96]; snprintf(src, sizeof src, "sp_%sArray_get(_t%d, _t%d)", k, ta, ti);
      Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, elem_t, src, &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p);
      buf_printf(g_pre, "); sp_PolyArray_push(_t%d, sp_box_int(_t%d));\n", tpair, tidx);
    }
  }
  if (block >= 0) {
    emit_indent(g_pre, din); emit_ctype(c, elem_t, g_pre); buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", rename_local(vo), k, ta, ti);
    emit_indent(g_pre, din); buf_printf(g_pre, "mrb_int lv_%s = _t%d;\n", rename_local(io), tidx);
  }

  int body = block >= 0 ? nt_ref(nt, block, "body") : -1;
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  /* fresh block-locals per iteration (this emitter walks the body itself
     instead of going through emit_stmts, so reset explicitly) */
  if (block >= 0) emit_block_locals_reset(c, block, g_pre, din);
  if (is_each) {
    for (int j = 0; j < bn; j++) emit_stmt(c, bb[j], g_pre, din);
  }
  else if (collect_pair && block < 0) {   /* to_a / entries */
    emit_indent(g_pre, din); buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", tres);
    Buf bx; memset(&bx, 0, sizeof bx); char pe[32]; snprintf(pe, sizeof pe, "_t%d", tpair); emit_boxed_text(c, pair_ty, pe, &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p);
    buf_puts(g_pre, ");\n");
  }
  else if (is_toh) {   /* to_h: set {element => index} directly into the hash */
    emit_indent(g_pre, din);
    TyKind kty = ty_hash_key(toh_ht), vty = ty_hash_val(toh_ht);
    char keyexpr[96]; snprintf(keyexpr, sizeof keyexpr, "sp_%sArray_get(_t%d, _t%d)", k, ta, ti);
    buf_printf(g_pre, "sp_%sHash_set(_t%d, ", toh_hcn, tres);
    if (kty == TY_POLY) { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, elem_t, keyexpr, &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p); }
    else buf_puts(g_pre, keyexpr);
    if (vty == TY_POLY) buf_printf(g_pre, ", sp_box_int(_t%d));\n", tidx);
    else buf_printf(g_pre, ", _t%d);\n", tidx);
  }
  else {   /* map / select / reject / count / any? / all? / none? -- need the block value */
    /* collect the block's value (next-aware) into a temp so an interior or tail
       `next <v>` contributes <v> rather than being dropped as a skip. */
    /* An empty block body (e.g. `map { |x, i| }`) has bn == 0, so guard the
       bb[bn - 1] read and treat the absent tail value as nil -- a poly value so
       the temp initializes to sp_box_nil() rather than an integer 0. */
    TyKind bt = bn > 0 ? comp_ntype(c, bb[bn - 1]) : TY_NIL;
    if (bt == TY_UNKNOWN) bt = TY_INT;
    int vpoly = (bt == TY_POLY || bt == TY_NIL);
    int tv = ++g_tmp; char tvb[24]; snprintf(tvb, sizeof tvb, "_t%d", tv);
    emit_indent(g_pre, din);
    if (vpoly) buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil();\n", tv);
    else { emit_ctype(c, bt, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tv, default_value(bt)); }
    emit_block_value_into(c, block, tvb, vpoly, din);
    /* Ruby truthiness of the block value: only nil/false are falsy. C
       zero-falsiness ("(_tN)") would wrongly drop a numeric 0 / 0.0 (or an
       empty string), which Ruby keeps -- so mirror emit_cond's per-type test
       on the temp: a boxed poly goes through sp_poly_truthy, a nilable scalar
       tests its sentinel, and every other concrete value is always truthy. */
    char truth[48];
    if (vpoly)                        snprintf(truth, sizeof truth, "sp_poly_truthy(_t%d)", tv);
    else if (bt == TY_BOOL)           snprintf(truth, sizeof truth, "(_t%d)", tv);
    else if (bt == TY_INT)            snprintf(truth, sizeof truth, "(_t%d != SP_INT_NIL)", tv);
    else if (bt == TY_FLOAT)          snprintf(truth, sizeof truth, "(!sp_float_is_nil(_t%d))", tv);
    else if (bt == TY_SYMBOL)         snprintf(truth, sizeof truth, "(_t%d != (sp_sym)-1)", tv);
    else if (comp_ty_value_obj(c, bt)) snprintf(truth, sizeof truth, "1");
    else if (bt == TY_STRING || ty_is_array(bt) || ty_is_hash(bt) || ty_is_object(bt) ||
             bt == TY_PROC || bt == TY_MATCHDATA ||
             bt == TY_EXCEPTION || bt == TY_BIGINT || bt == TY_REGEX)
                                      snprintf(truth, sizeof truth, "(_t%d != 0)", tv);
    else                              snprintf(truth, sizeof truth, "1");  /* concrete value: always truthy */
    if (is_map) {
      emit_indent(g_pre, din); buf_printf(g_pre, "sp_%sArray_push(_t%d, ", rk, tres);
      if (sp_streq(rk, "Poly") && !vpoly) { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, bt, tvb, &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p); }
      else buf_puts(g_pre, tvb);
      buf_puts(g_pre, ");\n");
    }
    else if (is_fmap) {
      /* filter_map: keep the block value only when truthy (nil/false dropped) */
      emit_indent(g_pre, din); buf_printf(g_pre, "if (%s) sp_%sArray_push(_t%d, ", truth, rk, tres);
      if (sp_streq(rk, "Poly") && !vpoly) { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, bt, tvb, &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p); }
      else buf_puts(g_pre, tvb);
      buf_puts(g_pre, ");\n");
    }
    else if (is_sel || is_rej) {
      emit_indent(g_pre, din); buf_printf(g_pre, "if (%s%s) sp_PolyArray_push(_t%d, ", is_rej ? "!" : "", truth, tres);
      Buf bx; memset(&bx, 0, sizeof bx); char pe[32]; snprintf(pe, sizeof pe, "_t%d", tpair); emit_boxed_text(c, pair_ty, pe, &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p);
      buf_puts(g_pre, ");\n");
    }
    else if (is_cnt) {
      emit_indent(g_pre, din); buf_printf(g_pre, "if (%s) _t%d++;\n", truth, tcnt);
    }
    else if (is_any) {
      emit_indent(g_pre, din); buf_printf(g_pre, "if (%s) { _t%d = 1; break; }\n", truth, tflag);
    }
    else if (is_none) {
      emit_indent(g_pre, din); buf_printf(g_pre, "if (%s) { _t%d = 0; break; }\n", truth, tflag);
    }
    else if (is_all) {
      emit_indent(g_pre, din); buf_printf(g_pre, "if (!%s) { _t%d = 0; break; }\n", truth, tflag);
    }
  }

  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");

  if (lv) lv->type = sv; if (li) li->type = si;

  if (is_map || is_fmap || collect_pair || is_toh) buf_printf(b, "_t%d", tres);
  else if (is_cnt) buf_printf(b, "_t%d", tcnt);
  else if (is_any || is_all || is_none) buf_printf(b, "_t%d", tflag);
  else buf_printf(b, "_t%d", ta);   /* each -> receiver */
  return 1;
}

/* sort_by { |x| key } as an expression: a stable bubble sort of a copy of
   the receiver, ordering by the block's computed (scalar) key. Returns 1 if
   handled. */
int emit_sortby_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int is_bang = sp_streq(name, "sort_by!");
  if (!sp_streq(name, "sort_by") && !is_bang) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  const char *p0_orig = block_param_name(c, block, 0);
  const char *p0 = p0_orig ? rename_local(p0_orig) : NULL;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  TyKind kt = comp_ntype(c, bb[bn - 1]);
  if (kt != TY_INT && kt != TY_FLOAT && kt != TY_STRING && kt != TY_POLY) return 0;  /* scalar or poly key */

  /* Schwartzian transform: compute each element's sort key exactly once (CRuby
     semantics -- the old bubble sort re-ran the block per comparison), stable-sort
     the indices by key, then gather the elements in sorted order. Non-mutating:
     the receiver is read by sorted index into a fresh result, never reordered. */
  int trv = ++g_tmp, tn = ++g_tmp, tkeys = ++g_tmp, tidx = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp, tg = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", trv, rb.p ? rb.p : ""); free(rb.p);
  /* root the receiver: the key loop allocates (boxing keys, growing the key/index
     arrays), so a freshly-built receiver held only here must survive a mid-build GC */
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", trv);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = sp_%sArray_length(_t%d);\n", tn, k, trv);
  /* boxed keys (rooted so they survive later iterations' allocations) + indices */
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tkeys, tkeys);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);\n", tidx, tidx);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) {\n", ti, ti, tn, ti);
  int np_sb = 0; while (block_param_name(c, block, np_sb)) np_sb++;
  if (np_sb >= 2 && rt == TY_POLY_ARRAY && !block_param_is_multi(c, block, 0)) {
    /* 2-param auto-splat: |name, age| over a poly array of sub-arrays. */
    int te = ++g_tmp;
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);\n", te, trv, ti);
    emit_autosplat_params(c, block, np_sb, te, g_indent + 1);
  }
  else if (p0) {
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trv, ti);
  }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
  int save = g_indent; g_indent += 1;
  Buf kb; memset(&kb, 0, sizeof kb); emit_expr(c, bb[bn - 1], &kb);
  g_indent = save;
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", tkeys);
  if (kt == TY_POLY) buf_puts(g_pre, kb.p ? kb.p : "sp_box_nil()");
  else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, kt, kb.p ? kb.p : "0", &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p); }
  buf_puts(g_pre, ");\n"); free(kb.p);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_IntArray_push(_t%d, _t%d);\n", tidx, ti);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_sort_idx_by_poly(_t%d->data + _t%d->start, _t%d->data, _t%d);\n", tidx, tidx, tkeys, tn);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);\n", tres, k, tres);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)\n", tg, tg, tn, tg);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, sp_IntArray_get(_t%d, _t%d)));\n", k, tres, k, trv, tidx, tg);
  if (is_bang) {
    /* sort_by!: write the gathered order back through the receiver pointer
       (aliases observe it) and yield the receiver -- CRuby returns self.
       The gather copy is needed anyway: writing in place while reading by
       sorted index would clobber the source. */
    int tw = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)\n", tw, tw, tn, tw);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_%sArray_set(_t%d, _t%d, sp_%sArray_get(_t%d, _t%d));\n", k, trv, tw, k, tres, tw);
    buf_printf(b, "_t%d", trv);
    return 1;
  }
  buf_printf(b, "_t%d", tres);
  return 1;
}

/* sort { |a, b| a <=> b } as an expression: stable bubble sort of a copy,
   ordered by the comparator block (which yields the <=> sign). Returns 1 if
   handled. */
int emit_sort_cmp_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int is_bang = sp_streq(name, "sort!");
  if (!sp_streq(name, "sort") && !is_bang) return 0;
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
  }
else {
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
void emit_block_param_assign(Compiler *c, int scope_id, const char *nm, int tidx, TyKind et, Buf *b) {
  Scope *sc = comp_scope_of(c, scope_id);
  LocalVar *lv = sc ? scope_local(sc, nm) : NULL;
  int box = lv && lv->type == TY_POLY && et != TY_POLY;
  if (box) {
    if (et == TY_INT)    buf_printf(b, "lv_%s = sp_box_int(_t%d);", nm, tidx);
    else if (et == TY_STRING) buf_printf(b, "lv_%s = sp_box_str(_t%d);", nm, tidx);
    else if (et == TY_FLOAT)  buf_printf(b, "lv_%s = sp_box_float(_t%d);", nm, tidx);
    else if (et == TY_BOOL)   buf_printf(b, "lv_%s = sp_box_bool(_t%d);", nm, tidx);
    else buf_printf(b, "lv_%s = _t%d;", nm, tidx);
  }
else {
    buf_printf(b, "lv_%s = _t%d;", nm, tidx);
  }
}

/* min / max / minmax { |a, b| a <=> b } as an expression: a single scan
   tracking the extreme(s) under the comparator block. min/max yield one
   element; minmax yields a fresh [min, max]. Returns 1 if handled. */
int emit_minmax_cmp_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int is_min = sp_streq(name, "min"), is_max = sp_streq(name, "max"), is_mm = sp_streq(name, "minmax");
  if (!is_min && !is_max && !is_mm) return 0;
  /* This lowers only the no-argument comparator form (one extreme element).
     `min(n)`/`max(n)` with a block takes the n extremes by the comparator and
     is not lowered; let it fall through to a clean reject rather than emitting
     a single-element scalar that silently ignores n. */
  int mm_args = nt_ref(nt, id, "arguments");
  if (mm_args >= 0) { int mm_argc = 0; nt_arr(nt, mm_args, "arguments", &mm_argc); if (mm_argc > 0) return 0; }
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
int emit_partition_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "partition")) return 0;
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

/* Emit a block body so its Ruby value lands in the already-declared C lvalue
   `dest`. Interior or tail `next <v>` assign `dest` (boxing when want_poly)
   then `continue`; a plain tail expression assigns `dest`. The caller declares
   `dest`, owns the surrounding loop, and consumes `dest` afterwards (push it for
   map, test its truthiness for select). Emits into g_pre at `indent`. This is
   the shared substrate that makes `next <value>` work inside a collecting
   block instead of dropping the value. */
static void emit_block_value_into(Compiler *c, int block, const char *dest,
                                  int want_poly, int indent) {
  const NodeTable *nt = c->nt;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  const char *sv_nx = g_ie_next_var; int sv_poly = g_ie_res_poly;
  int sv_lexc = g_loop_exc_base; g_loop_exc_base = g_exc_frame_depth;
  g_ie_next_var = dest; g_ie_res_poly = want_poly;
  g_c_loop_depth++;   /* the do{}while(0) wrapper makes `continue` valid */
  int sd = g_indent;
  /* Wrap the body in do{}while(0): an interior or tail `next <v>` assigns
     `dest` (via g_ie_next_var) then emits `continue`, which against while(0)
     exits this wrapper and falls through to the caller's collection rather
     than skipping it (a bare `continue` of the host loop would drop the
     value). Bodies without a next still run once -- the wrapper is free. */
  emit_indent(g_pre, indent); buf_puts(g_pre, "do {\n");
  int bi = indent + 1; g_indent = bi;
  /* fresh block-locals on every invocation (this path bypasses emit_stmts) */
  emit_block_locals_reset(c, block, g_pre, bi);
  for (int j = 0; j + 1 < bn; j++) emit_stmt(c, bb[j], g_pre, bi);
  if (bn > 0) {
    int tail = bb[bn - 1];
    const char *tty = nt_type(nt, tail);
    /* a control-flow tail (next/break/return/redo) is emitted as a statement;
       its own lowering writes `dest` where it carries a value. */
    int is_cf = tty && (sp_streq(tty, "NextNode") || sp_streq(tty, "BreakNode") ||
                        sp_streq(tty, "ReturnNode") || sp_streq(tty, "RedoNode"));
    if (is_cf) emit_stmt(c, tail, g_pre, bi);
    else {
      /* Emit the value into its own buffer so the tail's own preludes (e.g. a
         nested map's loop) flow to g_pre ahead of the assignment line rather
         than splicing into it. */
      TyKind tt = comp_ntype(c, tail);
      Buf vb; memset(&vb, 0, sizeof vb);
      if (want_poly && tt != TY_POLY) emit_boxed(c, tail, &vb);
      else emit_expr(c, tail, &vb);
      emit_indent(g_pre, bi);
      buf_printf(g_pre, "%s = %s;\n", dest, vb.p ? vb.p : "");
      free(vb.p);
    }
  }
  g_indent = sd;
  emit_indent(g_pre, indent); buf_puts(g_pre, "} while (0);\n");
  g_c_loop_depth--;
  g_ie_next_var = sv_nx; g_ie_res_poly = sv_poly; g_loop_exc_base = sv_lexc;
}

/* map/select/reject/filter as an expression: build a result array via a
   loop emitted into the statement prelude; the expression value is the
   temp array. Returns 1 if handled. */
int emit_collect_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  /* `arr.map(&blk)` inside `def m(&blk)`: a BlockArgumentNode that forwards
     the current method's block param maps over the caller's (already-inlined)
     literal block rather than treating it as an empty (nil-producing) block.
     Only a forward of the active block param is redirected — `&:sym` and
     `&proc_value` are left to their own handlers. */
  if (nt_type(nt, block) && sp_streq(nt_type(nt, block), "BlockArgumentNode")) {
    int fwd_expr = nt_ref(nt, block, "expression");
    int forwards_param = 0;
    if (fwd_expr < 0) forwards_param = 1;  /* anonymous `&` */
    else if (g_block_param_name && nt_type(nt, fwd_expr) &&
             sp_streq(nt_type(nt, fwd_expr), "LocalVariableReadNode")) {
      const char *en = nt_str(nt, fwd_expr, "name");
      forwards_param = en && sp_streq(en, g_block_param_name);
    }
    if (!forwards_param || g_block_id < 0) return 0;
    block = g_block_id;
  }
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name || recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  /* array.each_slice(n).map { |x, y, ...| } chain: unroll into a direct slice loop */
  if (ty_iter_shape(name) == TY_ITER_MAP &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "each_slice") &&
      nt_ref(nt, recv, "block") < 0) {
    int es_recv = nt_ref(nt, recv, "receiver");
    int es_args = nt_ref(nt, recv, "arguments");
    int es_argc = 0; const int *es_argv = es_args >= 0 ? nt_arr(nt, es_args, "arguments", &es_argc) : NULL;
    if (es_argc == 1 && es_recv >= 0) {
      TyKind arr_rt = comp_ntype(c, es_recv);
      if (ty_is_array(arr_rt)) {
        const char *k = (arr_rt == TY_POLY_ARRAY) ? "Poly" : array_kind(arr_rt);
        if (k) {
          TyKind restype_es = comp_ntype(c, id);
          int res_poly_es = (restype_es == TY_POLY_ARRAY);
          const char *rk_es = res_poly_es ? "Poly" : array_kind(restype_es);
          if (!rk_es) rk_es = "Int";
          int np_es = 0; while (block_param_name(c, block, np_es)) np_es++;
          int body_es = nt_ref(nt, block, "body");
          int bn_es = 0; const int *bb_es = body_es >= 0 ? nt_arr(nt, body_es, "body", &bn_es) : NULL;
          if (bn_es >= 1) {
            int ta_es = ++g_tmp, ts_es = ++g_tmp, tres_es = ++g_tmp, ti_es = ++g_tmp;
            Buf rb_es; memset(&rb_es, 0, sizeof rb_es); emit_expr(c, es_recv, &rb_es);
            emit_indent(g_pre, g_indent); emit_ctype(c, arr_rt, g_pre);
            buf_printf(g_pre, " _t%d = %s;\n", ta_es, rb_es.p ? rb_es.p : ""); free(rb_es.p);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "mrb_int _t%d = ", ts_es); emit_int_expr(c, es_argv[0], g_pre); buf_puts(g_pre, ";\n");
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk_es, tres_es, rk_es);
            emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres_es);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d += _t%d) {\n",
                       ti_es, ti_es, k, ta_es, ti_es, ts_es);
            if (block_param_is_multi(c, block, 0)) {
              /* |(a, b)| destructuring: assign each leaf from the slice */
              int lc_es = block_param_multi_count(c, block, 0);
              for (int li = 0; li < lc_es; li++) {
                const char *ln = block_param_multi_leaf(c, block, 0, li);
                if (!ln) continue;
                emit_indent(g_pre, g_indent + 1);
                buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d + %d);\n",
                           rename_local(ln), k, ta_es, ti_es, li);
              }
            }
            else if (np_es > 1) {
              for (int pj = 0; pj < np_es; pj++) {
                const char *pn = block_param_name(c, block, pj); if (!pn) break;
                emit_indent(g_pre, g_indent + 1);
                buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d + %d);\n",
                           rename_local(pn), k, ta_es, ti_es, pj);
              }
            }
            else {
              const char *p0_es = block_param_name(c, block, 0); if (p0_es) p0_es = rename_local(p0_es);
              if (p0_es) {
                emit_indent(g_pre, g_indent + 1);
                buf_printf(g_pre, "lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n",
                           p0_es, k, ta_es, ti_es, ts_es);
              }
            }
            /* collect the block's value (next-aware) into a result temp, push it */
            TyKind melem_es = res_poly_es ? TY_POLY : ty_array_elem(restype_es);
            int tv_es = ++g_tmp; char tvb_es[24]; snprintf(tvb_es, sizeof tvb_es, "_t%d", tv_es);
            emit_indent(g_pre, g_indent + 1);
            if (res_poly_es) buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil();\n", tv_es);
            else { emit_ctype(c, melem_es, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tv_es, default_value(melem_es)); }
            emit_block_value_into(c, block, tvb_es, res_poly_es, g_indent + 1);
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "sp_%sArray_push(_t%d, _t%d);\n", rk_es, tres_es, tv_es);
            emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
            buf_printf(b, "_t%d", tres_es);
            return 1;
          }
        }
      }
    }
  }
  /* array.each_cons(n).map { |pair| } or { |a,b| } or { |(a,b)| } chain */
  if (ty_iter_shape(name) == TY_ITER_MAP &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "each_cons") &&
      nt_ref(nt, recv, "block") < 0) {
    int ec_recv = nt_ref(nt, recv, "receiver");
    int ec_args = nt_ref(nt, recv, "arguments");
    int ec_argc = 0; const int *ec_argv = ec_args >= 0 ? nt_arr(nt, ec_args, "arguments", &ec_argc) : NULL;
    if (ec_argc == 1 && ec_recv >= 0) {
      TyKind arr_ec = comp_ntype(c, ec_recv);
      if (ty_is_array(arr_ec)) {
        const char *kec = (arr_ec == TY_POLY_ARRAY) ? "Poly" : array_kind(arr_ec);
        if (kec) {
          TyKind restype_ec = comp_ntype(c, id);
          int res_poly_ec = (restype_ec == TY_POLY_ARRAY);
          const char *rk_ec = res_poly_ec ? "Poly" : array_kind(restype_ec);
          if (!rk_ec) rk_ec = "Int";
          int body_ec = nt_ref(nt, block, "body");
          int bn_ec = 0; const int *bb_ec = body_ec >= 0 ? nt_arr(nt, body_ec, "body", &bn_ec) : NULL;
          if (bn_ec >= 1) {
            int ta_ec = ++g_tmp, tn_ec = ++g_tmp, tres_ec = ++g_tmp, ti_ec = ++g_tmp;
            Buf rb_ec; memset(&rb_ec, 0, sizeof rb_ec); emit_expr(c, ec_recv, &rb_ec);
            emit_indent(g_pre, g_indent); emit_ctype(c, arr_ec, g_pre);
            buf_printf(g_pre, " _t%d = %s;\n", ta_ec, rb_ec.p ? rb_ec.p : ""); free(rb_ec.p);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "mrb_int _t%d = ", tn_ec); emit_int_expr(c, ec_argv[0], g_pre); buf_puts(g_pre, ";\n");
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk_ec, tres_ec, rk_ec);
            emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres_ec);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d + _t%d - 1 < sp_%sArray_length(_t%d); _t%d++) {\n",
                       ti_ec, ti_ec, tn_ec, kec, ta_ec, ti_ec);
            int np_ec = 0; while (block_param_name(c, block, np_ec)) np_ec++;
            int is_multi_ec = block_param_is_multi(c, block, 0);
            if (is_multi_ec) {
              /* |(a, b)| destructuring: assign each leaf from window */
              int lc_ec = block_param_multi_count(c, block, 0);
              for (int li = 0; li < lc_ec; li++) {
                const char *ln = block_param_multi_leaf(c, block, 0, li);
                if (!ln) continue;
                emit_indent(g_pre, g_indent + 1);
                buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d + %d);\n",
                           rename_local(ln), kec, ta_ec, ti_ec, li);
              }
            }
            else if (np_ec > 1) {
              /* |a, b| flat multi-param: each element */
              for (int pj = 0; pj < np_ec; pj++) {
                const char *pn = block_param_name(c, block, pj); if (!pn) break;
                emit_indent(g_pre, g_indent + 1);
                buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d + %d);\n",
                           rename_local(pn), kec, ta_ec, ti_ec, pj);
              }
            }
            else if (np_ec == 1) {
              /* |pair| single param: slice of the window */
              const char *p0_ec = block_param_name(c, block, 0);
              const char *p0_ec_r = p0_ec ? rename_local(p0_ec) : NULL;
              if (p0_ec_r) {
                emit_indent(g_pre, g_indent + 1);
                buf_printf(g_pre, "lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n",
                           p0_ec_r, kec, ta_ec, ti_ec, tn_ec);
              }
            }
            for (int j = 0; j < bn_ec - 1; j++) emit_stmt(c, bb_ec[j], g_pre, g_indent + 1);
            int saveInd_ec = g_indent; g_indent = g_indent + 1;
            Buf vb_ec; memset(&vb_ec, 0, sizeof vb_ec);
            if (res_poly_ec) emit_boxed(c, bb_ec[bn_ec - 1], &vb_ec);
            else emit_expr(c, bb_ec[bn_ec - 1], &vb_ec);
            g_indent = saveInd_ec;
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "sp_%sArray_push(_t%d, %s);\n", rk_ec, tres_ec, vb_ec.p ? vb_ec.p : "");
            free(vb_ec.p);
            emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
            buf_printf(b, "_t%d", tres_ec);
            return 1;
          }
        }
      }
    }
  }

  /* array.each_cons(n).with_index(off).map { |pair, i| } or { |(a,b), i| } chain */
  if (ty_iter_shape(name) == TY_ITER_MAP &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "with_index") &&
      nt_ref(nt, recv, "block") < 0) {
    int wi_recv = nt_ref(nt, recv, "receiver");
    if (wi_recv >= 0 && nt_type(nt, wi_recv) && sp_streq(nt_type(nt, wi_recv), "CallNode") &&
        nt_str(nt, wi_recv, "name") && sp_streq(nt_str(nt, wi_recv, "name"), "each_cons") &&
        nt_ref(nt, wi_recv, "block") < 0) {
      int ec_recv2 = nt_ref(nt, wi_recv, "receiver");
      int ec_args2 = nt_ref(nt, wi_recv, "arguments");
      int ec_argc2 = 0; const int *ec_argv2 = ec_args2 >= 0 ? nt_arr(nt, ec_args2, "arguments", &ec_argc2) : NULL;
      int wi_args = nt_ref(nt, recv, "arguments");
      int wi_argc = 0; const int *wi_argv = wi_args >= 0 ? nt_arr(nt, wi_args, "arguments", &wi_argc) : NULL;
      if (ec_argc2 == 1 && ec_recv2 >= 0) {
        TyKind arr_wi = comp_ntype(c, ec_recv2);
        if (ty_is_array(arr_wi)) {
          const char *kwi = (arr_wi == TY_POLY_ARRAY) ? "Poly" : array_kind(arr_wi);
          if (kwi) {
            TyKind restype_wi = comp_ntype(c, id);
            int res_poly_wi = (restype_wi == TY_POLY_ARRAY);
            const char *rk_wi = res_poly_wi ? "Poly" : array_kind(restype_wi);
            if (!rk_wi) rk_wi = "Int";
            int body_wi = nt_ref(nt, block, "body");
            int bn_wi = 0; const int *bb_wi = body_wi >= 0 ? nt_arr(nt, body_wi, "body", &bn_wi) : NULL;
            if (bn_wi >= 1) {
              int ta_wi = ++g_tmp, tn_wi = ++g_tmp, tres_wi = ++g_tmp;
              int ti_wi = ++g_tmp, toff_wi = ++g_tmp, tidx_wi = ++g_tmp;
              Buf rb_wi; memset(&rb_wi, 0, sizeof rb_wi); emit_expr(c, ec_recv2, &rb_wi);
              emit_indent(g_pre, g_indent); emit_ctype(c, arr_wi, g_pre);
              buf_printf(g_pre, " _t%d = %s;\n", ta_wi, rb_wi.p ? rb_wi.p : ""); free(rb_wi.p);
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "mrb_int _t%d = ", tn_wi); emit_int_expr(c, ec_argv2[0], g_pre); buf_puts(g_pre, ";\n");
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "mrb_int _t%d = ", toff_wi);
              if (wi_argc > 0 && wi_argv) { emit_expr(c, wi_argv[0], g_pre); }
              else { buf_puts(g_pre, "0"); }
              buf_puts(g_pre, ";\n");
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk_wi, tres_wi, rk_wi);
              emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres_wi);
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "mrb_int _t%d = _t%d;\n", tidx_wi, toff_wi);
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d + _t%d - 1 < sp_%sArray_length(_t%d); _t%d++, _t%d++) {\n",
                         ti_wi, ti_wi, tn_wi, kwi, ta_wi, ti_wi, tidx_wi);
              /* assign second param (index) */
              const char *idx_p_wi = block_param_name(c, block, 1);
              if (idx_p_wi) {
                emit_indent(g_pre, g_indent + 1);
                buf_printf(g_pre, "lv_%s = _t%d;\n", rename_local(idx_p_wi), tidx_wi);
              }
              if (block_param_is_multi(c, block, 0)) {
                int lc_wi = block_param_multi_count(c, block, 0);
                Scope *bsc_wi = comp_scope_of(c, block);
                TyKind elem_ty_wi = ty_array_elem(arr_wi);
                for (int li = 0; li < lc_wi; li++) {
                  const char *ln = block_param_multi_leaf(c, block, 0, li);
                  if (!ln) continue;
                  const char *lnr = rename_local(ln);
                  LocalVar *lvw = bsc_wi ? scope_local(bsc_wi, ln) : NULL;
                  TyKind lv_ty_wi = lvw ? lvw->type : TY_UNKNOWN;
                  emit_indent(g_pre, g_indent + 1);
                  if (lv_ty_wi == TY_POLY && elem_ty_wi != TY_POLY && elem_ty_wi != TY_UNKNOWN) {
                    char esw[64];
                    snprintf(esw, sizeof esw, "sp_%sArray_get(_t%d, _t%d + %d)", kwi, ta_wi, ti_wi, li);
                    Buf bxw; memset(&bxw, 0, sizeof bxw); emit_boxed_text(c, elem_ty_wi, esw, &bxw);
                    buf_printf(g_pre, "lv_%s = %s;\n", lnr, bxw.p ? bxw.p : esw); free(bxw.p);
                  }
                  else {
                    buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d + %d);\n",
                               lnr, kwi, ta_wi, ti_wi, li);
                  }
                }
              }
              else {
                const char *pair_p_wi = block_param_name(c, block, 0);
                if (pair_p_wi) {
                  Scope *bsc_wi = comp_scope_of(c, block);
                  LocalVar *lvp_wi = bsc_wi ? scope_local(bsc_wi, pair_p_wi) : NULL;
                  char slice_wi[80];
                  snprintf(slice_wi, sizeof slice_wi, "sp_%sArray_slice(_t%d, _t%d, _t%d)",
                           kwi, ta_wi, ti_wi, tn_wi);
                  emit_indent(g_pre, g_indent + 1);
                  /* When the window element type didn't resolve at analyze time
                     the pair param is declared poly; box the typed slice so the
                     assignment types match (a desugared |(x,y), i| destructure
                     over a receiver like `n.times.map { ... }`). */
                  if (lvp_wi && lvp_wi->type == TY_POLY && !sp_streq(kwi, "Poly")) {
                    Buf bxs; memset(&bxs, 0, sizeof bxs);
                    emit_boxed_text(c, arr_wi, slice_wi, &bxs);
                    buf_printf(g_pre, "lv_%s = %s;\n", rename_local(pair_p_wi), bxs.p ? bxs.p : slice_wi);
                    free(bxs.p);
                  }
                  else {
                    buf_printf(g_pre, "lv_%s = %s;\n", rename_local(pair_p_wi), slice_wi);
                  }
                }
              }
              for (int j = 0; j < bn_wi - 1; j++) emit_stmt(c, bb_wi[j], g_pre, g_indent + 1);
              int saveInd_wi = g_indent; g_indent = g_indent + 1;
              Buf vb_wi; memset(&vb_wi, 0, sizeof vb_wi);
              if (res_poly_wi) emit_boxed(c, bb_wi[bn_wi - 1], &vb_wi);
              else emit_expr(c, bb_wi[bn_wi - 1], &vb_wi);
              g_indent = saveInd_wi;
              emit_indent(g_pre, g_indent + 1);
              buf_printf(g_pre, "sp_%sArray_push(_t%d, %s);\n", rk_wi, tres_wi, vb_wi.p ? vb_wi.p : "");
              free(vb_wi.p);
              emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
              buf_printf(b, "_t%d", tres_wi);
              return 1;
            }
          }
        }
      }
    }
  }

  if (ty_is_hash(rt)) {
    if (emit_hash_collect_expr(c, id, b)) return 1;
    if (emit_hash_reduce_search_expr(c, id, b)) return 1;
    if (emit_hash_sort_by_expr(c, id, b)) return 1;
    if (emit_hash_reduce_scalar_expr(c, id, b)) return 1;
    if (emit_hash_transform_expr(c, id, b)) return 1;
    if (emit_hash_group_by_expr(c, id, b)) return 1;
    return 0;
  }
  int range_recv = (rt == TY_RANGE);
  if (rt == TY_POLY) {
    /* poly-typed receiver (e.g. `arr = nil` default): iterate via
       sp_poly_arr_len / sp_poly_arr_get and build a typed result array */
    int is_map2 = ty_iter_shape(name) == TY_ITER_MAP;
    if (!is_map2) return 0;
    TyKind restype2 = comp_ntype(c, id);
    int res_poly2 = (restype2 == TY_POLY_ARRAY);
    const char *rk2 = res_poly2 ? "Poly" : array_kind(restype2);
    if (!rk2) return 0;
    const char *p0p = block_param_name(c, block, 0); if (p0p) p0p = rename_local(p0p);
    int body2 = nt_ref(nt, block, "body");
    int bn2 = 0;
    const int *bb2 = body2 >= 0 ? nt_arr(nt, body2, "body", &bn2) : NULL;
    if (bn2 < 1) return 0;
    int trecv2 = ++g_tmp, tn2 = ++g_tmp, tres2 = ++g_tmp, ti2 = ++g_tmp;
    Buf rb2; memset(&rb2, 0, sizeof rb2); emit_expr(c, recv, &rb2);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_RbVal _t%d = ", trecv2);
    buf_puts(g_pre, rb2.p ? rb2.p : "sp_box_nil()");
    buf_puts(g_pre, ";\n");
    free(rb2.p);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "mrb_int _t%d = sp_poly_arr_len(_t%d);\n", tn2, trecv2);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk2, tres2, rk2);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres2);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) {\n",
               ti2, ti2, tn2, ti2);
    /* pin block param to TY_POLY via shadow declaration */
    Scope *csc2 = p0p ? comp_scope_of(c, block) : NULL;
    LocalVar *clv2 = (csc2 && p0p) ? scope_local(csc2, p0p) : NULL;
    TyKind csaved2 = clv2 ? clv2->type : TY_UNKNOWN;
    if (clv2) clv2->type = TY_POLY;
    for (int j2 = 0; j2 < bn2; j2++) infer_type(c, bb2[j2]);
    emit_indent(g_pre, g_indent + 1);
    buf_puts(g_pre, "{\n");
    emit_indent(g_pre, g_indent + 2);
    buf_printf(g_pre, "sp_RbVal lv_%s = sp_poly_arr_get_hash(_t%d, _t%d);\n",
               p0p ? p0p : "_dummy", trecv2, ti2);
    for (int j2 = 0; j2 < bn2 - 1; j2++) emit_stmt(c, bb2[j2], g_pre, g_indent + 2);
    int saveIndent2 = g_indent; g_indent = g_indent + 2;
    Buf vb2; memset(&vb2, 0, sizeof vb2);
    if (res_poly2) emit_boxed(c, bb2[bn2 - 1], &vb2);
    else emit_expr(c, bb2[bn2 - 1], &vb2);
    g_indent = saveIndent2;
    emit_indent(g_pre, g_indent + 2);
    buf_printf(g_pre, "sp_%sArray_push(_t%d, %s);\n", rk2, tres2, vb2.p ? vb2.p : "");
    free(vb2.p);
    emit_indent(g_pre, g_indent + 1);
    buf_puts(g_pre, "}\n");
    emit_indent(g_pre, g_indent);
    buf_puts(g_pre, "}\n");
    if (clv2) clv2->type = csaved2;
    buf_printf(b, "_t%d", tres2);
    return 1;
  }
  if (!ty_is_array(rt) && !range_recv) return 0;
  const char *k = range_recv ? "Int" : (rt == TY_POLY_ARRAY ? "Poly" : array_kind(rt));
  if (!k) return 0;

  TyIterShape shp = ty_iter_shape(name);
  int is_map = shp == TY_ITER_MAP;
  int is_sel = shp == TY_ITER_SELECT;
  int is_rej = shp == TY_ITER_REJECT;
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
    buf_printf(&rb, "sp_range_to_ia(_t%d)", tr);
    rt = TY_INT_ARRAY;
  }
  else emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent);
  emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = ", trecv);
  buf_puts(g_pre, rb.p ? rb.p : "");
  buf_puts(g_pre, ";\n");
  free(rb.p);
  /* root the iteration source: the loop body may allocate and collect it */
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", trecv);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk, tres, rk);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, k, trecv, ti);

  TyKind et_elem = ty_array_elem(rt);
  /* 2-param auto-splat: |a, b| over a poly array whose elements are sub-arrays
     binds each param to a positional element of the sub-array, matching CRuby's
     proc auto-splat. The per-param types were pinned by infer_block_params, so
     bind directly (no shadow). select/reject still push the whole element. */
  int np_cl = 0; while (block_param_name(c, block, np_cl)) np_cl++;
  int autosplat = (np_cl >= 2 && rt == TY_POLY_ARRAY && !block_param_is_multi(c, block, 0));

  /* If the block param's scope type was widened (e.g. TY_POLY), pin it to
     the element type and use a C shadow declaration so body emission sees the
     right type. */
  Scope *csc = (p0 && !autosplat) ? comp_scope_of(c, block) : NULL;
  LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
  TyKind csaved0 = clv0 ? clv0->type : TY_UNKNOWN;
  int use_shadow = clv0 && clv0->type != et_elem && et_elem != TY_UNKNOWN;
  if (use_shadow) {
    clv0->type = et_elem;
    for (int j = 0; j < bn; j++) infer_type(c, bb[j]);
  }

  int bodyIndent = g_indent + 1;
  int innerIndent = use_shadow ? bodyIndent + 1 : bodyIndent;
  int te_split = -1;
  if (autosplat) {
    te_split = ++g_tmp;
    emit_indent(g_pre, bodyIndent);
    buf_printf(g_pre, "sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);\n", te_split, trecv, ti);
    emit_autosplat_params(c, block, np_cl, te_split, bodyIndent);
  }
  else if (use_shadow) {
    emit_indent(g_pre, bodyIndent); buf_puts(g_pre, "{\n");
    emit_indent(g_pre, innerIndent); emit_ctype(c, et_elem, g_pre);
    buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
  }
  else if (p0) {
    emit_indent(g_pre, bodyIndent);
    buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
  }
  if (is_map) {
    /* map: collect the block's value (next-aware) into a result temp, then
       push it -- so `next <v>` inside the block contributes <v> rather than
       dropping the element. */
    TyKind elem = ty_array_elem(restype);
    int tv = ++g_tmp;
    char tvbuf[24]; snprintf(tvbuf, sizeof tvbuf, "_t%d", tv);
    emit_indent(g_pre, innerIndent);
    if (res_poly) buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil();\n", tv);
    else { emit_ctype(c, elem, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tv, default_value(elem)); }
    emit_block_value_into(c, block, tvbuf, res_poly, innerIndent);
    emit_indent(g_pre, innerIndent);
    buf_printf(g_pre, "sp_%sArray_push(_t%d, _t%d);\n", rk, tres, tv);
  }
  else {
    /* select/reject: collect the block's value (next-aware) into a temp, then
       push the element when that value (negated for reject) is truthy -- so a
       `next <cond>` inside the block decides inclusion instead of being lost. */
    TyKind cty = comp_ntype(c, bb[bn - 1]);
    if (cty == TY_UNKNOWN) cty = TY_INT;
    int cond_poly = (cty == TY_POLY);
    int tv = ++g_tmp;
    char tvbuf[24]; snprintf(tvbuf, sizeof tvbuf, "_t%d", tv);
    emit_indent(g_pre, innerIndent);
    if (cond_poly) buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil();\n", tv);
    else { emit_ctype(c, cty, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tv, default_value(cty)); }
    emit_block_value_into(c, block, tvbuf, cond_poly, innerIndent);
    emit_indent(g_pre, innerIndent);
    /* Ruby truthiness: only nil and false are falsy. A nilable int/float reads
       falsy at its sentinel, but 0 / 0.0 are truthy -- so a block returning
       `x % 2` must keep the element. A bool keeps C-truthiness (0/1); a pointer
       value is falsy only when NULL (nil). Mirrors emit_cond. */
    if (cond_poly)         buf_printf(g_pre, "if (%ssp_poly_truthy(_t%d)) ", is_rej ? "!" : "", tv);
    else if (cty == TY_INT)   buf_printf(g_pre, "if (%s(_t%d != SP_INT_NIL)) ", is_rej ? "!" : "", tv);
    else if (cty == TY_FLOAT) buf_printf(g_pre, "if (%s(!sp_float_is_nil(_t%d))) ", is_rej ? "!" : "", tv);
    else                   buf_printf(g_pre, "if (%s(_t%d)) ", is_rej ? "!" : "", tv);
    if (autosplat)
      buf_printf(g_pre, "sp_%sArray_push(_t%d, _t%d);\n", rk, tres, te_split);
    else
      buf_printf(g_pre, "sp_%sArray_push(_t%d, lv_%s);\n", rk, tres, p0 ? p0 : "");
  }
  if (use_shadow) { emit_indent(g_pre, bodyIndent); buf_puts(g_pre, "}\n"); }
  if (use_shadow && clv0) clv0->type = csaved0;
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "}\n");

  buf_printf(b, "_t%d", tres);
  return 1;
}

/* arr.map.with_index(off) { |x, i| } / arr.each.with_index(off) { |x, i| } /
   arr.select.with_index(off) { |x, i| } (and collect/filter/reject): the
   receiver is a blockless enumerator over an array, and with_index binds the
   element plus a running index that starts at `off` (default 0). `map` collects
   the block value; `select`/`reject` collect the element conditionally; `each`
   runs the body for side effect and yields the receiver array. Arrays only --
   a range enumerator is a later slice. Returns 1 if handled. */
int emit_with_index_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "with_index")) return 0;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  int recv = nt_ref(nt, id, "receiver");  /* the blockless inner enumerator */
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) return 0;
  if (nt_ref(nt, recv, "block") >= 0) return 0;
  const char *inner = nt_str(nt, recv, "name");
  if (!inner) return 0;
  int is_each = sp_streq(inner, "each");
  TyIterShape shp = ty_iter_shape(inner);  /* map/select/reject; NONE for each */
  if (!is_each && shp == TY_ITER_NONE) return 0;
  int arr_recv = nt_ref(nt, recv, "receiver");
  if (arr_recv < 0) return 0;
  TyKind rt = comp_ntype(c, arr_recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;

  int is_map = shp == TY_ITER_MAP;
  int is_sel = shp == TY_ITER_SELECT;
  int is_rej = shp == TY_ITER_REJECT;
  int collecting = is_map || is_sel || is_rej;

  int wi_args = nt_ref(nt, id, "arguments");
  int wi_argc = 0; const int *wi_argv = wi_args >= 0 ? nt_arr(nt, wi_args, "arguments", &wi_argc) : NULL;

  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  const char *p1 = block_param_name(c, block, 1); if (p1) p1 = rename_local(p1);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1 && collecting) return 0;  /* map/select need a value */

  TyKind restype = comp_ntype(c, id);
  int res_poly = (restype == TY_POLY_ARRAY);
  const char *rk = collecting ? (res_poly ? "Poly" : array_kind(restype)) : NULL;
  if (collecting && !rk) rk = "Int";

  int trecv = ++g_tmp, ti = ++g_tmp, tidx = ++g_tmp;
  int tres = collecting ? ++g_tmp : 0;

  /* evaluate the source array once (its preludes land in g_pre first) */
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, arr_recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", trecv);
  /* running index, seeded with the offset */
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = ", tidx);
  if (wi_argc > 0 && wi_argv) emit_expr(c, wi_argv[0], g_pre); else buf_puts(g_pre, "0");
  buf_puts(g_pre, ";\n");
  if (collecting) {
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk, tres, rk);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres);
  }
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++, _t%d++) {\n",
             ti, ti, k, trecv, ti, tidx);

  int innerIndent = g_indent + 1;
  TyKind elem_t = ty_array_elem(rt);
  Scope *csc = comp_scope_of(c, block);
  LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
  LocalVar *clv1 = (csc && p1) ? scope_local(csc, p1) : NULL;
  if (p0) {
    emit_indent(g_pre, innerIndent);
    if (clv0 && clv0->type == TY_POLY && elem_t != TY_POLY && elem_t != TY_UNKNOWN) {
      char src[256]; snprintf(src, sizeof src, "sp_%sArray_get(_t%d, _t%d)", k, trecv, ti);
      buf_printf(g_pre, "lv_%s = ", p0); emit_boxed_text(c, elem_t, src, g_pre); buf_puts(g_pre, ";\n");
    }
    else buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
  }
  if (p1) {
    emit_indent(g_pre, innerIndent);
    if (clv1 && clv1->type == TY_POLY) buf_printf(g_pre, "lv_%s = sp_box_int(_t%d);\n", p1, tidx);
    else buf_printf(g_pre, "lv_%s = _t%d;\n", p1, tidx);
  }
  if (is_each) {
    for (int j = 0; j < bn; j++) emit_stmt(c, bb[j], g_pre, innerIndent);
  }
  else {
    for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, innerIndent);
    int saveInd = g_indent; g_indent = innerIndent;
    Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = saveInd;
    if (is_map) {
      TyKind body_ty = comp_ntype(c, bb[bn - 1]);
      emit_indent(g_pre, innerIndent); buf_printf(g_pre, "sp_%sArray_push(_t%d, ", rk, tres);
      if (res_poly && body_ty != TY_POLY) {
        Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, body_ty, vb.p ? vb.p : "", &bx);
        buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p);
      }
      else buf_puts(g_pre, vb.p ? vb.p : "");
      buf_puts(g_pre, ");\n");
    }
    else {  /* select / reject: push the element on the (negated) predicate */
      emit_indent(g_pre, innerIndent);
      buf_printf(g_pre, "if (%s(", is_rej ? "!" : "");
      buf_puts(g_pre, vb.p ? vb.p : ""); buf_puts(g_pre, ")) ");
      buf_printf(g_pre, "sp_%sArray_push(_t%d, lv_%s);\n", rk, tres, p0 ? p0 : "");
    }
    free(vb.p);
  }
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");

  if (collecting) buf_printf(b, "_t%d", tres);
  else buf_printf(b, "_t%d", trecv);  /* each.with_index yields the receiver */
  return 1;
}

/* all?/any?/none?/one? with a block: loop, count the truthy block results,
   and reduce to the predicate. Returns 1 if handled. */
int emit_predicate_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int is_all = sp_streq(name, "all?"), is_any = sp_streq(name, "any?"),
      is_none = sp_streq(name, "none?"), is_one = sp_streq(name, "one?");
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
    buf_printf(&rb, "sp_range_to_ia(_t%d)", tr);
    rt = TY_INT_ARRAY;
  }
  else emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent);
  emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", trecv);
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
int emit_grep_pred(Compiler *c, int pat, const char *ev, TyKind et, Buf *b) {
  const NodeTable *nt = c->nt;
  int re = re_lit_index(c, pat);
  if (re >= 0) { buf_printf(b, "sp_re_match_p(sp_re_pat_%d, %s)", re, ev); return 1; }
  const char *pty = nt_type(nt, pat);
  /* A value-literal pattern uses `pattern === elem` == value equality. The C
     comparison depends on the element representation: a raw scalar for a typed
     array, a boxed sp_RbVal for a poly array. */
  if (pty && sp_streq(pty, "IntegerNode")) {
    long long v = (long long)nt_int(nt, pat, "value", 0);
    if (et == TY_INT) buf_printf(b, "((%s) == %lldLL)", ev, v);
    else if (et == TY_POLY) buf_printf(b, "sp_poly_eq(%s, sp_box_int(%lldLL))", ev, v);
    else return 0;
    return 1;
  }
  if (pty && sp_streq(pty, "RangeNode")) {
    int tr = ++g_tmp;
    buf_printf(b, "({ sp_Range _t%d = ", tr); emit_expr(c, pat, b);
    /* sp_range_include takes mrb_int; coerce a poly scrutinee with sp_poly_to_i. */
    if (et == TY_POLY) buf_printf(b, "; sp_range_include(&_t%d, sp_poly_to_i(%s)); })", tr, ev);
    else buf_printf(b, "; sp_range_include(&_t%d, %s); })", tr, ev);
    return 1;
  }
  if (pty && sp_streq(pty, "ConstantReadNode")) {
    const char *cn = nt_str(nt, pat, "name");
    if (!cn) return 0;
    if (sp_streq(cn, "Integer") || sp_streq(cn, "Fixnum")) buf_printf(b, "(%s).tag == SP_TAG_INT", ev);
    else if (sp_streq(cn, "String"))   buf_printf(b, "(%s).tag == SP_TAG_STR", ev);
    else if (sp_streq(cn, "Float"))    buf_printf(b, "(%s).tag == SP_TAG_FLT", ev);
    else if (sp_streq(cn, "Symbol"))   buf_printf(b, "(%s).tag == SP_TAG_SYM", ev);
    else if (sp_streq(cn, "Numeric"))  buf_printf(b, "((%s).tag == SP_TAG_INT || (%s).tag == SP_TAG_FLT)", ev, ev);
    else return 0;
    return 1;
  }
  return 0;
}

/* grep(pattern) / grep_v(pattern) without a block: collect elements for which
   `pattern === e` holds (or fails, for grep_v). Returns 1 if handled. */
int emit_grep_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || (!sp_streq(name, "grep") && !sp_streq(name, "grep_v"))) return 0;
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
  TyKind et = ty_array_elem(rt);

  /* probe predicate support before emitting anything */
  Buf probe; memset(&probe, 0, sizeof probe);
  if (!emit_grep_pred(c, pat, "_e", et, &probe)) { free(probe.p); return 0; }
  free(probe.p);

  int neg = sp_streq(name, "grep_v");
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
  emit_grep_pred(c, pat, ev, et, g_pre);
  buf_printf(g_pre, ")) sp_%sArray_push(_t%d, _t%d);\n", k, tres, te);
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "}\n");

  buf_printf(b, "_t%d", tres);
  return 1;
}

/* Emit the value for callee param `idx`: the provided arg node if any,
   else the param's default (a nil default becomes the type's default). */
void emit_arg_or_default(Compiler *c, Scope *m, int idx, int provided, Buf *out) {
  LocalVar *p = scope_local(m, m->pnames[idx]);
  TyKind pt = p ? p->type : TY_INT;
  /* An unused/unresolved param is declared poly (sp_RbVal) in the method
     signature (codegen.c maps TY_UNKNOWN params to TY_POLY); box the argument
     to match, so a virtually-dispatched call into such a slot passes a valid
     sp_RbVal rather than a raw value (or `void` temp). */
  if (p && pt == TY_UNKNOWN) pt = TY_POLY;
  if (provided >= 0) {
    if (pt == TY_POLY) emit_boxed(c, provided, out);   /* box into a poly param */
    else {
      TyKind at = comp_ntype(c, provided);
      /* Bare call inside a class/module body: analyze may not have resolved the
         type because g_cbody_class_id is not set during fixpoint. Look it up now. */
      if (at == TY_UNKNOWN && g_class_body_id >= 0) {
        const char *ptn = nt_type(c->nt, provided);
        if (ptn && sp_streq(ptn, "CallNode") && nt_ref(c->nt, provided, "receiver") < 0) {
          const char *bn = nt_str(c->nt, provided, "name");
          int bsmi = bn ? comp_cmethod_in_chain(c, g_class_body_id, bn, NULL) : -1;
          if (bsmi >= 0) at = (TyKind)c->scopes[bsmi].ret;
        }
      }
      /* empty array literal `[]` defaults to IntArray in emit_expr; if the
         parameter expects a different array type, emit the right constructor */
      int nen = 0;
      const char *pty_node = nt_type(c->nt, provided);
      int is_empty_arr = pty_node && sp_streq(pty_node, "ArrayNode") &&
                         (nt_arr(c->nt, provided, "elements", &nen), nen == 0);
      if (is_empty_arr && ty_is_array(pt) && pt != TY_INT_ARRAY) {
        if (pt == TY_POLY_ARRAY) buf_puts(out, "sp_PolyArray_new()");
        else { const char *k = array_kind(pt); if (k) buf_printf(out, "sp_%sArray_new()", k); else emit_expr(c, provided, out); }
      }
      /* empty hash literal `{}` with unknown type: emit the param's hash constructor.
         Must be checked before the poly-unbox path below, as at==TY_UNKNOWN for {}.  */
      else {
        int phn = 0;
        int is_empty_hash = pty_node && (sp_streq(pty_node, "HashNode") || sp_streq(pty_node, "KeywordHashNode")) &&
                             (nt_arr(c->nt, provided, "elements", &phn), phn == 0);
        if (is_empty_hash && at == TY_UNKNOWN && ty_is_hash(pt)) {
          const char *hn = ty_hash_cname(pt);
          if (hn) { buf_printf(out, "sp_%sHash_new()", hn); return; }
        }
        /* A concrete str-keyed hash arg (StrStrHash / StrIntHash) into a
           poly-valued hash param (StrPolyHash): the two structs store values
           differently (const char* / mrb_int vs sp_RbVal), so a raw pointer
           pass reinterprets the layout and corrupts every read. Rebuild via the
           value-boxing converter, mirroring the local-assignment coercion in
           codegen_stmt.c. */
        if (pt == TY_STR_POLY_HASH && (at == TY_STR_STR_HASH || at == TY_STR_INT_HASH)) {
          buf_printf(out, "sp_StrPolyHash_from_%s(", at == TY_STR_STR_HASH ? "str_str_hash" : "str_int_hash");
          emit_expr(c, provided, out); buf_puts(out, ")");
          return;
        }
        /* When the param is a typed hash pointer but the caller passes a poly
           or nil value (e.g. an uninit ivar), extract .v.p from the RbVal.
           sp_box_nil() stores v.i=0 so .v.p is NULL, which hash getters handle
           safely via their NULL guards. */
        if (ty_is_hash(pt) && (at == TY_POLY || at == TY_NIL || at == TY_UNKNOWN)) {
          const char *hn = ty_hash_cname(pt);
          if (hn) {
            int ht = ++g_tmp;
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_RbVal _t%d = ", ht);
            Buf ab2; memset(&ab2, 0, sizeof ab2);
            emit_expr(c, provided, &ab2);
            buf_puts(g_pre, ab2.p ? ab2.p : "sp_box_nil()"); free(ab2.p);
            buf_puts(g_pre, ";\n");
            buf_printf(out, "(sp_%sHash *)_t%d.v.p", hn, ht);
            return;
          }
        }
        /* poly arg into a concrete param (holds the right type at runtime):
           coerce, else the generated C assigns sp_RbVal to a const char* /
           mrb_int / mrb_float / sp_<Class>* slot. */
        const char *ptn = c_type_name(pt);
        if (at == TY_POLY && pt == TY_STRING) { buf_puts(out, "sp_poly_to_s("); emit_expr(c, provided, out); buf_puts(out, ")"); }
        else if (at == TY_POLY && pt == TY_FLOAT) { buf_puts(out, "sp_poly_to_f("); emit_expr(c, provided, out); buf_puts(out, ")"); }
        else if (at == TY_POLY && pt == TY_SYMBOL) { buf_puts(out, "(sp_sym)sp_poly_to_i("); emit_expr(c, provided, out); buf_puts(out, ")"); }
        else if (at == TY_POLY && (pt == TY_INT || pt == TY_BOOL)) { buf_puts(out, "sp_poly_to_i("); emit_expr(c, provided, out); buf_puts(out, ")"); }
        /* poly arg into an object or other pointer-backed param (array, proc,
           ...): unbox the pointer via emit_unbox_text (a nil box has v.p ==
           NULL, the pointer-nil representation). The callee's RBS asserts the
           type, mirroring the seed-trusting coercion on the return side. (Typed-
           value hashes are handled by the ty_is_hash block above; by-value types
           have no .v.p form and fall through to a plain emit.) */
        else if (at == TY_POLY && (ty_is_object(pt) || (ptn && ptn[0] && ptn[strlen(ptn) - 1] == '*'))) {
          Buf ub; memset(&ub, 0, sizeof ub);
          emit_expr(c, provided, &ub);
          emit_unbox_text(c, pt, ub.p ? ub.p : "", out);
          free(ub.p);
        }
        else emit_expr(c, provided, out);
      }
    }
    return;
  }
  int dv = m->pdefault[idx];
  const char *dty = dv >= 0 ? nt_type(c->nt, dv) : NULL;
  if (dv < 0) {
    /* A missing required arg pads the slot with a zero-ish compat value so
       codegen completes (a compile-time warning already flagged the call).
       A poly-widened slot must mirror the scalar `0` an int slot emits, not
       sp_box_nil() -- otherwise the padded value renders as blank. */
    if (pt == TY_POLY) buf_puts(out, "sp_box_int(0)");
    else buf_puts(out, pt == TY_RANGE ? "(sp_Range){0}" : default_value(pt));
  }
else if (dty && sp_streq(dty, "NilNode")) {
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
    int is_empty_arr_dv = dty && sp_streq(dty, "ArrayNode") &&
                          (nt_arr(c->nt, dv, "elements", &den), den == 0);
    if (is_empty_arr_dv && ty_is_array(pt) && pt != TY_INT_ARRAY) {
      if (pt == TY_POLY_ARRAY) buf_puts(out, "sp_PolyArray_new()");
      else { const char *k = array_kind(pt); if (k) buf_printf(out, "sp_%sArray_new()", k); else emit_expr(c, dv, out); }
    }
    /* Default empty `{}` literal: emit the correct hash constructor for the
       parameter type, avoiding the "unsupported hash literal" fallback. */
    else {
      int dhn = 0;
      int is_empty_hash_dv = dty && (sp_streq(dty, "HashNode") || sp_streq(dty, "KeywordHashNode")) &&
                              (nt_arr(c->nt, dv, "elements", &dhn), dhn == 0);
      if (is_empty_hash_dv && ty_is_hash(pt)) {
        const char *hn = ty_hash_cname(pt);
        if (hn) buf_printf(out, "sp_%sHash_new()", hn);
        else emit_expr(c, dv, out);
      }
      else emit_expr(c, dv, out);
    }
  }
}

/* Emit a comma-separated argument list filling defaults for omitted
   optional params. `lead` is prepended before the first arg. */
/* Find the value node for keyword param named `kname` in a KeywordHashNode `kwh`. */
int kwh_lookup(const NodeTable *nt, int kwh, const char *kname) {
  if (kwh < 0 || !kname) return -1;
  int en = 0;
  const int *elems = nt_arr(nt, kwh, "elements", &en);
  for (int e = 0; e < en; e++) {
    int key = nt_ref(nt, elems[e], "key");
    if (key < 0) continue;
    const char *kty = nt_type(nt, key);
    const char *kn = (kty && sp_streq(kty, "SymbolNode")) ? nt_str(nt, key, "value") : NULL;
    if (kn && sp_streq(kn, kname)) return nt_ref(nt, elems[e], "value");
  }
  return -1;
}

/* Emit a PolyArray expression that collects call args[from..pos_argc-1].
   SplatNode arguments are expanded element-by-element into the array. */
/* An anonymous `*` at a forwarding call site (`f(a, *)`) is a SplatNode with no
   expression; it forwards the enclosing method's anonymous rest local. Returns
   that local's poly-array C expression into `buf` and 1, or 0 if `splat` is not
   an anonymous forward. */
static int emit_anon_rest_ref(Compiler *c, int splat, Buf *buf) {
  if (nt_ref(c->nt, splat, "expression") >= 0) return 0;
  Scope *sc = comp_scope_of(c, splat);
  if (!sc || sc->rest_idx < 0 || sc->rest_idx >= sc->nparams) return 0;
  buf_printf(buf, "lv_%s", sc->pnames[sc->rest_idx]);
  return 1;
}

/* An anonymous `**` at a forwarding call site (`f(**)`) is an AssocSplatNode with
   no value; it forwards the enclosing method's anonymous kwrest local. Returns
   that local's name, or NULL if `node` is not inside a scope with an anon kwrest. */
static const char *anon_kwrest_name(Compiler *c, int node) {
  Scope *sc = comp_scope_of(c, node);
  if (!sc || sc->kwrest_idx < 0 || sc->kwrest_idx >= sc->nparams || !sc->pnames) return NULL;
  const char *nm = sc->pnames[sc->kwrest_idx];
  return (nm && sp_streq(nm, "__anon_kwrest")) ? nm : NULL;
}

void emit_rest_pack(Compiler *c, int from, int pos_argc, const int *argv, Buf *b) {
  const NodeTable *nt = c->nt;
  /* Optimize: single pure-splat → direct conversion */
  if (pos_argc == from + 1) {
    const char *aty = argv ? nt_type(nt, argv[from]) : NULL;
    if (aty && sp_streq(aty, "SplatNode")) {
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
    if (aty && sp_streq(aty, "SplatNode")) {
      int inner = nt_ref(nt, argv[i], "expression");
      Buf arr; memset(&arr, 0, sizeof arr);
      TyKind at;
      if (inner >= 0) { at = comp_ntype(c, inner); emit_expr(c, inner, &arr); }
      else if (emit_anon_rest_ref(c, argv[i], &arr)) at = TY_POLY_ARRAY;  /* anonymous `*` */
      else at = TY_UNKNOWN;
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
    }
else {
      Buf el; memset(&el, 0, sizeof el);
      emit_boxed(c, argv[i], &el);
      buf_printf(b, " sp_PolyArray_push(_t%d, %s);", t, el.p ? el.p : "sp_box_nil()");
      free(el.p);
    }
  }
  buf_printf(b, " _t%d; })", t);
}

/* Emit the element at index `elem_idx` from a typed array temp `tmp`. */
void emit_array_elem_at(TyKind at, int tmp, int elem_idx, Buf *b) {
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
void emit_rest_from_splat_and_argv(int tmp, TyKind at, int from_idx,
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
    if (jty && sp_streq(jty, "SplatNode")) {
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
    }
else {
      Buf el; memset(&el, 0, sizeof el); emit_boxed(c, argv[j], &el);
      buf_printf(b, " sp_PolyArray_push(_t%d, %s);", t, el.p ? el.p : "sp_box_nil()");
      free(el.p);
    }
  }
  buf_printf(b, " _t%d; })", t);
}

/* Like emit_arg_or_default, but hoists a pointer-backed / poly argument into a
   g_pre temp and roots it before the call. A fresh allocation passed straight
   into a callee that allocates before it roots the parameter -- the canonical
   case being the `{}` for `def initialize(attrs = {})` into sp_<C>_new, which
   SP_POOL_NEWs (can GC) before sp_<C>_initialize roots lv_attrs (#1445) -- would
   otherwise be collected mid-call (use-after-free / SIGSEGV@0x0). This is the
   #1052-deferred "fresh temp passed straight into a call" shape. emit_args_filled
   (the .new / super arg path) emitted args inline; normal method calls already
   hoist+root via emit_dispatch. Rooting in the caller's frame keeps the value
   alive across the whole call. A scalar (int/float/...) arg needs no root and is
   emitted inline. */
static void emit_arg_rooted(Compiler *c, Scope *m, int idx, int provided, Buf *out) {
  LocalVar *p = scope_local(m, m->pnames[idx]);
  TyKind pt = p ? p->type : TY_UNKNOWN;
  int poly = (pt == TY_POLY);
  if (!poly && !needs_root(pt)) { emit_arg_or_default(c, m, idx, provided, out); return; }
  /* A bare read (local/ivar/const/self/nil/string literal) is already reachable
     from a root where it lives, so it needs no hoisted temp. Hoisting it into
     g_pre is also WRONG when the call sits in a sequence-expression that assigns
     the read variable before the call: the g_pre line is flushed at the
     statement boundary, capturing the value ABOVE that in-sequence assignment
     (`a = {...}; foo(a)` as an operand passed a stale `a`). Emit inline, matching
     the g_argov skip in emit_args_filled. A param default like `{}` (provided<0)
     is a fresh allocation and still hoisted -- the #1445 root case. */
  if (provided >= 0) {
    const char *aty = nt_type(c->nt, provided);
    if (aty && (sp_streq(aty, "LocalVariableReadNode") ||
                sp_streq(aty, "InstanceVariableReadNode") ||
                sp_streq(aty, "ConstantReadNode") ||
                sp_streq(aty, "SelfNode") || sp_streq(aty, "NilNode") ||
                sp_streq(aty, "StringNode"))) {
      emit_arg_or_default(c, m, idx, provided, out);
      return;
    }
  }
  Buf ab; memset(&ab, 0, sizeof ab);
  emit_arg_or_default(c, m, idx, provided, &ab);
  int t = ++g_tmp;
  emit_indent(g_pre, g_indent);
  emit_ctype(c, pt, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", t, ab.p ? ab.p : default_value(pt));
  emit_indent(g_pre, g_indent);
  if (poly) buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", t);
  else buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
  buf_printf(out, "_t%d", t);
  free(ab.p);
}

/* True if `name` is one of the callee's explicit keyword parameters (`k:` /
   `k: default`). Only keyword params consume a key from a forwarded `**hash`;
   positional params with the same name do not. Read from the callee's AST
   `keywords` array rather than pnames[], which mixes positional and keyword. */
static int callee_has_kwarg(Compiler *c, Scope *m, const char *name) {
  if (!m || !name || m->def_node < 0) return 0;
  int pn = nt_ref(c->nt, m->def_node, "parameters");
  if (pn < 0) return 0;
  int kn = 0; const int *kws = nt_arr(c->nt, pn, "keywords", &kn);
  for (int i = 0; i < kn; i++) {
    const char *kpn = nt_str(c->nt, kws[i], "name");
    if (kpn && sp_streq(kpn, name)) return 1;
  }
  return 0;
}

void emit_args_filled(Compiler *c, int callee_idx, int argsNode, const char *lead, Buf *out) {
  Scope *m = &c->scopes[callee_idx];
  const NodeTable *nt = c->nt;
  int argc = 0;
  const int *argv = argsNode >= 0 ? nt_arr(nt, argsNode, "arguments", &argc) : NULL;
  /* `bar(...)`: the ArgumentsNode holds a single ForwardingArgumentsNode.
     Forward the enclosing `def foo(...)` method's synthesized __fwd_* params
     directly to the callee, positionally (#1288). The compiler already knows
     foo's args; no rest array / splat is materialized. */
  if (argc == 1 && argv && nt_type(nt, argv[0]) &&
      sp_streq(nt_type(nt, argv[0]), "ForwardingArgumentsNode")) {
    Scope *encl = comp_scope_of(c, argv[0]);
    for (int i = 0; i < m->nparams; i++) {
      buf_puts(out, i == 0 ? lead : ", ");
      if (encl && i < encl->nparams) {
        LocalVar *ep = scope_local(encl, encl->pnames[i]);
        LocalVar *mp = scope_local(m, m->pnames[i]);
        TyKind et = ep ? ep->type : TY_POLY;
        TyKind mt = mp ? mp->type : TY_POLY;
        char txt[80]; snprintf(txt, sizeof txt, "lv_%s", encl->pnames[i]);
        if (mt == TY_POLY && et != TY_POLY) emit_boxed_text(c, et, txt, out);
        else buf_puts(out, txt);
      }
      else emit_arg_rooted(c, m, i, -1, out);
    }
    return;
  }
  /* Separate trailing keyword-hash arg (if any) from positional args. */
  int kwh = -1;
  int pos_argc = argc;
  if (argc > 0 && nt_type(nt, argv[argc - 1]) &&
      sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) {
    kwh = argv[argc - 1];
    pos_argc = argc - 1;
  }
  /* Detect double-splat (**hash) inside kwh: AssocSplatNode wrapping a hash expr.
     Pre-evaluate the hash to a temp so we can do per-param lookups. */
  int ds_hash_tmp = -1;  TyKind ds_hash_type = TY_UNKNOWN;
  if (kwh >= 0) {
    int en2 = 0; const int *elems2 = nt_arr(nt, kwh, "elements", &en2);
    for (int e = 0; e < en2; e++) {
      const char *ety2 = nt_type(nt, elems2[e]);
      if (ety2 && sp_streq(ety2, "AssocSplatNode")) {
        int inner2 = nt_ref(nt, elems2[e], "value");
        if (inner2 >= 0) {
          ds_hash_type = comp_ntype(c, inner2);
          if (ty_is_hash(ds_hash_type)) {
            ds_hash_tmp = ++g_tmp;
            /* Render the source into a side buffer first: a hash LITERAL
               (`**{ ... }`) drains its own construction into g_pre, which must
               land before -- not inside -- this temp's declaration line. A bare
               variable emits with no prelude, so this is a no-op there. */
            Buf hb; memset(&hb, 0, sizeof hb);
            emit_expr(c, inner2, &hb);
            emit_indent(g_pre, g_indent);
            emit_ctype(c, ds_hash_type, g_pre);
            buf_printf(g_pre, " _t%d = %s;\n", ds_hash_tmp, hb.p ? hb.p : "");
            free(hb.p);
          }
        } else {
          /* Anonymous `**`: materialize the enclosing __anon_kwrest (SymPolyHash)
             so the per-param extraction and kwrest collection below can read it. */
          const char *akw = anon_kwrest_name(c, elems2[e]);
          if (akw) {
            ds_hash_type = TY_SYM_POLY_HASH;
            ds_hash_tmp = ++g_tmp;
            emit_indent(g_pre, g_indent);
            emit_ctype(c, ds_hash_type, g_pre);
            buf_printf(g_pre, " _t%d = lv_%s;\n", ds_hash_tmp, akw);
          }
        }
        break;
      }
    }
  }

  /* Find the first SplatNode in positional args. If it comes before rest_idx
     (or before nparams for rest-less methods), pre-evaluate it to a temp so
     we can index into it per fixed param. */
  int splat_idx = -1;  /* index into argv[] of the SplatNode */
  int splat_tmp = -1;  TyKind splat_at = TY_UNKNOWN;
  for (int k = 0; k < pos_argc; k++) {
    if (argv && nt_type(nt, argv[k]) && sp_streq(nt_type(nt, argv[k]), "SplatNode")) {
      int need_expand = (m->rest_idx >= 0 && k < m->rest_idx) ||
                        (m->rest_idx < 0 && k < m->nparams);
      if (need_expand) {
        splat_idx = k;
        int inner = nt_ref(nt, argv[k], "expression");
        splat_at = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
        if (ty_is_array(splat_at) || splat_at == TY_POLY_ARRAY) {
          splat_tmp = ++g_tmp;
          /* Evaluate the splat operand into a side buffer: a literal array or a
             call result emits its own setup (a fresh `_tN = ..._new()` decl)
             into g_pre, which must land before this temp's declaration line --
             a bare local read has no setup, which is why those already worked. */
          Buf sb; memset(&sb, 0, sizeof sb);
          emit_expr(c, inner, &sb);
          emit_indent(g_pre, g_indent);
          emit_ctype(c, splat_at, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", splat_tmp, sb.p ? sb.p : "");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", splat_tmp);
          free(sb.p);
          /* Arity check: splatting into a fixed-arity (no-rest) method must
             supply a valid element count, else CRuby raises ArgumentError.
             Only emit when the splat is the last positional group, so the
             total given count is `splat_idx + array length`. */
          if (m->rest_idx < 0 && k == pos_argc - 1) {
            char expbuf[48];
            if (m->nrequired == m->nparams)
              snprintf(expbuf, sizeof expbuf, "expected %d", m->nparams);
            else
              snprintf(expbuf, sizeof expbuf, "expected %d..%d", m->nrequired, m->nparams);
            int gv = ++g_tmp;
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "mrb_int _t%d = %d + (_t%d ? _t%d->len : 0);\n", gv, splat_idx, splat_tmp, splat_tmp);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre,
                       "if (_t%d < %d || _t%d > %d) sp_raise_cls(\"ArgumentError\", sp_sprintf(\"wrong number of arguments (given %%lld, %s)\", (long long)_t%d));\n",
                       gv, m->nrequired, gv, m->nparams, expbuf, gv);
          }
        }
      }
      break;
    }
  }
  /* GC hazard: a freshly-allocated heap argument sits in an unrooted C
     temporary while the rest of the call is evaluated AND while the callee
     runs. Either a later argument or the callee's own body can trigger a
     collection that sweeps it — and a constructor (sp_X_new) always
     allocates, so even `Ray.new(Vec.new(...), eye)` is exposed. Pre-evaluate
     each allocating heap arg into a rooted temp, left to right; emit_expr
     substitutes the temp via g_argov. Plain positional calls only — the
     splat/kwarg machinery has its own evaluation order. */
  int argov_saved = g_n_argov;
  if (splat_idx < 0 && kwh < 0 && argv) {
    for (int k = 0; k < pos_argc && k < m->nparams; k++) {
      if (g_n_argov >= MAX_ARG_OVERRIDE) break;
      TyKind at = comp_ntype(c, argv[k]);
      if (at != TY_POLY && !needs_root(at)) continue;
      const char *aty = nt_type(nt, argv[k]);
      /* a bare read is already rooted where it lives */
      if (aty && (sp_streq(aty, "LocalVariableReadNode") ||
                  sp_streq(aty, "InstanceVariableReadNode") ||
                  sp_streq(aty, "ConstantReadNode") ||
                  sp_streq(aty, "SelfNode") || sp_streq(aty, "NilNode") ||
                  sp_streq(aty, "StringNode"))) continue;
      /* only a fresh allocation needs protecting; a non-allocating heap
         expression (e.g. a ternary over two already-live reads) does not. */
      if (!subtree_may_allocate(nt, argv[k])) continue;
      int ht = ++g_tmp;
      /* Evaluate into a side buffer first: the expression may push its own
         setup into g_pre, which must be fully flushed before this temp's
         declaration line is written. */
      Buf hb; memset(&hb, 0, sizeof hb);
      emit_expr(c, argv[k], &hb);
      emit_indent(g_pre, g_indent);
      if (at == TY_POLY) {
        buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n",
                   ht, hb.p ? hb.p : "sp_box_nil()", ht);
      }
else {
        emit_ctype(c, at, g_pre);
        buf_printf(g_pre, " _t%d = %s; SP_GC_ROOT(_t%d);\n",
                   ht, hb.p ? hb.p : "0", ht);
      }
      free(hb.p);
      g_argov_node[g_n_argov] = argv[k];
      snprintf(g_argov_text[g_n_argov], sizeof g_argov_text[0], "_t%d", ht);
      g_n_argov++;
    }
  }
  for (int i = 0; i < m->nparams; i++) {
    buf_puts(out, i == 0 ? lead : ", ");
    if (m->rest_idx >= 0 && i == m->rest_idx) {
      /* rest collects middle args; stop before post-splat params */
      int rest_end = pos_argc - m->npost_rest;
      if (splat_tmp >= 0) {
        emit_rest_from_splat_and_argv(splat_tmp, splat_at, i - splat_idx,
                                      c, splat_idx + 1, rest_end, argv, out);
      }
else {
        emit_rest_pack(c, i, rest_end, argv, out);
      }
    }
else if (m->rest_idx >= 0 && m->npost_rest > 0 && i > m->rest_idx) {
      /* post-splat required param: take from the end of the call args */
      int post_j = i - m->rest_idx - 1;  /* 0-based index in posts */
      int argv_idx = pos_argc - m->npost_rest + post_j;
      if (argv && argv_idx >= 0 && argv_idx < pos_argc)
        emit_arg_rooted(c, m, i, argv[argv_idx], out);
      else
        emit_arg_rooted(c, m, i, -1, out);
    }
else if (splat_tmp >= 0 && i >= splat_idx) {
      /* this param comes from the splatted array at offset (i - splat_idx) */
      int off = i - splat_idx;
      LocalVar *sp = (m && m->pnames[i]) ? scope_local(m, m->pnames[i]) : NULL;
      TyKind set = ty_array_elem(splat_at);
      Buf eb; memset(&eb, 0, sizeof eb);
      if (sp && sp->type == TY_POLY && set != TY_POLY && set != TY_UNKNOWN) {
        /* a scalar splat element into a poly-widened param: box it */
        Buf raw; memset(&raw, 0, sizeof raw);
        emit_array_elem_at(splat_at, splat_tmp, off, &raw);
        emit_boxed_text(c, set, raw.p ? raw.p : "0", &eb); free(raw.p);
      }
      else emit_array_elem_at(splat_at, splat_tmp, off, &eb);
      /* An optional param may fall past the end of a (runtime-sized) splat
         array; the arity check guarantees the required params are present, so
         guard only the optionals and fall back to their default. */
      if (i >= m->nrequired) {
        Buf db; memset(&db, 0, sizeof db);
        emit_arg_or_default(c, m, i, -1, &db);
        TyKind pt = sp ? sp->type : TY_INT;
        buf_printf(out, "(%d < (_t%d ? _t%d->len : 0) ? %s : %s)", off, splat_tmp, splat_tmp,
                   eb.p ? eb.p : "", db.p ? db.p : default_value(pt));
        free(db.p);
      }
      else buf_puts(out, eb.p ? eb.p : "");
      free(eb.p);
    }
else {
      /* Check if this param has a keyword match (lookup by param name in kwh). */
      int kv = kwh >= 0 ? kwh_lookup(nt, kwh, m->pnames[i]) : -1;
      if (kv >= 0) {
        emit_arg_rooted(c, m, i, kv, out);
      }
      else if (ds_hash_tmp >= 0 && m->pnames[i] && i != m->kwrest_idx) {
        /* Double-splat: extract param by name from the pre-eval'd hash. */
        const char *hn = ty_hash_cname(ds_hash_type);
        LocalVar *plv = scope_local(m, m->pnames[i]);
        TyKind pt = plv ? plv->type : TY_INT;
        if (hn) {
          /* SymPoly: get returns sp_RbVal, unbox to param type.
             Other sym/str keyed hashes: get returns the value type directly. */
          TyKind hval = ty_hash_val(ds_hash_type);
          Buf vb; memset(&vb, 0, sizeof vb);
          char get_expr[256];
          snprintf(get_expr, sizeof get_expr,
                   "sp_%sHash_get(_t%d, sp_sym_intern(\"%s\"))",
                   hn, ds_hash_tmp, m->pnames[i]);
          if (hval == TY_POLY) emit_unbox_text(c, pt, get_expr, &vb);
          else buf_puts(&vb, get_expr);
          /* An optional keyword param (one with a default) whose key may be
             absent from the forwarded hash falls back to its default: a bare
             get returns nil and silently drops the callee's default value. */
          if (m->pdefault && m->pdefault[i] >= 0) {
            Buf db; memset(&db, 0, sizeof db);
            emit_arg_or_default(c, m, i, -1, &db);
            buf_printf(out, "(sp_%sHash_has_key(_t%d, sp_sym_intern(\"%s\")) ? (%s) : (%s))",
                       hn, ds_hash_tmp, m->pnames[i],
                       vb.p ? vb.p : "", db.p ? db.p : default_value(pt));
            free(db.p);
          }
          else buf_puts(out, vb.p ? vb.p : "");
          free(vb.p);
        }
        else {
          buf_printf(out, "%s", default_value(pt));
        }
      }
      else if (m->kwrest_idx >= 0 && i == m->kwrest_idx) {
        /* Collect remaining (unbound) keyword args into a sp_SymPolyHash. */
        int krhash = ++g_tmp;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_SymPolyHash *_t%d = sp_SymPolyHash_new();\n", krhash);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", krhash);
        if (kwh >= 0) {
          int en3 = 0; const int *elems3 = nt_arr(nt, kwh, "elements", &en3);
          int splat_seen = 0;
          for (int e3 = 0; e3 < en3; e3++) {
            const char *ety3 = nt_type(nt, elems3[e3]);
            if (ety3 && sp_streq(ety3, "AssocSplatNode")) {
              /* Forwarded `**hash`: merge its entries into the keyword-rest
                 (later entries win, so order with literals is preserved). Only
                 a symbol-keyed hash can flow into a keyword-rest parameter. */
              int inner3 = nt_ref(nt, elems3[e3], "value");
              if (inner3 < 0) {
                /* Anonymous `**`: merge the enclosing __anon_kwrest directly. */
                const char *akw = anon_kwrest_name(c, elems3[e3]);
                if (!akw) continue;
                splat_seen = 1;
                emit_indent(g_pre, g_indent);
                buf_printf(g_pre, "sp_SymPolyHash_update(_t%d, lv_%s);\n", krhash, akw);
                continue;
              }
              const char *shn = ty_hash_cname(comp_ntype(c, inner3));
              if (!shn || !sp_streq(shn, "SymPoly")) {
                unsupported(c, argsNode, "double-splat forward of a non-symbol-keyed hash into a keyword-rest parameter");
                continue;
              }
              int src;
              if (!splat_seen && ds_hash_tmp >= 0) {
                /* Reuse the first splat's materialized temp. It is declared with
                   ds_hash_type's C type, so it must be SymPoly to flow into
                   sp_SymPolyHash_update (the inner3 check above guarantees this
                   for the matching first splat; assert it explicitly so the
                   type-punned reuse can't silently emit a mismatched pointer). */
                const char *dshn = ty_hash_cname(ds_hash_type);
                if (!dshn || !sp_streq(dshn, "SymPoly")) {
                  unsupported(c, argsNode, "double-splat forward of a non-symbol-keyed hash into a keyword-rest parameter");
                  continue;
                }
                src = ds_hash_tmp;  /* first splat already materialized above */
              } else {
                src = ++g_tmp;
                emit_indent(g_pre, g_indent);
                buf_printf(g_pre, "sp_SymPolyHash *_t%d = ", src);
                emit_expr(c, inner3, g_pre);
                buf_puts(g_pre, ";\n");
                emit_indent(g_pre, g_indent);
                buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", src);
              }
              splat_seen = 1;
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "sp_SymPolyHash_update(_t%d, _t%d);\n", krhash, src);
              continue;
            }
            int key3 = nt_ref(nt, elems3[e3], "key");
            int val3 = nt_ref(nt, elems3[e3], "value");
            if (key3 < 0 || val3 < 0) continue;
            const char *kty3 = nt_type(nt, key3);
            const char *kname3 = (kty3 && sp_streq(kty3, "SymbolNode")) ? nt_str(nt, key3, "value") : NULL;
            if (!kname3) continue;
            /* A literal `k: v` whose name is an explicit keyword param is bound
               to that param, not the keyword-rest. A positional param of the
               same name does not consume it. */
            if (callee_has_kwarg(c, m, kname3)) continue;
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_SymPolyHash_set(_t%d, sp_sym_intern(\"%s\"), ", krhash, kname3);
            emit_boxed(c, val3, g_pre);
            buf_puts(g_pre, ");\n");
          }
          /* Keys merged from a `**hash` that name an explicit keyword param are
             consumed by that param, so drop them from the keyword-rest. Only
             keyword params consume keys -- a positional param of the same name
             leaves its key in the rest. */
          if (splat_seen && m->def_node >= 0) {
            int dpn = nt_ref(nt, m->def_node, "parameters");
            int kpn = 0; const int *kwps = dpn >= 0 ? nt_arr(nt, dpn, "keywords", &kpn) : NULL;
            for (int kk = 0; kk < kpn; kk++) {
              const char *kpname = nt_str(nt, kwps[kk], "name");
              if (!kpname) continue;
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "sp_SymPolyHash_delete(_t%d, sp_sym_intern(\"%s\"));\n",
                         krhash, kpname);
            }
          }
        }
        buf_printf(out, "_t%d", krhash);
      }
      else if (i < pos_argc) {
        emit_arg_rooted(c, m, i, argv[i], out);
      }
      else {
        /* No positional arg and no keyword match. If the param is hash-typed
           (required `def f(attrs)` or optional `def f(opts = {})`) and the
           call site passed a KeywordHashNode (e.g. `f(key: val)`), Ruby packs
           the keywords into that hash parameter -- treat the whole kwh as the
           implicit hash argument rather than using the default. */
        LocalVar *p = scope_local(m, m->pnames[i]);
        TyKind pt = p ? p->type : TY_INT;
        int use_kwh = (kwh >= 0 && ty_is_hash(pt));
        emit_arg_rooted(c, m, i, use_kwh ? kwh : -1, out);
      }
    }
  }
  g_n_argov = argov_saved;  /* drop this call's hoisted-arg overrides */
}

int is_descendant(Compiler *c, int k, int anc) {
  for (int x = k; x >= 0; x = c->classes[x].parent) if (x == anc) return 1;
  return 0;
}

/* Number of distinct implementations of `name` across cid's subtree
   (cid + all descendants). >1 means a self/obj call needs runtime dispatch. */
int dispatch_impl_count(Compiler *c, int cid, const char *name) {
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
   are pre-evaluated into temps so they're emitted once.
   `blk_node` is the BlockNode id of the attached block, or -1 if none. */
void emit_dispatch(Compiler *c, int cid, const char *name,
                          const char *selfptr, int argsNode, int blk_node, Buf *b) {
  const NodeTable *nt = c->nt;
  int defcls = cid;
  int mi = comp_method_in_chain(c, cid, name, &defcls);
  Scope *m = mi >= 0 ? &c->scopes[mi] : NULL;
  TyKind ret = m ? m->ret : TY_UNKNOWN;
  /* Unify return type across all descendant implementations so that even
     when the base method has TY_VOID/TY_UNKNOWN, a subclass override
     with a real return type makes the dispatch virtual and typed. */
  for (int k = 0; k < c->nclasses; k++) {
    if (!is_descendant(c, k, cid)) continue;
    int kd = -1;
    int kmi = comp_method_in_chain(c, k, name, &kd);
    if (kmi >= 0 && (TyKind)c->scopes[kmi].ret != TY_UNKNOWN)
      ret = ty_unify(ret, (TyKind)c->scopes[kmi].ret);
  }

  int argc = 0;
  const int *argv = argsNode >= 0 ? nt_arr(nt, argsNode, "arguments", &argc) : NULL;
  /* `callee(...)`: forward the enclosing `def foo(...)` method's synthesized
     __fwd_* params positionally (#1288), same as the emit_args_filled path. */
  Scope *fwd_encl = NULL;
  if (argc == 1 && argv && nt_type(nt, argv[0]) &&
      sp_streq(nt_type(nt, argv[0]), "ForwardingArgumentsNode"))
    fwd_encl = comp_scope_of(c, argv[0]);
  /* separate keyword-hash arg */
  int kwh_d = -1, pos_argc_d = argc;
  if (argc > 0 && nt_type(nt, argv[argc - 1]) &&
      sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) {
    kwh_d = argv[argc - 1]; pos_argc_d = argc - 1;
  }
  int np = m ? m->nparams : pos_argc_d;
  /* evaluate each param value (provided arg or default) into a temp so the
     virtual-dispatch cases reuse them without re-evaluating */
  int *atmp = np ? malloc(sizeof(int) * np) : NULL;
  const char *saved_self = g_self;
  /* A positional SplatNode `obj.f(*args)` expands across the fixed params, the
     same way emit_args_filled handles it for free-function calls: pre-evaluate
     the array to a rooted temp and fill each param from it. Scoped to a fixed-
     arity callee (no rest param). */
  int splat_idx_d = -1, splat_tmp_d = -1; TyKind splat_at_d = TY_UNKNOWN;
  for (int k = 0; m && m->rest_idx < 0 && k < pos_argc_d; k++) {
    if (argv && nt_type(nt, argv[k]) && sp_streq(nt_type(nt, argv[k]), "SplatNode") &&
        k < m->nparams) {
      int inner = nt_ref(nt, argv[k], "expression");
      splat_at_d = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
      if (ty_is_array(splat_at_d) || splat_at_d == TY_POLY_ARRAY) {
        splat_idx_d = k;
        splat_tmp_d = ++g_tmp;
        Buf sb; memset(&sb, 0, sizeof sb);
        emit_expr(c, inner, &sb);
        emit_indent(g_pre, g_indent);
        emit_ctype(c, splat_at_d, g_pre);
        buf_printf(g_pre, " _t%d = %s;\n", splat_tmp_d, sb.p ? sb.p : "");
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", splat_tmp_d);
        free(sb.p);
        /* Arity check when the splat is the last positional group: the total
           given count is splat_idx + array length. */
        if (k == pos_argc_d - 1) {
          char expbuf[48];
          if (m->nrequired == m->nparams)
            snprintf(expbuf, sizeof expbuf, "expected %d", m->nparams);
          else
            snprintf(expbuf, sizeof expbuf, "expected %d..%d", m->nrequired, m->nparams);
          int gv = ++g_tmp;
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = %d + (_t%d ? _t%d->len : 0);\n", gv, splat_idx_d, splat_tmp_d, splat_tmp_d);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre,
                     "if (_t%d < %d || _t%d > %d) sp_raise_cls(\"ArgumentError\", sp_sprintf(\"wrong number of arguments (given %%lld, %s)\", (long long)_t%d));\n",
                     gv, m->nrequired, gv, m->nparams, expbuf, gv);
        }
      }
      break;
    }
  }
  for (int k = 0; k < np; k++) {
    atmp[k] = ++g_tmp;
    Buf ab; memset(&ab, 0, sizeof ab);
    LocalVar *p = m ? scope_local(m, m->pnames[k]) : NULL;
    if (m && m->rest_idx >= 0 && k == m->rest_idx) {
      /* rest param: pack remaining positional args into PolyArray */
      emit_rest_pack(c, k, pos_argc_d, argv, &ab);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = %s;\n", atmp[k], ab.p ? ab.p : "sp_PolyArray_new()");
    }
    else if (fwd_encl && k < fwd_encl->nparams) {
      LocalVar *ep = scope_local(fwd_encl, fwd_encl->pnames[k]);
      TyKind et = ep ? ep->type : TY_POLY;
      char txt[80]; snprintf(txt, sizeof txt, "lv_%s", fwd_encl->pnames[k]);
      if (p && (p->type == TY_POLY || p->type == TY_UNKNOWN) && et != TY_POLY) emit_boxed_text(c, et, txt, &ab);
      else buf_puts(&ab, txt);
      TyKind att = p ? (p->type == TY_UNKNOWN ? TY_POLY : p->type) : et;
      emit_indent(g_pre, g_indent);
      emit_ctype(c, att, g_pre);
      buf_printf(g_pre, " _t%d = ", atmp[k]);
      buf_puts(g_pre, ab.p ? ab.p : ""); buf_puts(g_pre, ";\n");
      if (att == TY_POLY) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", atmp[k]); }
      free(ab.p);
      continue;
    }
else {
      int kv = (m && kwh_d >= 0) ? kwh_lookup(nt, kwh_d, m->pnames[k]) : -1;
      int provided = kv >= 0 ? kv : (k < pos_argc_d ? argv[k] : -1);
      if (splat_tmp_d >= 0 && k >= splat_idx_d) {
        /* fill this fixed param from the splatted array at offset k-splat_idx */
        int off = k - splat_idx_d;
        TyKind set = ty_array_elem(splat_at_d);
        Buf eb; memset(&eb, 0, sizeof eb);
        if (p && p->type == TY_POLY && set != TY_POLY && set != TY_UNKNOWN) {
          /* a scalar splat element into a poly-widened param: box it */
          Buf raw; memset(&raw, 0, sizeof raw);
          emit_array_elem_at(splat_at_d, splat_tmp_d, off, &raw);
          emit_boxed_text(c, set, raw.p ? raw.p : "0", &eb); free(raw.p);
        }
        else emit_array_elem_at(splat_at_d, splat_tmp_d, off, &eb);
        /* an optional param may fall past the (runtime-sized) array end; the
           arity check covers required params, so guard only the optionals */
        if (k >= m->nrequired) {
          Buf db; memset(&db, 0, sizeof db);
          emit_arg_or_default(c, m, k, -1, &db);
          TyKind pt = p ? p->type : TY_INT;
          buf_printf(&ab, "(%d < (_t%d ? _t%d->len : 0) ? %s : %s)", off, splat_tmp_d, splat_tmp_d,
                     eb.p ? eb.p : "", db.p ? db.p : default_value(pt));
          free(db.p);
        }
        else buf_puts(&ab, eb.p ? eb.p : "");
        free(eb.p);
      }
      else {
        /* Default expressions (e.g. `@ivar * 10`) reference the callee's self and
           callee's class, not the caller's. Temporarily redirect both. A value-
           type receiver is a by-value struct, so its ivars dereference with `.`,
           not `->` (without this, `def m(r = @r)` emits `selfval->iv_r`). */
        int saved_emcls2 = g_emitting_class_id;
        const char *saved_deref3 = g_self_deref;
        if (provided < 0) {
          g_self = selfptr;
          g_self_deref = comp_ty_value_obj(c, ty_object(cid)) ? "." : "->";
          if (m) g_emitting_class_id = m->class_id;
        }
        emit_arg_or_default(c, m, k, provided, &ab);
        g_self = saved_self;
        g_self_deref = saved_deref3;
        g_emitting_class_id = saved_emcls2;
      }
      TyKind att = p ? p->type : comp_ntype(c, k < argc ? argv[k] : -1);
      if (p && att == TY_UNKNOWN) att = TY_POLY;  /* poly in the callee signature */
      emit_indent(g_pre, g_indent);
      emit_ctype(c, att, g_pre);
      buf_printf(g_pre, " _t%d = ", atmp[k]);
      buf_puts(g_pre, ab.p ? ab.p : ""); buf_puts(g_pre, ";\n");
      /* Root heap-typed arg temps: evaluating a later argument may allocate
         and collect an earlier one still sitting in its temp. */
      if (att == TY_POLY) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", atmp[k]); }
      else if (needs_root(att)) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", atmp[k]); }
    }
    free(ab.p);
  }

  /* &block param that escapes: pre-evaluate the block as sp_Proc * temp.
     When the call site has no block, blk_tmp stays -1 and we pass NULL. */
  int blk_tmp = -1;
  int needs_blk_arg = m && m->blk_param && m->blk_param[0] && !m->yields;
  if (needs_blk_arg) blk_node = resolve_forwarded_block(c, blk_node);
  if (needs_blk_arg && blk_node >= 0) {
    blk_tmp = ++g_tmp;
    Buf pb; memset(&pb, 0, sizeof pb);
    emit_proc_literal(c, blk_node, &pb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_Proc *_t%d = %s;\n", blk_tmp, pb.p ? pb.p : "NULL");
    free(pb.p);
  }

  /* The aliased name may differ from the defining method's real name. */
  const char *mname = m ? m->name : name;

  /* Force a runtime switch when there is no base implementation (m == NULL):
     a template method defined only in subclasses cannot be called directly as
     sp_<base>_<name>, so even a single descendant impl must dispatch virtually. */
  int impl_n = dispatch_impl_count(c, cid, name);
  /* A void/nil-returning method that subclasses override must still dispatch on
     the runtime class -- an implicit-self call to it from a base method (e.g.
     `def run; validate; end` where each subclass overrides `validate`) would
     otherwise bind statically to the base impl and skip the override (#1443).
     The GCC statement-expression wrapper can't declare a `void` result, so a
     void dispatch uses a dummy int temp (its value is discarded). */
  int ret_is_void = (ret == TY_VOID || ret == TY_NIL);
  TyKind disp_ret = ret_is_void ? TY_INT : ret;
  int virtual = (is_scalar_ret(ret) || ret_is_void) && (impl_n > 1 || (!m && impl_n >= 1));

  if (!virtual) {
    /* a value-type receiver is passed by value (no pointer cast) */
    if (comp_ty_value_obj(c, ty_object(cid)))
      buf_printf(b, "sp_%s_%s(%s", c->classes[defcls].c_name, mc(mname), selfptr);
    else
      buf_printf(b, "sp_%s_%s((sp_%s *)%s", c->classes[defcls].c_name, mc(mname), c->classes[defcls].c_name, selfptr);
    for (int k = 0; k < np; k++) buf_printf(b, ", _t%d", atmp[k]);
    if (needs_blk_arg) {
      if (blk_tmp >= 0) buf_printf(b, ", _t%d", blk_tmp);
      else buf_puts(b, ", NULL");
    }
    buf_puts(b, ")");
    free(atmp);
    return;
  }

  /* runtime dispatch on cls_id (GCC statement-expression) */
  int rtmp = ++g_tmp;
  buf_puts(b, "({ ");
  emit_ctype(c, disp_ret, b);
  buf_printf(b, " _t%d; switch ((%s)->cls_id) {", rtmp, selfptr);
  for (int k = 0; k < c->nclasses; k++) {
    if (!is_descendant(c, k, cid)) continue;
    int kd = -1;
    int kmi = comp_method_in_chain(c, k, name, &kd);
    if (kmi < 0) continue;
    /* A `case` arm calling an override with no standalone definition (DCE-pruned
       or yield-inlined) would reference an absent symbol and dangle at link; the
       class can't be the receiver here anyway. Same guard as the poly-dispatch
       loops in codegen_call.c (issue #1583). */
    if (!scope_has_callable_symbol(c, kmi)) continue;
    TyKind arm_ret = (TyKind)c->scopes[kmi].ret;
    const char *kfn = mc(c->scopes[kmi].name);
    if (method_is_void(&c->scopes[kmi])) {
      /* override emitted as a void C function (method_is_void: VOID/NIL/UNKNOWN
         ret, or initialize) -- call it, assign nil/zero to the result temp */
      buf_printf(b, " case %d: sp_%s_%s((sp_%s *)%s", k,
                 c->classes[kd].c_name, kfn, c->classes[kd].c_name, selfptr);
      for (int a = 0; a < np; a++) buf_printf(b, ", _t%d", atmp[a]);
      buf_printf(b, "); _t%d = %s; break;", rtmp, default_value(disp_ret));
    }
    else if (arm_ret != ret && ret == TY_POLY) {
      /* arm returns a concrete type but switch expects sp_RbVal: box it */
      buf_printf(b, " case %d: { ", k);
      Buf _bx; memset(&_bx, 0, sizeof _bx);
      buf_printf(&_bx, "sp_%s_%s((sp_%s *)%s",
                 c->classes[kd].c_name, kfn, c->classes[kd].c_name, selfptr);
      for (int a = 0; a < np; a++) buf_printf(&_bx, ", _t%d", atmp[a]);
      buf_puts(&_bx, ")");
      buf_printf(b, "_t%d = ", rtmp);
      emit_boxed_text(c, arm_ret, _bx.p ? _bx.p : "0", b);
      free(_bx.p);
      buf_puts(b, "; break; }");
    }
    else {
      buf_printf(b, " case %d: _t%d = sp_%s_%s((sp_%s *)%s", k, rtmp,
                 c->classes[kd].c_name, kfn, c->classes[kd].c_name, selfptr);
      for (int a = 0; a < np; a++) buf_printf(b, ", _t%d", atmp[a]);
      buf_puts(b, "); break;");
    }
  }
  /* When the method is defined only in descendants (m == NULL), the base class
     has no implementation. The default arm is unreachable (self is always a
     descendant that has the method), so emit a typed placeholder rather than a
     call to a nonexistent sp_<base>_<name>. */
  if (!m) {
    buf_printf(b, " default: _t%d = %s; break; } _t%d; })", rtmp,
               ret == TY_POLY ? "sp_box_nil()" : default_value(disp_ret), rtmp);
    free(atmp);
    return;
  }
  /* default arm uses the base-class (defcls) implementation */
  TyKind def_ret = (TyKind)m->ret;
  if (method_is_void(m)) {
    buf_printf(b, " default: sp_%s_%s((sp_%s *)%s",
               c->classes[defcls].c_name, mc(mname), c->classes[defcls].c_name, selfptr);
    for (int a = 0; a < np; a++) buf_printf(b, ", _t%d", atmp[a]);
    buf_printf(b, "); _t%d = %s; break;", rtmp, default_value(disp_ret));
  }
  else if (def_ret != ret && ret == TY_POLY) {
    buf_printf(b, " default: { ");
    Buf _bx; memset(&_bx, 0, sizeof _bx);
    buf_printf(&_bx, "sp_%s_%s((sp_%s *)%s",
               c->classes[defcls].c_name, mc(mname), c->classes[defcls].c_name, selfptr);
    for (int a = 0; a < np; a++) buf_printf(&_bx, ", _t%d", atmp[a]);
    buf_puts(&_bx, ")");
    buf_printf(b, "_t%d = ", rtmp);
    emit_boxed_text(c, def_ret, _bx.p ? _bx.p : "0", b);
    free(_bx.p);
    buf_puts(b, "; break; }");
  }
  else {
    buf_printf(b, " default: _t%d = sp_%s_%s((sp_%s *)%s", rtmp,
               c->classes[defcls].c_name, mc(mname), c->classes[defcls].c_name, selfptr);
    for (int a = 0; a < np; a++) buf_printf(b, ", _t%d", atmp[a]);
    buf_puts(b, "); break;");
  }
  buf_printf(b, " } _t%d; })", rtmp);
  free(atmp);
}

/* array.group_by { |x| key_expr } -> sp_PolyPolyHash (pre-statements to g_pre) */
int emit_group_by_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "group_by")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind et = ty_array_elem(rt);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  const char *p0_orig = block_param_name(c, block, 0);
  const char *p0 = p0_orig ? rename_local(p0_orig) : NULL;

  int trecv = ++g_tmp, thash = ++g_tmp, tkey = ++g_tmp, tarr = ++g_tmp, ti = ++g_tmp;

  /* emit receiver */
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
  /* result hash */
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyPolyHash *_t%d = sp_PolyPolyHash_new(); SP_GC_ROOT(_t%d);\n", thash, thash);
  /* loop */
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
             ti, ti, k, trecv, ti);
  /* assign element to block param(s). A 2+-param block over a poly array of
     sub-arrays auto-splats each element across the params (the bucket push below
     re-reads and stores the whole element regardless). */
  int np_gb = 0; while (block_param_name(c, block, np_gb)) np_gb++;
  if (np_gb >= 2 && rt == TY_POLY_ARRAY && !block_param_is_multi(c, block, 0)) {
    int telem = ++g_tmp;
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);\n", telem, trecv, ti);
    emit_autosplat_params(c, block, np_gb, telem, g_indent + 1);
  }
  else if (p0) {
    Scope *cs = comp_scope_of(c, id);
    LocalVar *outer_p0 = cs ? scope_local(cs, p0) : NULL;
    TyKind p0_type = outer_p0 ? outer_p0->type : et;
    emit_indent(g_pre, g_indent + 1);
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
  /* evaluate block body side-effect stmts, then key expression */
  int save_indent = g_indent; g_indent++;
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent);
  g_indent = save_indent;
  TyKind key_t = comp_ntype(c, bb[bn - 1]);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_RbVal _t%d = ", tkey);
  if (key_t != TY_POLY) {
    Buf kbuf; memset(&kbuf, 0, sizeof kbuf);
    int save2 = g_indent; g_indent += 1;
    emit_expr(c, bb[bn - 1], &kbuf);
    g_indent = save2;
    emit_boxed_text(c, key_t, kbuf.p ? kbuf.p : "0", g_pre);
    free(kbuf.p);
  }
  else {
    int save2 = g_indent; g_indent += 1;
    emit_expr(c, bb[bn - 1], g_pre);
    g_indent = save2;
  }
  buf_puts(g_pre, ";\n");
  /* get-or-create the PolyArray for this key */
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_PolyArray *_t%d;\n", tarr);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "if (sp_PolyPolyHash_has_key(_t%d, _t%d)) {\n", thash, tkey);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "_t%d = (sp_PolyArray *)sp_PolyPolyHash_get(_t%d, _t%d).v.p;\n", tarr, thash, tkey);
  emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "else {\n");
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tarr, tarr);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "sp_PolyPolyHash_set(_t%d, _t%d, sp_box_obj(_t%d, SP_BUILTIN_POLY_ARRAY));\n",
             thash, tkey, tarr);
  emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
  /* push element (boxed) */
  emit_indent(g_pre, g_indent + 1);
  if (et != TY_POLY) {
    char elem_s[64];
    snprintf(elem_s, sizeof elem_s, "sp_%sArray_get(_t%d, _t%d)", k, trecv, ti);
    Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, et, elem_s, &bx);
    buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s);\n", tarr, bx.p ? bx.p : "sp_box_nil()");
    free(bx.p);
  }
  else {
    buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_%sArray_get(_t%d, _t%d));\n", tarr, k, trecv, ti);
  }
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  /* the expression evaluates to the hash */
  buf_printf(b, "_t%d", thash);
  return 1;
}

/* array.each_with_object(init) { |x, acc| ... } → acc (pre-statements to g_pre) */
int emit_each_with_object_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "each_with_object")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *bty = nt_type(nt, block);
  if (!bty || !sp_streq(bty, "BlockNode")) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (argc < 1 || !argv) return 0;
  TyKind rt = comp_ntype(c, recv);

  /* Hash receiver: hash.each_with_object(memo) { |(k,v), acc| } or { |k,v,acc| } */
  if (ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    if (!hn) return 0;
    int body = nt_ref(nt, block, "body");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    if (bn < 1) return 0;
    /* Detect block param form: |(k,v), memo| (multi at idx 0), flat |k, v, memo|,
       or |element, memo| where element is the [k,v] pair (the shape the destructure
       desugar produces from |(k,v), memo|). */
    int is_multi = block_param_is_multi(c, block, 0);
    const char *kname_orig = NULL, *vname_orig = NULL, *mname_orig = NULL, *pairname_orig = NULL;
    int pair_mode = 0;
    if (is_multi) {
      kname_orig = block_param_multi_leaf(c, block, 0, 0);
      vname_orig = block_param_multi_leaf(c, block, 0, 1);
      mname_orig = block_param_name(c, block, 1);
    }
    else {
      const char *p0n = block_param_name(c, block, 0);
      const char *p1n = block_param_name(c, block, 1);
      const char *p2n = block_param_name(c, block, 2);
      if (p1n && !p2n) {
        pair_mode = 1;
        pairname_orig = p0n;
        mname_orig = p1n;
      }
      else {
        kname_orig = p0n;
        vname_orig = p1n;
        mname_orig = p2n;
      }
    }
    const char *kname = kname_orig ? rename_local(kname_orig) : NULL;
    const char *vname = vname_orig ? rename_local(vname_orig) : NULL;
    const char *mname = mname_orig ? rename_local(mname_orig) : NULL;
    const char *pairname = pairname_orig ? rename_local(pairname_orig) : NULL;
    TyKind accT = infer_type(c, argv[0]);
    if (accT == TY_UNKNOWN) {
      const char *a0ty = nt_type(nt, argv[0]);
      int an0 = 0;
      if (a0ty && sp_streq(a0ty, "ArrayNode")) nt_arr(nt, argv[0], "elements", &an0);
      if (a0ty && sp_streq(a0ty, "ArrayNode") && an0 == 0) accT = TY_INT_ARRAY;
      else return 0;
    }
    /* Receiver */
    int trecv = ++g_tmp;
    { Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
      emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
      buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p); }
    /* Accumulator: use memo's declared type (may differ from accT if widened) */
    Scope *cs = comp_scope_of(c, id);
    LocalVar *memo_lv = mname_orig && cs ? scope_local(cs, mname_orig) : NULL;
    TyKind memo_decl = (memo_lv && memo_lv->type != TY_UNKNOWN) ? memo_lv->type : accT;
    int tacc = ++g_tmp;
    { Buf ab; memset(&ab, 0, sizeof ab); emit_expr(c, argv[0], &ab);
      emit_indent(g_pre, g_indent); emit_ctype(c, memo_decl, g_pre);
      if (memo_decl != accT)
        buf_printf(g_pre, " _t%d = %s_new();\n", tacc,
                   memo_decl == TY_POLY_ARRAY ? "sp_PolyArray" : "sp_IntArray");
      else
        buf_printf(g_pre, " _t%d = %s;\n", tacc, ab.p ? ab.p : default_value(memo_decl));
      free(ab.p); }
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tacc);
    /* Bind accumulator to memo param before loop */
    if (mname) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "lv_%s = _t%d;\n", mname, tacc);
    }
    /* Loop */
    int ti = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
    /* Bind the [k,v] pair for the |element, memo| form (CRuby yields the pair). */
    if (pairname) {
      char kexpr[96], vexpr[96];
      if (rt == TY_POLY_POLY_HASH) {
        snprintf(kexpr, sizeof kexpr, "_t%d->keys[_t%d->order[_t%d]]", trecv, trecv, ti);
        snprintf(vexpr, sizeof vexpr, "_t%d->vals[_t%d->order[_t%d]]", trecv, trecv, ti);
      }
      else {
        snprintf(kexpr, sizeof kexpr, "_t%d->order[_t%d]", trecv, ti);
        snprintf(vexpr, sizeof vexpr, "sp_%sHash_get(_t%d, _t%d->order[_t%d])", hn, trecv, trecv, ti);
      }
      int tpair = ++g_tmp;
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tpair, tpair);
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", tpair);
      if (rt == TY_POLY_POLY_HASH) buf_puts(g_pre, kexpr);
      else { Buf kb; memset(&kb, 0, sizeof kb); emit_boxed_text(c, ty_hash_key(rt), kexpr, &kb);
             buf_puts(g_pre, kb.p ? kb.p : kexpr); free(kb.p); }
      buf_puts(g_pre, ");\n");
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", tpair);
      if (rt == TY_POLY_POLY_HASH) buf_puts(g_pre, vexpr);
      else { Buf vb; memset(&vb, 0, sizeof vb); emit_boxed_text(c, ty_hash_val(rt), vexpr, &vb);
             buf_puts(g_pre, vb.p ? vb.p : vexpr); free(vb.p); }
      buf_puts(g_pre, ");\n");
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "lv_%s = _t%d;\n", pairname, tpair);
    }
    /* Assign key */
    if (kname) {
      emit_indent(g_pre, g_indent + 1);
      if (rt == TY_POLY_POLY_HASH)
        buf_printf(g_pre, "lv_%s = _t%d->keys[_t%d->order[_t%d]];\n", kname, trecv, trecv, ti);
      else
        buf_printf(g_pre, "lv_%s = _t%d->order[_t%d];\n", kname, trecv, ti);
    }
    /* Assign value */
    if (vname) {
      emit_indent(g_pre, g_indent + 1);
      if (rt == TY_POLY_POLY_HASH)
        buf_printf(g_pre, "lv_%s = _t%d->vals[_t%d->order[_t%d]];\n", vname, trecv, trecv, ti);
      else
        buf_printf(g_pre, "lv_%s = sp_%sHash_get(_t%d, _t%d->order[_t%d]);\n", vname, hn, trecv, trecv, ti);
    }
    /* Body */
    int save_indent = g_indent; g_indent++;
    emit_loop_body(c, body, g_pre, g_indent);
    g_indent = save_indent;
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
    buf_printf(b, "_t%d", tacc);
    return 1;
  }

  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind et = ty_array_elem(rt);
  TyKind accT = infer_type(c, argv[0]);
  int empty_seed = 0;       /* empty `[]` -> fresh typed array */
  int empty_hash_seed = 0;  /* empty `{}` -> fresh boxed hash */
  if (accT == TY_UNKNOWN) {
    const char *a0ty = nt_type(nt, argv[0]);
    int an0 = 0;
    if (a0ty && sp_streq(a0ty, "ArrayNode")) nt_arr(nt, argv[0], "elements", &an0);
    if (a0ty && sp_streq(a0ty, "ArrayNode") && an0 == 0) {
      empty_seed = 1;
      TyKind me = ewo_memo_elem_type(c, id);
      accT = (me != TY_UNKNOWN) ? ty_array_of(me) : TY_INT_ARRAY;
    }
    else if (a0ty && sp_streq(a0ty, "HashNode") &&
             (nt_arr(nt, argv[0], "elements", &an0), an0 == 0)) {
      empty_hash_seed = 1;
      accT = TY_POLY_POLY_HASH;
    }
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

  /* Accumulator: an empty `[]` seed is built as a fresh typed array of the
     inferred element type, so a string/poly memo isn't materialized as int
     storage; a non-empty seed is emitted as written. */
  int tacc = ++g_tmp;
  if (empty_hash_seed) {
    emit_indent(g_pre, g_indent); emit_ctype(c, accT, g_pre);
    buf_printf(g_pre, " _t%d = sp_PolyPolyHash_new();\n", tacc);
  }
  else if (empty_seed) {
    const char *ak = (accT == TY_POLY_ARRAY) ? "Poly" : array_kind(accT);
    emit_indent(g_pre, g_indent); emit_ctype(c, accT, g_pre);
    buf_printf(g_pre, " _t%d = sp_%sArray_new();\n", tacc, ak ? ak : "Int");
  }
  else {
    /* a non-empty seed's emit_expr writes its construction to the prelude first,
       so the ctype/assign must follow it. */
    Buf accb; memset(&accb, 0, sizeof accb); emit_expr(c, argv[0], &accb);
    emit_indent(g_pre, g_indent); emit_ctype(c, accT, g_pre);
    buf_printf(g_pre, " _t%d = %s;\n", tacc, accb.p ? accb.p : default_value(accT)); free(accb.p);
  }

  /* Save outer vars if block params shadow them (same-type only) */
  Scope *cs = comp_scope_of(c, id);
  LocalVar *outer_p0 = (p0 && cs) ? scope_local(cs, p0) : NULL;
  int ts_p0 = 0;
  int p0_mismatch = outer_p0 && outer_p0->type != et;
  if (outer_p0 && !p0_mismatch) {
    ts_p0 = ++g_tmp;
    Buf ot; memset(&ot, 0, sizeof ot); emit_ctype(c, outer_p0->type, &ot);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "%s _t%d = lv_%s;\n", ot.p ? ot.p : "mrb_int", ts_p0, p0); free(ot.p);
  }
  LocalVar *outer_p1 = (p1 && cs) ? scope_local(cs, p1) : NULL;
  int ts_p1 = 0;
  int p1_mismatch = outer_p1 && outer_p1->type != accT;
  if (outer_p1 && !p1_mismatch) {
    ts_p1 = ++g_tmp;
    Buf ot; memset(&ot, 0, sizeof ot); emit_ctype(c, outer_p1->type, &ot);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "%s _t%d = lv_%s;\n", ot.p ? ot.p : "mrb_int", ts_p1, p1); free(ot.p);
  }

  /* When block params shadow outer vars with different types, open a C scope
     and declare typed inner shadows so the body dispatches correctly. */
  int need_scope = p0_mismatch || p1_mismatch;
  if (need_scope) {
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "{\n");
    g_indent++;
    if (p1_mismatch) {
      emit_indent(g_pre, g_indent);
      emit_ctype(c, accT, g_pre);
      buf_printf(g_pre, " lv_%s = _t%d;\n", p1, tacc);
    }
    /* Temporarily override types for body dispatch */
    TyKind saved_p0_type = outer_p0 ? outer_p0->type : TY_UNKNOWN;
    TyKind saved_p1_type = outer_p1 ? outer_p1->type : TY_UNKNOWN;
    if (p0_mismatch) outer_p0->type = et;
    if (p1_mismatch) outer_p1->type = accT;
    for (int j = 0; j < bn; j++) infer_type(c, bb[j]);
    /* Bind accumulator to p1 before loop (type now matches) */
    if (p1 && !p1_mismatch) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "lv_%s = _t%d;\n", p1, tacc);
    }
    /* Loop */
    int ti = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
               ti, ti, k, trecv, ti);
    if (p0) {
      emit_indent(g_pre, g_indent + 1);
      if (p0_mismatch) {
        emit_ctype(c, et, g_pre);
        buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
      }
      else if (outer_p0 && outer_p0->type == TY_POLY && et != TY_POLY) {
        char elem_s[64];
        snprintf(elem_s, sizeof elem_s, "sp_%sArray_get(_t%d, _t%d)", k, trecv, ti);
        Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, et, elem_s, &bx);
        buf_printf(g_pre, "lv_%s = %s;\n", p0, bx.p ? bx.p : elem_s); free(bx.p);
      }
      else {
        buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
      }
    }
    int save_indent = g_indent; g_indent++;
    emit_loop_body(c, body, g_pre, g_indent);
    g_indent = save_indent;
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
    /* Restore types */
    if (p0_mismatch) { outer_p0->type = saved_p0_type; for (int j = 0; j < bn; j++) infer_type(c, bb[j]); }
    if (p1_mismatch) { outer_p1->type = saved_p1_type; for (int j = 0; j < bn; j++) infer_type(c, bb[j]); }
    g_indent--;
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  }
  else {
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
    emit_loop_body(c, body, g_pre, g_indent);
    g_indent = save_indent;

    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");

    /* Restore outer vars */
    if (p0 && ts_p0 > 0) {
      emit_indent(g_pre, g_indent); buf_printf(g_pre, "lv_%s = _t%d;\n", p0, ts_p0);
    }
    if (p1 && ts_p1 > 0) {
      emit_indent(g_pre, g_indent); buf_printf(g_pre, "lv_%s = _t%d;\n", p1, ts_p1);
    }
  }

  /* The expression evaluates to the accumulator */
  buf_printf(b, "_t%d", tacc);
  return 1;
}

/* True if `recv` names the constant `name` either as a bare ConstantReadNode
   or as a root-anchored absolute path `::name` (ConstantPathNode, no parent). */
int recv_is_const(const NodeTable *nt, int recv, const char *name) {
  if (recv < 0) return 0;
  const char *rty = nt_type(nt, recv);
  if (!rty) return 0;
  if (sp_streq(rty, "ConstantReadNode") ||
      (sp_streq(rty, "ConstantPathNode") && nt_ref(nt, recv, "parent") < 0)) {
    const char *n = nt_str(nt, recv, "name");
    return n && sp_streq(n, name);
  }
  return 0;
}

int sp_is_fiber_storage_recv(const NodeTable *nt, int recv) {
  if (recv < 0) return 0;
  const char *rty = nt_type(nt, recv);
  if (!rty) return 0;
  if (sp_streq(rty, "ConstantReadNode") ||
      (sp_streq(rty, "ConstantPathNode") && nt_ref(nt, recv, "parent") < 0)) {
    const char *rn = nt_str(nt, recv, "name");
    return rn && sp_streq(rn, "Fiber");
  }
  if (sp_streq(rty, "CallNode")) {
    const char *rn = nt_str(nt, recv, "name");
    int rr = nt_ref(nt, recv, "receiver");
    if (!rn || !sp_streq(rn, "current") || rr < 0) return 0;
    const char *rrty = nt_type(nt, rr);
    const char *rrn = nt_str(nt, rr, "name");
    return rrty && sp_streq(rrty, "ConstantReadNode") && rrn && sp_streq(rrn, "Fiber");
  }
  return 0;
}

/* `Klass.new(args) { block }` where Klass#initialize yields: the constructor
   only allocates (a yielding initialize is never emitted), so inline the
   initialize body at the call site with self bound to the fresh object and the
   literal block feeding its yields -- the same per-call-site specialization as
   ordinary yield-method inlining. Returns 1 if handled. */
