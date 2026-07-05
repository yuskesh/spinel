/* codegen_call_recv.c -- receiver-typed method-call emitters (array/hash/
   scalar/object/value/range/poly), split out of codegen_call.c. Pure code
   movement, no logic change. */

#include "codegen_internal.h"

/* Boxing function that lifts an array of kind `kk` ("Int"/"Str"/"Float"/"Poly")
   into a poly sp_RbVal. */
static const char *array_box_fn(const char *kk) {
  if (sp_streq(kk, "Int"))   return "sp_box_int_array";
  if (sp_streq(kk, "Str"))   return "sp_box_str_array";
  if (sp_streq(kk, "Float")) return "sp_box_float_array";
  return "sp_box_poly_array";
}

/* Emit the return value of an in-place filter mutator (select!/filter!/reject!/
   keep_if/delete_if) into `b`, after the compaction loop has run. Shared by the
   typed-array, poly-array, and hash bang handlers.
   Temps available:
     _t<trecv> : the (now-mutated) receiver (typed array/hash or poly array)
     _t<torig> : element count BEFORE compaction (mrb_int)
     _t<twp>   : element count AFTER compaction (mrb_int) == survivor count
   `boxed_self` is the receiver boxed into a poly sp_RbVal (e.g.
   "sp_box_int_array(_t4)" or "sp_box_obj(_t7, SP_BUILTIN_SYM_INT_HASH)").

   CRuby contract:
     - reject! / select! / filter!  ->  nil when nothing was removed, else self.
       These infer TY_POLY, so self must be boxed (a typed array/hash can't hold
       nil), hence the boxed ternary.
     - keep_if / delete_if          ->  always self, returned bare as the
       receiver type. */
static void emit_filter_bang_result(const char *name, int trecv, int torig,
                                    int twp, const char *boxed_self, Buf *b) {
  if (sp_streq(name, "reject!") || sp_streq(name, "select!") || sp_streq(name, "filter!"))
    buf_printf(b, "(_t%d != _t%d ? %s : sp_box_nil())", torig, twp, boxed_self);
  else
    buf_printf(b, "_t%d", trecv);  /* keep_if / delete_if: always self */
}

int emit_array_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  TyKind res = comp_ntype(c, id);
  /* Homogeneous object array (sp_PtrArray of unboxed sp_X*), produced by the
     post-fixpoint narrow_object_arrays pass. Indexing yields a typed `sp_X *`
     directly -- no sp_RbVal box, no cls-id dispatch. Only the op set the pass
     admits reaches here; the pass and this block stay in lockstep. */
  if (recv >= 0 && ty_is_obj_array(rt)) {
    int ecls = ty_obj_array_class(rt);
    const char *ecn = c->classes[ecls].name;
    if ((sp_streq(name, "[]") || sp_streq(name, "at")) && argc == 1) {
      buf_printf(b, "((sp_%s *)sp_PtrArray_get(", ecn);
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, "))");
      return 1;
    }
    if ((sp_streq(name, "first") || sp_streq(name, "last")) && argc == 0) {
      buf_printf(b, "((sp_%s *)sp_PtrArray_get(", ecn);
      emit_expr(c, recv, b);
      buf_puts(b, sp_streq(name, "first") ? ", 0))" : ", -1))");
      return 1;
    }
    if (sp_streq(name, "[]=") && argc == 2) {
      int tv = ++g_tmp;
      buf_printf(b, "({ sp_%s *_t%d = ", ecn, tv); emit_expr(c, argv[1], b);
      buf_puts(b, "; sp_PtrArray_set("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_int_expr(c, argv[0], b); buf_printf(b, ", _t%d); _t%d; })", tv, tv);
      return 1;
    }
    if ((sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "append")) && argc >= 1) {
      int tr = ++g_tmp;
      buf_printf(b, "({ sp_PtrArray *_t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, ";");
      for (int a = 0; a < argc; a++) {
        buf_printf(b, " sp_PtrArray_push(_t%d, ", tr); emit_expr(c, argv[a], b); buf_puts(b, ");");
      }
      buf_printf(b, " _t%d; })", tr);
      return 1;
    }
    if ((sp_streq(name, "length") || sp_streq(name, "size")) && argc == 0) {
      buf_puts(b, "sp_PtrArray_length("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "empty?") && argc == 0) {
      buf_puts(b, "sp_PtrArray_empty("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    /* no-block comparisons via the boxed comparator (user `<=>` through the
       cmp hook); the narrowing pass admits these only when the element class
       has `<=>` and (for sort) the result lands in a modeled consumer. */
    if (sp_streq(name, "sort") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      buf_puts(b, "sp_PtrArray_sort_obj("); emit_expr(c, recv, b);
      buf_printf(b, ", %d)", ecls);
      return 1;
    }
    if (sp_streq(name, "sort!") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      int tr = ++g_tmp;
      buf_printf(b, "({ sp_PtrArray *_t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; sp_PtrArray_sort_obj_bang(_t%d, %d); _t%d; })", tr, ecls, tr);
      return 1;
    }
    if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc == 0 &&
        nt_ref(nt, id, "block") < 0) {
      buf_printf(b, "((sp_%s *)sp_PtrArray_minmax_obj(", ecn);
      emit_expr(c, recv, b);
      buf_printf(b, ", %d, %d))", ecls, sp_streq(name, "max") ? 1 : 0);
      return 1;
    }
    return 0;  /* unsupported obj-array op: pass should have prevented this. */
  }
  if (recv >= 0 && ty_is_array(rt)) {
    if (sp_streq(name, "pack") && argc == 1 && (rt == TY_INT_ARRAY || rt == TY_POLY_ARRAY)) {
      buf_printf(b, "sp_%sArray_pack(", rt == TY_POLY_ARRAY ? "Poly" : "Int");
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* values_at(i, j, ...) -> fresh same-kind array of the picked elements
       (works for typed and poly arrays alike, and range args) */
    if (sp_streq(name, "values_at") && argc >= 1) {
      const char *an = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
      if (an) {
        int tr = ++g_tmp, to = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", an, tr); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray *_t%d = sp_%sArray_new(); ", an, to, an);
        for (int a = 0; a < argc; a++) {
          TyKind at = comp_ntype(c, argv[a]);
          if (at == TY_RANGE) {
            int trng = ++g_tmp, ti = ++g_tmp;
            buf_printf(b, "{ sp_Range _t%d = ", trng); emit_expr(c, argv[a], b);
            buf_printf(b, "; for (mrb_int _t%d = _t%d.first; _t%d <= _t%d.last - _t%d.excl; _t%d++)"
                          " sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d)); } ",
                       ti, trng, ti, trng, trng, ti, an, to, an, tr, ti);
          }
          else {
            buf_printf(b, "sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, ", an, to, an, tr);
            emit_expr(c, argv[a], b); buf_puts(b, ")); ");
          }
        }
        buf_printf(b, "_t%d; })", to);
        return 1;
      }
    }
    const char *k = array_kind(rt);
    /* drop(n) / take(n): subarrays via slice (all kinds incl. poly). */
    if ((sp_streq(name, "drop") || sp_streq(name, "take")) && argc == 1) {
      const char *dk = (rt == TY_POLY_ARRAY) ? "Poly" : k;
      if (dk) {
        int t = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", dk, t); emit_expr(c, recv, b);
        buf_printf(b, "; mrb_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
        if (sp_streq(name, "take"))
          buf_printf(b, "; sp_%sArray_slice(_t%d, 0, _t%d); })", dk, t, tn);
        else
          buf_printf(b, "; sp_%sArray_slice(_t%d, _t%d, _t%d->len - _t%d); })", dk, t, tn, t, tn);
        return 1;
      }
    }
    /* poly-array max/min: boxed elements compared at runtime (numerics,
       strings, int-array tuples lexicographically). */
    if ((sp_streq(name, "max") || sp_streq(name, "min")) && argc == 0 &&
        rt == TY_POLY_ARRAY && nt_ref(nt, id, "block") < 0) {
      buf_printf(b, "sp_PolyArray_%s(", name); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    /* fill(val[, start[, len]]): fill a range with val, evaluate to self. */
    if (sp_streq(name, "fill") && argc >= 1 && argc <= 3) {
      const char *fk = (rt == TY_POLY_ARRAY) ? "Poly" : k;
      if (fk) {
        int t = ++g_tmp, ti = ++g_tmp, tv = ++g_tmp, tn = ++g_tmp, ts = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", fk, t); emit_expr(c, recv, b); buf_puts(b, "; ");
        emit_ctype(c, ty_array_elem(rt), b); buf_printf(b, " _t%d = ", tv);
        if (rt == TY_POLY_ARRAY) emit_boxed(c, argv[0], b); else emit_expr(c, argv[0], b);
        buf_printf(b, "; mrb_int _t%d = sp_%sArray_length(_t%d);", tn, fk, t);
        if (argc >= 2 && comp_ntype(c, argv[1]) == TY_RANGE) {
          /* fill(val, range): use range as index span */
          int tr = ++g_tmp, te = ++g_tmp;
          buf_printf(b, " sp_Range _t%d = ", tr); emit_expr(c, argv[1], b);
          buf_printf(b, "; mrb_int _t%d = _t%d.first; if (_t%d < 0) _t%d += _t%d; if (_t%d < 0) _t%d = 0;",
                     ts, tr, ts, ts, tn, ts, ts);
          buf_printf(b, " mrb_int _t%d = _t%d.last - _t%d.excl;", te, tr, tr);
          buf_printf(b, " for (mrb_int _t%d = _t%d; _t%d <= _t%d; _t%d++)"
                        " sp_%sArray_set(_t%d, _t%d, _t%d); _t%d; })",
                     ti, ts, ti, te, ti, fk, t, ti, tv, t);
        }
        else if (argc >= 2) {
          buf_printf(b, " mrb_int _t%d = ", ts); emit_int_expr(c, argv[1], b);
          buf_printf(b, "; if (_t%d < 0) _t%d += _t%d; if (_t%d < 0) _t%d = 0;", ts, ts, tn, ts, ts);
          if (argc == 3) {
            int tl = ++g_tmp;
            buf_printf(b, " mrb_int _t%d = ", tl); emit_int_expr(c, argv[2], b);
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
        return 1;
      }
    }
    if (rt == TY_POLY_ARRAY && sp_streq(name, "sum") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      buf_puts(b, "sp_box_int(sp_PolyArray_sum_int("); emit_expr(c, recv, b); buf_puts(b, "))");
      return 1;
    }
    if (rt == TY_POLY_ARRAY && sp_streq(name, "sum") && argc == 1 && nt_ref(nt, id, "block") < 0) {
      TyKind init_t = comp_ntype(c, argv[0]);
      buf_puts(b, "sp_box_int(");
      if (init_t == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else { emit_expr(c, argv[0], b); }
      buf_puts(b, " + sp_PolyArray_sum_int("); emit_expr(c, recv, b); buf_puts(b, "))");
      return 1;
    }
    if (rt == TY_POLY_ARRAY && (sp_streq(name, "shift") || sp_streq(name, "pop")) && argc == 0) {
      buf_printf(b, "sp_PolyArray_%s(", name); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    if (rt == TY_POLY_ARRAY && sp_streq(name, "dig") && argc >= 1) {
      if (argc == 1) {
        buf_puts(b, "sp_PolyArray_get("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else {
        for (int di = argc - 1; di >= 1; di--) buf_printf(b, "sp_poly_arr_get_hash(");
        buf_puts(b, "sp_PolyArray_get("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        for (int di = 1; di < argc; di++) { buf_puts(b, ", "); emit_expr(c, argv[di], b); buf_puts(b, ")"); }
      }
      return 1;
    }
    /* each_index { |i| ... } - iterate with index (works for all array kinds) */
    {
      int ei_blk = nt_ref(nt, id, "block");
      if (sp_streq(name, "each_index") && ei_blk >= 0) {
        const char *ek = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
        if (ek) {
          const char *ip = block_param_name(c, ei_blk, 0); if (ip) ip = rename_local(ip);
          int body = nt_ref(nt, ei_blk, "body");
          int trecv = ++g_tmp, ti = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, ek, trecv, ti);
          if (ip) {
            Scope *eic = comp_scope_of(c, ei_blk);
            LocalVar *eilv = eic ? scope_local(eic, ip) : NULL;
            TyKind eit = eilv ? eilv->type : TY_INT;
            emit_indent(g_pre, g_indent + 1);
            if (eit == TY_POLY)
              buf_printf(g_pre, "lv_%s = sp_box_int(_t%d);\n", ip, ti);
            else
              buf_printf(g_pre, "lv_%s = _t%d;\n", ip, ti);
          }
          emit_stmts(c, body, g_pre, g_indent + 1);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", trecv); return 1;
        }
      }
    }
    /* take_while / drop_while (works for typed and poly arrays alike) */
    if ((sp_streq(name, "take_while") || sp_streq(name, "drop_while")) && argc == 0
        && nt_ref(nt, id, "block") >= 0) {
      int is_drop = sp_streq(name, "drop_while");
      int tw_blk = nt_ref(nt, id, "block");
      const char *tw_bp = block_param_name(c, tw_blk, 0); if (tw_bp) tw_bp = rename_local(tw_bp);
      int tw_body = nt_ref(nt, tw_blk, "body");
      int tw_bn = 0; const int *tw_bb = tw_body >= 0 ? nt_arr(nt, tw_body, "body", &tw_bn) : NULL;
      if (tw_bn > 0) {
        const char *ek = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
        if (ek) {
          TyKind et = ty_array_elem(rt);
          int trecv = ++g_tmp, tout = ++g_tmp, ti = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", ek, tout, ek);
          if (is_drop) {
            emit_indent(g_pre, g_indent);
            buf_puts(g_pre, "{ mrb_bool _dropping = 1;\n");
          }
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, ek, trecv, ti);
          if (tw_bp) {
            emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre);
            buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", tw_bp, ek, trecv, ti);
          }
          for (int j = 0; j < tw_bn - 1; j++) emit_stmt(c, tw_bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent = g_indent + 1;
          Buf cb = expr_buf(c, tw_bb[tw_bn - 1]); g_indent = sv;
          if (is_drop) {
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "if (_dropping && !(%s)) _dropping = 0;\n", cb.p ? cb.p : "0");
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "if (!_dropping) sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d));\n",
                       ek, tout, ek, trecv, ti);
          }
          else {
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "if (!(%s)) break;\n", cb.p ? cb.p : "0");
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d));\n",
                       ek, tout, ek, trecv, ti);
          }
          free(cb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          if (is_drop) { emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n"); }
          buf_printf(b, "_t%d", tout); return 1;
        }
      }
    }
    if (rt == TY_POLY_ARRAY && sp_streq(name, "tally") && argc == 0) {
      buf_puts(b, "sp_PolyArray_tally("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    if (rt == TY_POLY_ARRAY && sp_streq(name, "delete_at") && argc == 1) {
      buf_puts(b, "sp_PolyArray_delete_at("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* Array#delete(v) (value-based, not index-based) on TY_POLY_ARRAY --
       same array_kind()==NULL gating gap as delete_at above. The typed
       forms live inside the `if (k)` block below; poly arrays never get
       there. Needed for the array-backed Set package's #delete, whose
       @data widens to poly for mixed-element sets. */
    if (rt == TY_POLY_ARRAY && sp_streq(name, "delete") && argc == 1) {
      buf_puts(b, "sp_PolyArray_delete("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* find / detect { |x| cond } on a poly array -> the element or nil. The
       typed-array forms live inside the `if (k)` block below, but array_kind is
       NULL for a poly array, so handle it here with the boxed element type. */
    if (rt == TY_POLY_ARRAY && (sp_streq(name, "find") || sp_streq(name, "detect"))) {
      int fblock = nt_ref(nt, id, "block");
      if (fblock >= 0) {
        const char *bp = block_param_name(c, fblock, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, fblock, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s; SP_GC_ROOT(_t%d);\n", trecv, rb.p ? rb.p : "", trecv); free(rb.p);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", tres, default_value(TY_POLY), tres);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n", ti, ti, trecv, ti);
          /* Declare the block param in the loop body so the form is self-contained
             when this find is a parameter default hoisted to the call site (whose
             function has no top-level declaration for the block local). */
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf cb; memset(&cb, 0, sizeof cb); emit_cond(c, bb[bn - 1], &cb); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          if (bp) buf_printf(g_pre, "if (%s) { _t%d = lv_%s; break; }\n", cb.p ? cb.p : "0", tres, bp);
          else buf_printf(g_pre, "if (%s) { _t%d = sp_PolyArray_get(_t%d, _t%d); break; }\n", cb.p ? cb.p : "0", tres, trecv, ti);
          free(cb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tres); return 1;
        }
      }
    }
    /* find_index / index { |x| cond } on a poly array -> the index or nil.
       The typed-array form lives inside the `if (k)` block below; array_kind
       is NULL for a poly array, so handle it here. Unlike the int/str-array
       forms (which infer TY_POLY and box), index/find_index on a poly array
       infer as a plain nullable mrb_int -- return the bare SP_INT_NIL
       sentinel, don't box. */
    if (rt == TY_POLY_ARRAY && (sp_streq(name, "find_index") || sp_streq(name, "index")) &&
        nt_ref(nt, id, "block") >= 0) {
      int fblock = nt_ref(nt, id, "block");
      const char *bp = block_param_name(c, fblock, 0); if (bp) bp = rename_local(bp);
      int body = nt_ref(nt, fblock, "body");
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      if (bn >= 1) {
        int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp;
        Buf rb = expr_buf(c, recv);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_PolyArray *_t%d = %s; SP_GC_ROOT(_t%d);\n", trecv, rb.p ? rb.p : "NULL", trecv); free(rb.p);
        emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = SP_INT_NIL;\n", tres);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n",
                   ti, ti, trecv, ti);
        /* Declare the block param in the loop body so the form is self-contained
           (same rationale as find/detect above). */
        if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
        for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
        int sv = g_indent; g_indent++;
        Buf cb; memset(&cb, 0, sizeof cb); emit_cond(c, bb[bn - 1], &cb); g_indent = sv;
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "if (%s) { _t%d = _t%d; break; }\n", cb.p ? cb.p : "0", tres, ti);
        free(cb.p);
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        buf_printf(b, "_t%d", tres);
        return 1;
      }
    }
    /* index(v) / find_index(v) on a poly array (no block) -> the first
       position whose element == v (sp_poly_eq), or nil (SP_INT_NIL),
       mirroring the count(v)/any?(v) idiom (doom: @map.sectors.index(sector)). */
    if (rt == TY_POLY_ARRAY && (sp_streq(name, "index") || sp_streq(name, "find_index")) &&
        argc == 1 && nt_ref(nt, id, "block") < 0) {
      int trecv = ++g_tmp, ta = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;
      Buf ra = expr_buf(c, recv);
      /* Root the receiver and the boxed needle: sp_poly_eq can allocate
         (bigint promotion), so a collection may run mid-loop. */
      buf_printf(b, "({ sp_PolyArray *_t%d = %s; SP_GC_ROOT(_t%d);", trecv, ra.p ? ra.p : "NULL", trecv); free(ra.p);
      buf_printf(b, " sp_RbVal _t%d = ", ta); emit_boxed(c, argv[0], b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", ta);
      buf_printf(b, " mrb_int _t%d = SP_INT_NIL;", tres);
      buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)", ti, ti, trecv, ti);
      buf_printf(b, " if (sp_poly_eq(sp_PolyArray_get(_t%d, _t%d), _t%d)) { _t%d = _t%d; break; }",
                 trecv, ti, ta, tres, ti);
      buf_printf(b, " _t%d; })", tres);
      return 1;
    }
    if (k) {
      if ((sp_streq(name, "to_a") || sp_streq(name, "to_ary") || sp_streq(name, "entries") ||
           sp_streq(name, "flatten") || sp_streq(name, "compact")) && argc == 0) {
        /* a scalar-element array can't nest or hold nil: these are identity */
        emit_expr(c, recv, b); return 1;
      }
      if (sp_streq(name, "[]") && argc == 1 && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode")) {
        /* arr[a..b] / arr[a...b] -> subarray */
        int rn = argv[0];
        int excl = (int)(nt_int(nt, rn, "flags", 0) & 4) ? 1 : 0;
        int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
        buf_printf(b, "sp_%sArray_slice_range(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (lo >= 0) emit_expr(c, lo, b); else buf_puts(b, "0");
        buf_puts(b, ", ");
        if (hi >= 0) emit_expr(c, hi, b); else buf_puts(b, "-1");
        buf_printf(b, ", %d)", hi >= 0 ? excl : 0);
        return 1;
      }
      if (sp_streq(name, "[]") && argc == 2) {
        /* arr[start, len] -> subarray */
        buf_printf(b, "sp_%sArray_slice(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "[]") || sp_streq(name, "at")) && argc == 1) {
        buf_printf(b, "sp_%sArray_get(", k);
        emit_expr(c, recv, b); buf_puts(b, ", ");
        if (infer_type(c, argv[0]) == TY_POLY) {
          int t = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", t);
          emit_expr(c, argv[0], b);
          buf_printf(b, "; _t%d.v.i; })", t);
        }
else {
          emit_expr(c, argv[0], b);
        }
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "fetch") && (argc == 1 || argc == 2)) {
        int ta = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp, tnorm = ++g_tmp;
        Buf ra = expr_buf(c, recv);
        buf_printf(b, "({ sp_%sArray *_t%d = %s;", k, ta, ra.p ? ra.p : "NULL"); free(ra.p);
        buf_printf(b, " mrb_int _t%d = ", ti); emit_int_expr(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " mrb_int _t%d = sp_%sArray_length(_t%d);", tn, k, ta);
        buf_printf(b, " mrb_int _t%d = _t%d < 0 ? _t%d + _t%d : _t%d;", tnorm, ti, ti, tn, ti);
        buf_printf(b, " (_t%d >= 0 && _t%d < _t%d) ? sp_%sArray_get(_t%d, _t%d) :", tnorm, tnorm, tn, k, ta, tnorm);
        if (argc == 2) {
          buf_puts(b, " "); emit_expr(c, argv[1], b); buf_puts(b, "; })");
        }
        else {
          buf_printf(b, " (sp_raise_cls(\"IndexError\", \"index out of bounds\"), %s); })",
                     default_value(ty_array_elem(rt)));
        }
        return 1;
      }
      if (sp_streq(name, "dig") && argc >= 1) {
        if (argc == 1) {
          /* single-step: same as arr[i] */
          buf_printf(b, "sp_%sArray_get(", k); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else {
          /* multi-step: box the array as sp_RbVal, then chain sp_poly_arr_get */
          buf_printf(b, "sp_poly_arr_get_hash(");
          /* first step: box the typed array as obj, then get element i */
          int is_int = (rt == TY_INT_ARRAY);
          (void)is_int;
          /* build chain from innermost outward */
          for (int di = argc - 1; di >= 1; di--) {
            buf_printf(b, "sp_poly_arr_get_hash(");
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
        return 1;
      }
      if (sp_streq(name, "+") && argc == 1 && a0 == rt) {
        /* array + array of the same kind -> a fresh concatenation */
        buf_printf(b, "sp_%sArray_concat(", k);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "+") && argc == 1 && ty_is_array(a0) && a0 != rt) {
        /* array + different-kind array -> poly_array */
        const char *k2 = (a0 == TY_POLY_ARRAY) ? "Poly" : array_kind(a0);
        if (k2) {
          int tL = ++g_tmp, tR = ++g_tmp, tO = ++g_tmp, ti = ++g_tmp;
          Buf lbuf = expr_buf(c, recv);
          Buf rbuf = expr_buf(c, argv[0]);
          const char *box_l = (rt == TY_INT_ARRAY) ? "sp_box_int" :
                              (rt == TY_FLOAT_ARRAY) ? "sp_box_float" :
                              (rt == TY_STR_ARRAY) ? "sp_box_str" : NULL;
          const char *box_r = (a0 == TY_INT_ARRAY) ? "sp_box_int" :
                              (a0 == TY_FLOAT_ARRAY) ? "sp_box_float" :
                              (a0 == TY_STR_ARRAY) ? "sp_box_str" : NULL;
          const char *get_l = (rt == TY_POLY_ARRAY) ? "sp_PolyArray_get" :
                              NULL;
          const char *get_r = (a0 == TY_POLY_ARRAY) ? "sp_PolyArray_get" :
                              NULL;
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_%sArray *_t%d = %s;\n", k, tL, lbuf.p ? lbuf.p : ""); free(lbuf.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_%sArray *_t%d = %s;\n", k2, tR, rbuf.p ? rbuf.p : ""); free(rbuf.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tO, tO);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++)\n", ti, ti, k, tL, ti);
          emit_indent(g_pre, g_indent + 1);
          if (rt == TY_POLY_ARRAY)
            buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));\n", tO, tL, ti);
          else if (box_l)
            buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s(sp_%sArray_get(_t%d, _t%d)));\n", tO, box_l, k, tL, ti);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++)\n", ti, ti, k2, tR, ti);
          emit_indent(g_pre, g_indent + 1);
          if (a0 == TY_POLY_ARRAY)
            buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));\n", tO, tR, ti);
          else if (box_r)
            buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s(sp_%sArray_get(_t%d, _t%d)));\n", tO, box_r, k2, tR, ti);
          buf_printf(b, "_t%d", tO);
          (void)get_l; (void)get_r;
          return 1;
        }
      }
      if (sp_streq(name, "clear") && argc == 0) {
        /* empty the array in place, evaluate to it (Ruby returns self) */
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; if (_t%d) _t%d->len = 0; _t%d; })", t, t, t);
        return 1;
      }
      if ((sp_streq(name, "shift") || sp_streq(name, "pop")) && argc == 0) {
        /* remove and return first/last element (nil sentinel when empty) */
        buf_printf(b, "sp_%sArray_%s(", k, name); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "unshift") && argc >= 1) {
        int t = ++g_tmp;
        if (rt == TY_INT_ARRAY) {
          buf_printf(b, "({ sp_IntArray *_t%d = ", t); emit_expr(c, recv, b); buf_puts(b, ";");
          for (int a = argc - 1; a >= 0; a--) {
            buf_printf(b, " sp_IntArray_unshift(_t%d, ", t); emit_expr(c, argv[a], b); buf_puts(b, ");");
          }
        }
        else if (rt == TY_STR_ARRAY) {
          buf_printf(b, "({ sp_StrArray *_t%d = ", t); emit_expr(c, recv, b); buf_puts(b, ";");
          for (int a = 0; a < argc; a++) {
            buf_printf(b, " sp_StrArray_insert(_t%d, %d, ", t, a); emit_expr(c, argv[a], b); buf_puts(b, ");");
          }
        }
        else {
          /* FloatArray (the only other element kind that reaches this typed
             dispatch; poly arrays route elsewhere). Evaluate the arguments
             left to right into temporaries (Ruby's argument-evaluation order),
             then prepend them in reverse so a multi-arg unshift keeps order. */
          buf_printf(b, "({ sp_FloatArray *_t%d = ", t); emit_expr(c, recv, b); buf_puts(b, ";");
          for (int a = 0; a < argc; a++) {
            buf_printf(b, " mrb_float _u%d_%d = ", t, a); emit_float_expr(c, argv[a], b); buf_puts(b, ";");
          }
          for (int a = argc - 1; a >= 0; a--) {
            buf_printf(b, " sp_FloatArray_unshift(_t%d, _u%d_%d);", t, t, a);
          }
        }
        buf_printf(b, " _t%d; })", t);
        return 1;
      }
      /* non-mutating copy-then-operate methods */
      if (sp_streq(name, "shuffle") && argc == 0) {
        buf_printf(b, "sp_%sArray_shuffle(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      /* in-place mutators that return self (raise FrozenError when frozen) */
      {
        const char *base = NULL;
        if      (sp_streq(name, "reverse!")) base = "reverse_bang";
        else if (sp_streq(name, "sort!"))    base = "sort_bang";
        else if (sp_streq(name, "shuffle!")) base = "shuffle_bang";
        else if (sp_streq(name, "uniq!"))    base = "uniq_bang";
        if (base && argc == 0) {
          int t = ++g_tmp;
          buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
          buf_printf(b, "; sp_%sArray_%s(_t%d); _t%d; })", k, base, t, t);
          return 1;
        }
      }
      if ((sp_streq(name, "dup") || sp_streq(name, "clone")) && argc == 0) {
        /* a real copy: arrays are mutable, so dup/clone must not alias. */
        buf_printf(b, "sp_%sArray_dup(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "reverse") && argc == 0) {
        /* copy + reverse in place; sp_*Array_dup exists for Int/Str/Float/Poly */
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = sp_%sArray_dup(", k, t, k); emit_expr(c, recv, b);
        buf_printf(b, "); sp_%sArray_reverse_bang(_t%d); _t%d; })", k, t, t);
        return 1;
      }
      if (sp_streq(name, "zip") && argc >= 1 && nt_ref(nt, id, "block") < 0) {
        /* recv.zip(b, c...) → [[recv[0],b[0],c[0],...], ...] as PolyArray of PolyArrays */
        int ta = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tpair = ++g_tmp;
        int tb[16]; TyKind at[16]; int nargs = argc < 16 ? argc : 16;
        for (int j = 0; j < nargs; j++) { tb[j] = ++g_tmp; at[j] = comp_ntype(c, argv[j]); }
        const char *ka = (rt == TY_POLY_ARRAY) ? "Poly" : k;
        buf_printf(b, "({ sp_%sArray *_t%d = ", ka, ta); emit_expr(c, recv, b); buf_puts(b, ";");
        for (int j = 0; j < nargs; j++) {
          const char *kj = (at[j] == TY_POLY_ARRAY) ? "Poly" : (array_kind(at[j]) ? array_kind(at[j]) : "Poly");
          buf_printf(b, " sp_%sArray *_t%d = ", kj, tb[j]); emit_expr(c, argv[j], b); buf_puts(b, ";");
        }
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {",
                   ti, ti, ka, ta, ti);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new();", tpair);
        if (rt == TY_INT_ARRAY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_IntArray_get(_t%d, _t%d)));", tpair, ta, ti);
        else if (rt == TY_STR_ARRAY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_StrArray_get(_t%d, _t%d)));", tpair, ta, ti);
        else if (rt == TY_FLOAT_ARRAY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_float(sp_FloatArray_get(_t%d, _t%d)));", tpair, ta, ti);
        else
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, ta, ti);
        for (int j = 0; j < nargs; j++) {
          if (at[j] == TY_INT_ARRAY)
            buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_IntArray_get(_t%d, _t%d)));", tpair, tb[j], ti);
          else if (at[j] == TY_STR_ARRAY)
            buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_StrArray_get(_t%d, _t%d)));", tpair, tb[j], ti);
          else if (at[j] == TY_FLOAT_ARRAY)
            buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_float(sp_FloatArray_get(_t%d, _t%d)));", tpair, tb[j], ti);
          else
            buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, tb[j], ti);
        }
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));", tr, tpair);
        buf_printf(b, " } _t%d; })", tr);
        return 1;
      }
      if (sp_streq(name, "product") && argc == 1) {
        TyKind at = comp_ntype(c, argv[0]);
        const char *kb = (at == TY_POLY_ARRAY) ? "Poly" : (array_kind(at) ? array_kind(at) : "Poly");
        int ta = ++g_tmp, tb = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp, tpair = ++g_tmp;
        Buf ra; memset(&ra, 0, sizeof ra); Buf rb2; memset(&rb2, 0, sizeof rb2);
        emit_expr(c, recv, &ra); emit_expr(c, argv[0], &rb2);
        buf_printf(b, "({ sp_%sArray *_t%d = %s; sp_%sArray *_t%d = %s;",
                   k, ta, ra.p ? ra.p : "NULL", kb, tb, rb2.p ? rb2.p : "NULL");
        free(ra.p); free(rb2.p);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
        buf_printf(b, " sp_PolyArray *_t%d = NULL;", tpair);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {", ti, ti, k, ta, ti);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {", tj, tj, kb, tb, tj);
        buf_printf(b, " _t%d = sp_PolyArray_new();", tpair);
        if (rt == TY_INT_ARRAY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_IntArray_get(_t%d, _t%d)));", tpair, ta, ti);
        else if (rt == TY_STR_ARRAY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_StrArray_get(_t%d, _t%d)));", tpair, ta, ti);
        else if (rt == TY_FLOAT_ARRAY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_float(sp_FloatArray_get(_t%d, _t%d)));", tpair, ta, ti);
        else
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, ta, ti);
        if (at == TY_INT_ARRAY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_IntArray_get(_t%d, _t%d)));", tpair, tb, tj);
        else if (at == TY_STR_ARRAY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_StrArray_get(_t%d, _t%d)));", tpair, tb, tj);
        else if (at == TY_FLOAT_ARRAY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_float(sp_FloatArray_get(_t%d, _t%d)));", tpair, tb, tj);
        else
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, tb, tj);
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));", tr, tpair);
        buf_printf(b, " } } _t%d; })", tr);
        return 1;
      }
      if (sp_streq(name, "repeated_combination") && argc == 1 && rt == TY_INT_ARRAY) {
        int ta = ++g_tmp, tc = ++g_tmp, tout = ++g_tmp, ti = ++g_tmp;
        Buf ra = expr_buf(c, recv);
        buf_printf(b, "({ sp_IntArray *_t%d = %s;", ta, ra.p ? ra.p : "NULL"); free(ra.p);
        buf_printf(b, " sp_PtrArray *_t%d = sp_IntArray_repeated_combination(_t%d, ", tc, ta);
        emit_expr(c, argv[0], b);
        buf_printf(b, "); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tout, tout);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++)", ti, ti, tc, ti);
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int_array(_t%d->data[_t%d]));", tout, tc, ti);
        buf_printf(b, " _t%d; })", tout);
        return 1;
      }
      if (sp_streq(name, "rotate!") && argc <= 1) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray_rotate_bang(_t%d, ", k, t);
        if (argc == 1) emit_expr(c, argv[0], b); else buf_puts(b, "1");
        buf_printf(b, "); _t%d; })", t);
        return 1;
      }
      if (sp_streq(name, "replace") && argc == 1 && a0 == rt) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray_replace(_t%d, ", k, t); emit_expr(c, argv[0], b);
        buf_printf(b, "); _t%d; })", t);
        return 1;
      }
      if (sp_streq(name, "insert") && argc == 2 && (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY)) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray_insert(_t%d, ", k, t); emit_expr(c, argv[0], b);
        buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_printf(b, "); _t%d; })", t);
        return 1;
      }
      if (sp_streq(name, "delete_at") && argc == 1) {
        buf_printf(b, "sp_%sArray_delete_at(", k); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "delete") && argc == 1 && (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY)) {
        buf_printf(b, "sp_%sArray_delete(", k); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "tally") && argc == 0) {
        if (rt == TY_INT_ARRAY) { buf_printf(b, "sp_IntArray_tally_int("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
        if (rt == TY_STR_ARRAY) { buf_printf(b, "sp_StrArray_tally("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
        if (rt == TY_POLY_ARRAY) { buf_printf(b, "sp_PolyArray_tally("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      }
      if (sp_streq(name, "slice!") && argc == 2) {
        /* slice!(start, len): remove and return the subarray (raises
           FrozenError inside the runtime helper when the array is frozen) */
        buf_printf(b, "sp_%sArray_slice_bang(", k); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
        return 1;
      }
      int block = nt_ref(nt, id, "block");
      /* bsearch { |x| cond } on typed arrays - find-minimum mode */
      if (sp_streq(name, "bsearch") && block >= 0) {
        const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          TyKind et = ty_array_elem(rt);
          int trecv = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp, tres = ++g_tmp, tmid = ++g_tmp;
          Buf rbs = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rbs.p ? rbs.p : "NULL"); free(rbs.p);
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
          Buf cb = expr_buf(c, bb[bn - 1]); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "if (%s) { _t%d = sp_%sArray_get(_t%d, _t%d); _t%d = _t%d - 1; }\n",
                     cb.p ? cb.p : "0", tres, k, trecv, tmid, thi, tmid);
          free(cb.p);
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d = _t%d + 1; }\n", tlo, tmid);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tres); return 1;
        }
      }
      /* bsearch_index { |x| cond }: find-minimum binary search returning the
         index of the first element satisfying the predicate, or nil. */
      if (sp_streq(name, "bsearch_index") && block >= 0) {
        const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          int trecv = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp, tres = ++g_tmp, tmid = ++g_tmp;
          Buf rbs = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rbs.p ? rbs.p : "NULL"); free(rbs.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = 0, _t%d = sp_%sArray_length(_t%d) - 1, _t%d = SP_INT_NIL;\n", tlo, thi, k, trecv, tres);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "while (_t%d <= _t%d) {\n", tlo, thi);
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "mrb_int _t%d = _t%d + (_t%d - _t%d) / 2;\n", tmid, tlo, thi, tlo);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, tmid); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          /* The block value is the search predicate: route through emit_cond so a
             poly / nullable-scalar result becomes a valid C truthiness test rather
             than `if (sp_RbVal)` or `if (SP_INT_NIL)`. */
          Buf cb; memset(&cb, 0, sizeof cb); emit_cond(c, bb[bn - 1], &cb); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "if (%s) { _t%d = _t%d; _t%d = _t%d - 1; }\n", cb.p ? cb.p : "0", tres, tmid, thi, tmid);
          free(cb.p);
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d = _t%d + 1; }\n", tlo, tmid);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tres); return 1;
        }
      }
      /* find_index { |x| cond } / index { |x| cond } on typed arrays - returns
         the index or nil (`index` is an alias for `find_index` in this form). */
      if ((sp_streq(name, "find_index") || sp_streq(name, "index")) && block >= 0) {
        const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp;
          Buf rfi = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rfi.p ? rfi.p : "NULL"); free(rfi.p);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = SP_INT_NIL;\n", tres);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, k, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf cb = expr_buf(c, bb[bn - 1]); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "if (%s) { _t%d = _t%d; break; }\n", cb.p ? cb.p : "0", tres, ti);
          free(cb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "(_t%d == SP_INT_NIL ? sp_box_nil() : sp_box_int(_t%d))", tres, tres);
          return 1;
        }
      }
      /* find / detect { |x| cond } - returns element or nil */
      if ((sp_streq(name, "find") || sp_streq(name, "detect")) && block >= 0) {
        const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          TyKind et = ty_array_elem(rt);
          int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre);
          if (et == TY_STRING) buf_printf(g_pre, " _t%d = NULL;\n", tres);
          else if (et == TY_INT) buf_printf(g_pre, " _t%d = SP_INT_NIL;\n", tres);
          else buf_printf(g_pre, " _t%d = 0;\n", tres);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, k, trecv, ti);
          /* Declare the block param in the loop body (not a bare assignment) so
             the find is self-contained: when this call is a parameter default
             hoisted to the call site, the enclosing function has no top-level
             declaration for the block local. Shadows the method-scope slot in
             the ordinary in-body case, which is harmless. */
          if (bp) { emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf cb = expr_buf(c, bb[bn - 1]); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          if (bp) buf_printf(g_pre, "if (%s) { _t%d = lv_%s; break; }\n", cb.p ? cb.p : "0", tres, bp);
          else buf_printf(g_pre, "if (%s) { _t%d = sp_%sArray_get(_t%d, _t%d); break; }\n",
                          cb.p ? cb.p : "0", tres, k, trecv, ti);
          free(cb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tres); return 1;
        }
      }
      /* map! / collect! { |x| body } - in-place transform, returns receiver */
      if ((sp_streq(name, "map!") || sp_streq(name, "collect!")) && block >= 0) {
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
          Buf rb = expr_buf(c, recv);
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
          Buf vb = expr_buf(c, bb[bn - 1]); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "sp_%sArray_set(_t%d, _t%d, %s);\n", k, trecv, ti, vb.p ? vb.p : "0");
          free(vb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          if (mlv) mlv->type = msaved;
          buf_printf(b, "_t%d", trecv); return 1;
        }
      }
      /* select! / filter! / keep_if / reject! / delete_if { |x| cond } - in-place
         filter. Works on typed (int/str/float) AND poly arrays. */
      if ((sp_streq(name, "select!") || sp_streq(name, "filter!") || sp_streq(name, "keep_if") ||
           sp_streq(name, "reject!") || sp_streq(name, "delete_if")) && block >= 0) {
        int is_rej = sp_streq(name, "reject!") || sp_streq(name, "delete_if");
        const char *kk = (rt == TY_POLY_ARRAY) ? "Poly" : k;
        const char *bp0 = block_param_name(c, block, 0);
        const char *bp = bp0 ? rename_local(bp0) : NULL;
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (kk && bn >= 1) {
          TyKind et = ty_array_elem(rt);
          Scope *fs = comp_scope_of(c, block);
          LocalVar *flv = (fs && bp0) ? scope_local(fs, bp0) : NULL;
          TyKind fsaved = flv ? flv->type : TY_UNKNOWN;
          if (flv) { flv->type = et; for (int j = 0; j < bn; j++) infer_type(c, bb[j]); }
          int trecv = ++g_tmp, ti = ++g_tmp, twp = ++g_tmp, torig = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = sp_%sArray_length(_t%d);\n", torig, kk, trecv);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = 0;\n", twp);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, kk, trecv, ti);
          emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre);
          buf_printf(g_pre, " _telt%d = sp_%sArray_get(_t%d, _t%d);\n", ti, kk, trecv, ti);
          if (bp) {
            emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre);
            buf_printf(g_pre, " lv_%s = _telt%d;\n", bp, ti);
          }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf cb = expr_buf(c, bb[bn - 1]); g_indent = sv;
          TyKind cty = comp_ntype(c, bb[bn - 1]);
          /* Ruby truthiness on the predicate value: only nil/false are falsy, so a
             nilable int/float reads falsy at its sentinel but 0/0.0 stay truthy.
             Mirrors emit_each_with_index_terminal's select/reject test. */
          emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "if (");
          if (is_rej) buf_puts(g_pre, "!");
          if (cty == TY_POLY)       buf_printf(g_pre, "sp_poly_truthy(%s)", cb.p ? cb.p : "sp_box_nil()");
          else if (cty == TY_INT)   buf_printf(g_pre, "((%s) != SP_INT_NIL)", cb.p ? cb.p : "0");
          else if (cty == TY_FLOAT) buf_printf(g_pre, "(!sp_float_is_nil(%s))", cb.p ? cb.p : "0");
          else                      buf_printf(g_pre, "(%s)", cb.p ? cb.p : "0");
          buf_printf(g_pre, ") { sp_%sArray_set(_t%d, _t%d, _telt%d); _t%d++; }\n",
                     kk, trecv, twp, ti, twp);
          free(cb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "if (_t%d) _t%d->len = _t%d;\n", trecv, trecv, twp);
          if (flv) flv->type = fsaved;
          char box[64]; snprintf(box, sizeof box, "%s(_t%d)", array_box_fn(kk), trecv);
          emit_filter_bang_result(name, trecv, torig, twp, box, b);
          return 1;
        }
      }
      if ((sp_streq(name, "all?") || sp_streq(name, "any?") ||
           sp_streq(name, "none?") || sp_streq(name, "one?")) &&
          argc == 0 && nt_ref(nt, id, "block") < 0) {
        /* scalar-element arrays never hold nil/false: predicate is length-based */
        const char *op = sp_streq(name, "all?") ? ">= 0" : sp_streq(name, "any?") ? "> 0"
                       : sp_streq(name, "none?") ? "== 0" : "== 1";
        buf_printf(b, "(sp_%sArray_length(", k); emit_expr(c, recv, b); buf_printf(b, ") %s)", op);
        return 1;
      }
      if ((sp_streq(name, "any?") || sp_streq(name, "none?") || sp_streq(name, "one?") || sp_streq(name, "count")) &&
          argc == 1 && nt_ref(nt, id, "block") < 0) {
        /* array.any?(v) / none?(v) / one?(v) / count(v) -- compare by == */
        int ta = ++g_tmp, tv = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
        Buf ra = expr_buf(c, recv);
        buf_printf(b, "({ sp_%sArray *_t%d = %s;", k, ta, ra.p ? ra.p : "NULL"); free(ra.p);
        emit_indent(g_pre, 0);
        buf_printf(b, " "); emit_ctype(c, ty_array_elem(rt), b);
        buf_printf(b, " _t%d = ", tv); emit_expr(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " mrb_int _t%d = 0;", tc);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++)", ti, ti, k, ta, ti);
        if (rt == TY_STR_ARRAY)
          buf_printf(b, " if (strcmp(sp_%sArray_get(_t%d, _t%d), _t%d) == 0) _t%d++;", k, ta, ti, tv, tc);
        else
          buf_printf(b, " if (sp_%sArray_get(_t%d, _t%d) == _t%d) _t%d++;", k, ta, ti, tv, tc);
        if (sp_streq(name, "any?"))   buf_printf(b, " _t%d > 0; })", tc);
        else if (sp_streq(name, "none?"))  buf_printf(b, " _t%d == 0; })", tc);
        else if (sp_streq(name, "one?"))   buf_printf(b, " _t%d == 1; })", tc);
        else                              buf_printf(b, " _t%d; })", tc);
        return 1;
      }
      if ((sp_streq(name, "length") || sp_streq(name, "size") || sp_streq(name, "count")) &&
          argc == 0 && nt_ref(nt, id, "block") < 0) {
        buf_printf(b, "sp_%sArray_length(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "count") && argc == 0 && nt_ref(nt, id, "block") >= 0) {
        /* count { |x| cond } -- loop and count truthy block results */
        int blk = nt_ref(nt, id, "block");
        const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
        int body2 = nt_ref(nt, blk, "body");
        int bn2 = 0; const int *bb2 = body2 >= 0 ? nt_arr(nt, body2, "body", &bn2) : NULL;
        if (bn2 > 0) {
          int trecv = ++g_tmp, tcnt = ++g_tmp, ti = ++g_tmp;
          Buf rb2 = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rb2.p ? rb2.p : ""); free(rb2.p);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = 0;\n", tcnt);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, k, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti); }
          for (int j = 0; j < bn2 - 1; j++) emit_stmt(c, bb2[j], g_pre, g_indent + 1);
          int saveI = g_indent; g_indent = g_indent + 1;
          /* The block value is a condition: route through emit_cond so a poly /
             nil / scalar predicate becomes a valid C truthiness test (e.g.
             `count(&:alive)` where the element method is poly-dispatched would
             otherwise emit `if (sp_RbVal)` -- a struct in scalar position). */
          Buf vb2; memset(&vb2, 0, sizeof vb2); emit_cond(c, bb2[bn2 - 1], &vb2);
          g_indent = saveI;
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "if (%s) _t%d++;\n", vb2.p ? vb2.p : "0", tcnt);
          free(vb2.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tcnt);
          return 1;
        }
      }
      if (sp_streq(name, "empty?") && argc == 0) {
        buf_printf(b, "(sp_%sArray_length(", k); emit_expr(c, recv, b); buf_puts(b, ") == 0)");
        return 1;
      }
      if (sp_streq(name, "sum") && argc == 0) {
        buf_printf(b, "sp_%sArray_sum(", k); emit_expr(c, recv, b); buf_puts(b, ", 0)");
        return 1;
      }
      if (sp_streq(name, "sum") && argc == 1 && nt_ref(nt, id, "block") < 0) {
        TyKind init_t = comp_ntype(c, argv[0]);
        /* a float initial value promotes an integer-array sum to Float: add the
           float init to the integer total in floating point (sp_IntArray_sum
           returns mrb_int, so accumulating the init through it would truncate). */
        if (rt == TY_INT_ARRAY && init_t == TY_FLOAT) {
          buf_puts(b, "((mrb_float)("); emit_expr(c, argv[0], b);
          buf_puts(b, ") + (mrb_float)sp_IntArray_sum("); emit_expr(c, recv, b); buf_puts(b, ", 0))");
          return 1;
        }
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
        return 1;
      }
      if (sp_streq(name, "join") && argc <= 1) {
        buf_printf(b, "sp_%sArray_join(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (argc == 1 && comp_ntype(c, argv[0]) == TY_POLY) {
          buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else if (argc == 1) emit_expr(c, argv[0], b);
        else buf_puts(b, "\"\"");
        buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "inspect") || sp_streq(name, "to_s")) && argc == 0) {
        buf_printf(b, "sp_%sArray_inspect(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "first") && argc == 0) {
        buf_printf(b, "sp_%sArray_get(", k); emit_expr(c, recv, b); buf_puts(b, ", 0)");
        return 1;
      }
      if (sp_streq(name, "first") && argc == 1) {
        buf_printf(b, "sp_%sArray_slice(", k); emit_expr(c, recv, b); buf_puts(b, ", 0, ");
        emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "last") && argc == 1) {
        /* slice's negative start counts from the end -> the last n elements */
        int tn = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; sp_%sArray_slice(", k); emit_expr(c, recv, b);
        buf_printf(b, ", -_t%d, _t%d); })", tn, tn);
        return 1;
      }
      if (sp_streq(name, "pop") && argc == 0) {
        buf_printf(b, "sp_%sArray_pop(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc == 0) {
        buf_printf(b, "sp_%sArray_%s(", k, name); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "minmax") && argc == 0 && block < 0) {
        int t = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray *_t%d = sp_%sArray_new(); sp_%sArray_push(_t%d, sp_%sArray_min(_t%d));"
                      " sp_%sArray_push(_t%d, sp_%sArray_max(_t%d)); _t%d; })",
                   k, o, k, k, o, k, t, k, o, k, t, o);
        return 1;
      }
      if ((sp_streq(name, "index") || sp_streq(name, "find_index") || sp_streq(name, "rindex")) && argc == 1 && (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY)) {
        /* nil-on-miss -> poly */
        const char *fn = sp_streq(name, "rindex") ? "rindex_poly" : "index_poly";
        buf_printf(b, "sp_%sArray_%s(", k, fn);
        emit_expr(c, recv, b); buf_puts(b, ", ");
        if (sp_streq(k, "Int")) emit_int_expr(c, argv[0], b); else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "include?") && argc == 1) {
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
          return 1;
        }
      }
      if ((sp_streq(name, "include?") || sp_streq(name, "member?") || sp_streq(name, "index") || sp_streq(name, "find_index")) && argc == 1 && rt != TY_FLOAT_ARRAY) {
        const char *fn = (sp_streq(name, "include?") || sp_streq(name, "member?")) ? "include" : "index";
        buf_printf(b, "sp_%sArray_%s(", k, fn);
        emit_expr(c, recv, b); buf_puts(b, ", ");
        if (sp_streq(k, "Int")) emit_int_expr(c, argv[0], b); else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "sort") && argc == 0 &&
          (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY || rt == TY_STR_ARRAY)) {
        buf_printf(b, "sp_%sArray_sort(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "uniq") && argc == 0 && rt == TY_INT_ARRAY) {
        buf_puts(b, "sp_IntArray_uniq("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "last") && argc == 0) {
        int t = ++g_tmp;
        Buf rb = expr_buf(c, recv);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "%s _t%d = ", c_type_name(rt), t);
        buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
        buf_printf(b, "sp_%sArray_get(_t%d, sp_%sArray_length(_t%d) - 1)", k, t, k, t);
        return 1;
      }
      if ((sp_streq(name, "&") || sp_streq(name, "intersection") ||
           sp_streq(name, "|") || sp_streq(name, "union") ||
           sp_streq(name, "-") || sp_streq(name, "difference")) && argc == 1 && (a0 == rt || a0 == TY_UNKNOWN)) {
        const char *fn = (sp_streq(name, "&") || sp_streq(name, "intersection")) ? "intersect" : ((sp_streq(name, "|") || sp_streq(name, "union")) ? "union" : "difference");
        /* empty literal [] arg: use a null pointer (safe for all sp_*Array_* set ops) */
        if (a0 == TY_UNKNOWN) { buf_printf(b, "sp_%sArray_%s(", k, fn); emit_expr(c, recv, b); buf_puts(b, ", NULL)"); }
        else { buf_printf(b, "sp_%sArray_%s(", k, fn); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        return 1;
      }
      /* variadic named set ops: union/intersection/difference(*others) fold the
         binary operator over each argument, accumulating in a rooted temp. */
      if ((sp_streq(name, "intersection") || sp_streq(name, "union") ||
           sp_streq(name, "difference")) && argc >= 2) {
        int ok = 1;
        for (int j = 0; j < argc; j++) {
          TyKind atj = comp_ntype(c, argv[j]);
          if (atj != rt && atj != TY_UNKNOWN) { ok = 0; break; }
        }
        if (ok) {
          const char *fn = sp_streq(name, "intersection") ? "intersect" :
                           sp_streq(name, "union") ? "union" : "difference";
          int t = ++g_tmp;
          buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
          buf_printf(b, "; SP_GC_ROOT(_t%d);", t);
          for (int j = 0; j < argc; j++) {
            buf_printf(b, " _t%d = sp_%sArray_%s(_t%d, ", t, k, fn, t);
            if (comp_ntype(c, argv[j]) == TY_UNKNOWN) buf_puts(b, "NULL");
            else emit_expr(c, argv[j], b);
            buf_puts(b, ");");
          }
          buf_printf(b, " _t%d; })", t);
          return 1;
        }
      }
      if (sp_streq(name, "intersect?") && argc == 1 && (a0 == rt || a0 == TY_UNKNOWN)) {
        buf_printf(b, "sp_%sArray_intersect_p(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (a0 == TY_UNKNOWN) buf_puts(b, "NULL"); else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "union") && argc == 0) {
        buf_printf(b, "sp_%sArray_union(", k); emit_expr(c, recv, b); buf_puts(b, ", NULL)");
        return 1;
      }
      if (sp_streq(name, "sample") && argc == 0) {
        buf_printf(b, "sp_%sArray_sample(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "rotate") && argc <= 1) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = sp_%sArray_dup(", k, t, k); emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d); sp_%sArray_rotate_bang(_t%d, ", t, k, t);
        if (argc == 1) emit_int_expr(c, argv[0], b); else buf_puts(b, "1");
        buf_printf(b, "); _t%d; })", t);
        return 1;
      }
      if ((sp_streq(name, "slice") || sp_streq(name, "[]")) && argc == 2) {
        buf_printf(b, "sp_%sArray_slice(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "sample") && argc == 1) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = sp_%sArray_shuffle(", k, t, k); emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d); sp_%sArray_slice(_t%d, 0, ", t, k, t); emit_int_expr(c, argv[0], b);
        buf_puts(b, "); })");
        return 1;
      }
      if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc == 1 && block < 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = sp_%sArray_sort(", k, t, k); emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d);", t);
        if (sp_streq(name, "max")) buf_printf(b, " sp_%sArray_reverse_bang(_t%d);", k, t);
        buf_printf(b, " sp_%sArray_slice(_t%d, 0, ", k, t); emit_int_expr(c, argv[0], b);
        buf_puts(b, "); })");
        return 1;
      }
    }
    /* poly (mixed-element) array methods: elements are boxed sp_RbVal */
    if (rt == TY_POLY_ARRAY) {
      if (sp_streq(name, "[]") && argc == 1 && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode")) {
        /* arr[a..b] / arr[a...b] -> subarray */
        int rn = argv[0];
        int excl = (int)(nt_int(nt, rn, "flags", 0) & 4) ? 1 : 0;
        int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
        buf_puts(b, "sp_PolyArray_slice_range("); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (lo >= 0) emit_int_expr(c, lo, b); else buf_puts(b, "0");
        buf_puts(b, ", ");
        if (hi >= 0) emit_int_expr(c, hi, b); else buf_puts(b, "-1");
        buf_printf(b, ", %d)", hi >= 0 ? excl : 0);
        return 1;
      }
      if (sp_streq(name, "[]") && argc == 1) {
        buf_puts(b, "sp_PolyArray_get("); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (a0 == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "clear") && argc == 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; if (_t%d) _t%d->len = 0; _t%d; })", t, t, t);
        return 1;
      }
      if (sp_streq(name, "+") && argc == 1 && a0 == TY_POLY_ARRAY) {
        buf_puts(b, "sp_PolyArray_concat("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "&") || sp_streq(name, "intersection") ||
           sp_streq(name, "|") || sp_streq(name, "union") ||
           sp_streq(name, "-") || sp_streq(name, "difference")) && argc == 1 && (a0 == TY_POLY_ARRAY || a0 == TY_UNKNOWN)) {
        const char *fn = (sp_streq(name, "&") || sp_streq(name, "intersection")) ? "intersect" : (sp_streq(name, "|") || sp_streq(name, "union") ? "union" : "difference");
        buf_printf(b, "sp_PolyArray_%s(", fn);
        emit_expr(c, recv, b); buf_puts(b, ", ");
        if (a0 == TY_UNKNOWN) buf_puts(b, "NULL"); else emit_expr(c, argv[0], b);
        buf_puts(b, ")"); return 1;
      }
      /* variadic named set ops on a poly array: fold over each argument */
      if ((sp_streq(name, "intersection") || sp_streq(name, "union") ||
           sp_streq(name, "difference")) && argc >= 2) {
        int ok = 1;
        for (int j = 0; j < argc; j++) {
          TyKind atj = comp_ntype(c, argv[j]);
          if (atj != TY_POLY_ARRAY && atj != TY_UNKNOWN) { ok = 0; break; }
        }
        if (ok) {
          const char *fn = sp_streq(name, "intersection") ? "intersect" :
                           sp_streq(name, "union") ? "union" : "difference";
          int t = ++g_tmp;
          buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
          buf_printf(b, "; SP_GC_ROOT(_t%d);", t);
          for (int j = 0; j < argc; j++) {
            buf_printf(b, " _t%d = sp_PolyArray_%s(_t%d, ", t, fn, t);
            if (comp_ntype(c, argv[j]) == TY_UNKNOWN) buf_puts(b, "NULL");
            else emit_expr(c, argv[j], b);
            buf_puts(b, ");");
          }
          buf_printf(b, " _t%d; })", t);
          return 1;
        }
      }
      if (sp_streq(name, "intersect?") && argc == 1 && (a0 == TY_POLY_ARRAY || a0 == TY_UNKNOWN)) {
        buf_puts(b, "sp_PolyArray_intersect_p("); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (a0 == TY_UNKNOWN) buf_puts(b, "NULL"); else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "union") && argc == 0) {
        buf_puts(b, "sp_PolyArray_union("); emit_expr(c, recv, b); buf_puts(b, ", NULL)");
        return 1;
      }
      if (sp_streq(name, "sample") && argc == 0) {
        buf_puts(b, "sp_PolyArray_sample("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "rotate") && argc <= 1) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_dup(", t); emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d); sp_PolyArray_rotate_bang(_t%d, ", t, t);
        if (argc == 1) emit_int_expr(c, argv[0], b); else buf_puts(b, "1");
        buf_printf(b, "); _t%d; })", t);
        return 1;
      }
      if ((sp_streq(name, "slice") || sp_streq(name, "[]")) && argc == 2) {
        buf_puts(b, "sp_PolyArray_slice("); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "sample") && argc == 1) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_shuffle(", t); emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d); sp_PolyArray_slice(_t%d, 0, ", t, t); emit_int_expr(c, argv[0], b);
        buf_puts(b, "); })");
        return 1;
      }
      if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc == 1 && nt_ref(nt, id, "block") < 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_sort(", t); emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d);", t);
        if (sp_streq(name, "max")) buf_printf(b, " sp_PolyArray_reverse_bang(_t%d);", t);
        buf_printf(b, " sp_PolyArray_slice(_t%d, 0, ", t); emit_int_expr(c, argv[0], b);
        buf_puts(b, "); })");
        return 1;
      }
      if ((sp_streq(name, "all?") || sp_streq(name, "any?") ||
           sp_streq(name, "none?") || sp_streq(name, "one?")) &&
          argc == 0 && nt_ref(nt, id, "block") < 0) {
        /* count truthy elements; a poly element may be nil/false */
        int t = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; mrb_int _t%d = 0; for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)"
                      " if (sp_poly_truthy(sp_PolyArray_get(_t%d, _t%d))) _t%d++;",
                   tn, ti, ti, t, ti, t, ti, tn);
        const char *expr = sp_streq(name, "all?") ? "_t%d == sp_PolyArray_length(_t%d)"
                         : sp_streq(name, "any?") ? "_t%d > 0"
                         : sp_streq(name, "none?") ? "_t%d == 0" : "_t%d == 1";
        buf_puts(b, " (");
        if (sp_streq(name, "all?")) buf_printf(b, expr, tn, t);
        else buf_printf(b, expr, tn);
        buf_puts(b, "); })");
        return 1;
      }
      if ((sp_streq(name, "any?") || sp_streq(name, "none?") || sp_streq(name, "one?") || sp_streq(name, "count")) &&
          argc == 1 && nt_ref(nt, id, "block") < 0) {
        /* poly_array.one?(v) / any?(v) / none?(v) / count(v) */
        int ta = ++g_tmp, tv = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
        Buf ra = expr_buf(c, recv);
        buf_printf(b, "({ sp_PolyArray *_t%d = %s;", ta, ra.p ? ra.p : "NULL"); free(ra.p);
        buf_printf(b, " sp_RbVal _t%d = ", tv); emit_boxed(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " mrb_int _t%d = 0;", tc);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)", ti, ti, ta, ti);
        buf_printf(b, " if (sp_poly_eq(sp_PolyArray_get(_t%d, _t%d), _t%d)) _t%d++;", ta, ti, tv, tc);
        if (sp_streq(name, "any?"))        buf_printf(b, " _t%d > 0; })", tc);
        else if (sp_streq(name, "none?"))  buf_printf(b, " _t%d == 0; })", tc);
        else if (sp_streq(name, "one?"))   buf_printf(b, " _t%d == 1; })", tc);
        else                              buf_printf(b, " _t%d; })", tc);
        return 1;
      }
      if ((sp_streq(name, "length") || sp_streq(name, "size") || sp_streq(name, "count")) && argc == 0
          && nt_ref(nt, id, "block") < 0) {
        buf_puts(b, "sp_PolyArray_length("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "count") && argc == 0 && nt_ref(nt, id, "block") >= 0) {
        /* count { |x| cond } on PolyArray */
        int blk = nt_ref(nt, id, "block");
        const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
        int body2 = nt_ref(nt, blk, "body");
        int bn2 = 0; const int *bb2 = body2 >= 0 ? nt_arr(nt, body2, "body", &bn2) : NULL;
        if (bn2 > 0) {
          int trecv = ++g_tmp, tcnt = ++g_tmp, ti = ++g_tmp;
          Buf rb2 = expr_buf(c, recv);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_PolyArray *_t%d = %s;\n", trecv, rb2.p ? rb2.p : ""); free(rb2.p);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = 0;\n", tcnt);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n",
                     ti, ti, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
          for (int j = 0; j < bn2 - 1; j++) emit_stmt(c, bb2[j], g_pre, g_indent + 1);
          int saveI = g_indent; g_indent = g_indent + 1;
          /* The block value is a condition: route through emit_cond so a poly /
             nil / scalar predicate becomes a valid C truthiness test (e.g.
             `count(&:alive)` where the element method is poly-dispatched would
             otherwise emit `if (sp_RbVal)` -- a struct in scalar position). */
          Buf vb2; memset(&vb2, 0, sizeof vb2); emit_cond(c, bb2[bn2 - 1], &vb2);
          g_indent = saveI;
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "if (%s) _t%d++;\n", vb2.p ? vb2.p : "0", tcnt);
          free(vb2.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tcnt);
          return 1;
        }
      }
      if (sp_streq(name, "empty?") && argc == 0) {
        buf_puts(b, "(sp_PolyArray_length("); emit_expr(c, recv, b); buf_puts(b, ") == 0)");
        return 1;
      }
      if ((sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "append")) && argc == 1) {
        buf_puts(b, "sp_PolyArray_push("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "first") && argc == 0) {
        buf_puts(b, "sp_PolyArray_get("); emit_expr(c, recv, b); buf_puts(b, ", 0)");
        return 1;
      }
      if (sp_streq(name, "to_a") && argc == 0) { emit_expr(c, recv, b); return 1; }
      if (sp_streq(name, "fetch") && (argc == 1 || argc == 2)) {
        int ta = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp, tnorm = ++g_tmp;
        Buf ra = expr_buf(c, recv);
        buf_printf(b, "({ sp_PolyArray *_t%d = %s;", ta, ra.p ? ra.p : "NULL"); free(ra.p);
        buf_printf(b, " mrb_int _t%d = ", ti); emit_int_expr(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " mrb_int _t%d = sp_PolyArray_length(_t%d);", tn, ta);
        buf_printf(b, " mrb_int _t%d = _t%d < 0 ? _t%d + _t%d : _t%d;", tnorm, ti, ti, tn, ti);
        buf_printf(b, " (_t%d >= 0 && _t%d < _t%d) ? sp_PolyArray_get(_t%d, _t%d) :", tnorm, tnorm, tn, ta, tnorm);
        if (argc == 2) {
          buf_puts(b, " "); emit_boxed(c, argv[1], b); buf_puts(b, "; })");
        }
        else {
          buf_printf(b, " (sp_raise_cls(\"IndexError\", \"index out of bounds\"), sp_box_nil()); })");
        }
        return 1;
      }
      if (sp_streq(name, "zip") && argc >= 1 && nt_ref(nt, id, "block") < 0) {
        int ta = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tpair = ++g_tmp;
        int tb[16]; TyKind at[16]; int nargs = argc < 16 ? argc : 16;
        for (int j = 0; j < nargs; j++) { tb[j] = ++g_tmp; at[j] = comp_ntype(c, argv[j]); }
        Buf ra = expr_buf(c, recv);
        buf_printf(b, "({ sp_PolyArray *_t%d = %s;", ta, ra.p ? ra.p : "NULL"); free(ra.p);
        for (int j = 0; j < nargs; j++) {
          const char *kj = (at[j] == TY_POLY_ARRAY) ? "Poly" : (array_kind(at[j]) ? array_kind(at[j]) : "Poly");
          buf_printf(b, " sp_%sArray *_t%d = ", kj, tb[j]); emit_expr(c, argv[j], b); buf_puts(b, ";");
        }
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ti, ti, ta, ti);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new();", tpair);
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, ta, ti);
        for (int j = 0; j < nargs; j++) {
          if (at[j] == TY_INT_ARRAY)
            buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_IntArray_get(_t%d, _t%d)));", tpair, tb[j], ti);
          else if (at[j] == TY_STR_ARRAY)
            buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_StrArray_get(_t%d, _t%d)));", tpair, tb[j], ti);
          else if (at[j] == TY_FLOAT_ARRAY)
            buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_float(sp_FloatArray_get(_t%d, _t%d)));", tpair, tb[j], ti);
          else
            buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, tb[j], ti);
        }
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));", tr, tpair);
        buf_printf(b, " } _t%d; })", tr);
        return 1;
      }
      if (sp_streq(name, "last") && argc == 0) {
        int t = ++g_tmp;
        Buf rb = expr_buf(c, recv);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_PolyArray *_t%d = ", t);
        buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
        buf_printf(b, "sp_PolyArray_get(_t%d, sp_PolyArray_length(_t%d) - 1)", t, t);
        return 1;
      }
      if (sp_streq(name, "include?") && argc == 1) {
        buf_puts(b, "sp_PolyArray_include("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "dup") || sp_streq(name, "clone")) && argc == 0) {
        buf_puts(b, "sp_PolyArray_dup("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "compact") && argc == 0) {
        buf_puts(b, "sp_PolyArray_compact("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "compact!") && argc == 0) {
        buf_puts(b, "sp_PolyArray_compact_bang("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "flatten") && argc <= 1) {
        if (argc == 1) { buf_puts(b, "sp_PolyArray_flatten_n("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else { buf_puts(b, "sp_PolyArray_flatten("); emit_expr(c, recv, b); buf_puts(b, ")"); }
        return 1;
      }
      if (sp_streq(name, "transpose") && argc == 0) {
        buf_puts(b, "sp_int_array_transpose("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "assoc") || sp_streq(name, "rassoc")) && argc == 1) {
        buf_printf(b, "sp_PolyArray_%s(", name); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "join") && argc <= 1) {
        buf_puts(b, "sp_PolyArray_join("); emit_expr(c, recv, b); buf_puts(b, ", ");
        /* the separator must be a const char*; a poly separator (e.g. a reader
           whose ivar widened to poly) is converted with sp_poly_to_s. */
        if (argc == 1 && comp_ntype(c, argv[0]) == TY_POLY) {
          buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else if (argc == 1) emit_expr(c, argv[0], b);
        else buf_puts(b, "\"\"");
        buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "inspect") || sp_streq(name, "to_s")) && argc == 0) {
        buf_puts(b, "sp_PolyArray_inspect("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "slice!") && argc == 2) {
        buf_puts(b, "sp_PolyArray_slice_bang("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "replace") && argc == 1 && a0 == TY_POLY_ARRAY) {
        buf_puts(b, "sp_PolyArray_replace("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "shuffle") && argc == 0) {
        buf_puts(b, "sp_PolyArray_shuffle("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "sort") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        buf_puts(b, "sp_PolyArray_sort("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      /* minmax (no block): [min, max] via the poly comparator (user `<=>`
         through the cmp hook); incomparable raises the Comparable
         ArgumentError; empty -> [nil, nil]. Both temps rooted: min/max can
         allocate inside sp_poly_cmp (bigint temps) and push reallocs. */
      if (sp_streq(name, "minmax") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        int t = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, sp_PolyArray_min(_t%d));"
                      " sp_PolyArray_push(_t%d, sp_PolyArray_max(_t%d)); _t%d; })",
                   t, o, o, o, t, o, t, o);
        return 1;
      }
      {
        const char *base = NULL;
        if      (sp_streq(name, "reverse!")) base = "reverse_bang";
        else if (sp_streq(name, "shuffle!")) base = "shuffle_bang";
        else if (sp_streq(name, "sort!"))    base = "sort_bang";
        else if (sp_streq(name, "uniq!"))    base = "uniq_bang";
        if (base && argc == 0) {
          int t = ++g_tmp;
          buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
          buf_printf(b, "; sp_PolyArray_%s(_t%d); _t%d; })", base, t, t);
          return 1;
        }
      }
      if (sp_streq(name, "product") && argc == 1 && a0 == TY_POLY_ARRAY) {
        int ta = ++g_tmp, tb = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp, tpair = ++g_tmp;
        Buf ra; memset(&ra, 0, sizeof ra); Buf rb2; memset(&rb2, 0, sizeof rb2);
        emit_expr(c, recv, &ra); emit_expr(c, argv[0], &rb2);
        buf_printf(b, "({ sp_PolyArray *_t%d = %s; sp_PolyArray *_t%d = %s;",
                   ta, ra.p ? ra.p : "NULL", tb, rb2.p ? rb2.p : "NULL");
        free(ra.p); free(rb2.p);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
        buf_printf(b, " sp_PolyArray *_t%d = NULL;", tpair);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ti, ti, ta, ti);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", tj, tj, tb, tj);
        buf_printf(b, " _t%d = sp_PolyArray_new();", tpair);
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, ta, ti);
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, tb, tj);
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));", tr, tpair);
        buf_printf(b, " } } _t%d; })", tr);
        return 1;
      }
      if (sp_streq(name, "rotate!") && argc <= 1) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_PolyArray_rotate_bang(_t%d, ", t);
        if (argc == 1) emit_expr(c, argv[0], b); else buf_puts(b, "1");
        buf_printf(b, "); _t%d; })", t);
        return 1;
      }
      if ((sp_streq(name, "map!") || sp_streq(name, "collect!")) && nt_ref(nt, id, "block") >= 0) {
        int blk = nt_ref(nt, id, "block");
        const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          int trecv = ++g_tmp, ti = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n", ti, ti, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf vb = expr_buf(c, bb[bn - 1]); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "sp_PolyArray_set(_t%d, _t%d, %s);\n", trecv, ti, vb.p ? vb.p : "sp_box_nil()");
          free(vb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", trecv); return 1;
        }
      }
      if ((sp_streq(name, "select!") || sp_streq(name, "filter!") || sp_streq(name, "keep_if") ||
           sp_streq(name, "reject!") || sp_streq(name, "delete_if")) && nt_ref(nt, id, "block") >= 0) {
        int is_rej = sp_streq(name, "reject!") || sp_streq(name, "delete_if");
        int blk = nt_ref(nt, id, "block");
        const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          int trecv = ++g_tmp, ti = ++g_tmp, twp = ++g_tmp, torig = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = sp_PolyArray_length(_t%d);\n", torig, trecv);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = 0;\n", twp);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n", ti, ti, trecv, ti);
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal _telt%d = sp_PolyArray_get(_t%d, _t%d);\n", ti, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = _telt%d;\n", bp, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          /* Capture the predicate in its own buffer: a multi-statement terminal
             (a block ending in an if/else expression) lowers its statements
             through g_pre, and emitting the condition straight into g_pre would
             splice them after the already-written `if (!`. */
          Buf cb; memset(&cb, 0, sizeof cb);
          emit_cond(c, bb[bn - 1], &cb);
          emit_indent(g_pre, g_indent);
          buf_puts(g_pre, "if (");
          if (is_rej) buf_puts(g_pre, "!");
          buf_puts(g_pre, cb.p ? cb.p : "0");
          free(cb.p);
          g_indent = sv;
          buf_printf(g_pre, ") { sp_PolyArray_set(_t%d, _t%d, _telt%d); _t%d++; }\n",
                     trecv, twp, ti, twp);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "if (_t%d) _t%d->len = _t%d;\n", trecv, trecv, twp);
          char box[64]; snprintf(box, sizeof box, "sp_box_poly_array(_t%d)", trecv);
          emit_filter_bang_result(name, trecv, torig, twp, box, b);
          return 1;
        }
      }
      if (sp_streq(name, "to_h") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        TyKind res = comp_ntype(c, id);
        const char *hn = ty_hash_cname(res);
        if (!hn) hn = "SymPoly";
        TyKind kty = ty_hash_key(res), vty = ty_hash_val(res);
        int tr = ++g_tmp, th = ++g_tmp, ti = ++g_tmp, tp = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", tr); emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);", tr, hn, th, hn, th);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ti, ti, tr, ti);
        /* Each pair is a boxed array whose own kind varies (IntArray for [1,2],
           StrArray for ["a","b"], PolyArray for mixed); sp_poly_arr_get boxes an
           element from any of them, so key/value extraction works regardless. */
        buf_printf(b, " sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);", tp, tr, ti);
        buf_printf(b, " sp_%sHash_set(_t%d, ", hn, th);
        char kexpr[128];
        if (kty == TY_SYMBOL)      snprintf(kexpr, sizeof kexpr, "(sp_sym)sp_poly_arr_get(_t%d, 0).v.i", tp);
        else if (kty == TY_STRING) snprintf(kexpr, sizeof kexpr, "sp_poly_arr_get(_t%d, 0).v.s", tp);
        else if (kty == TY_POLY)   snprintf(kexpr, sizeof kexpr, "sp_poly_arr_get(_t%d, 0)", tp);
        else                       snprintf(kexpr, sizeof kexpr, "sp_poly_arr_get(_t%d, 0).v.i", tp);
        buf_puts(b, kexpr); buf_puts(b, ", ");
        /* value extraction */
        if (vty == TY_POLY)        buf_printf(b, "sp_poly_arr_get(_t%d, 1)", tp);
        else if (vty == TY_INT)    buf_printf(b, "sp_poly_arr_get(_t%d, 1).v.i", tp);
        else if (vty == TY_STRING) buf_printf(b, "sp_poly_arr_get(_t%d, 1).v.s", tp);
        else if (vty == TY_FLOAT)  buf_printf(b, "sp_poly_arr_get(_t%d, 1).v.f", tp);
        else                       buf_printf(b, "sp_poly_arr_get(_t%d, 1)", tp);
        buf_printf(b, "); } _t%d; })", th);
        return 1;
      }
      /* to_h { |x| [k, v] } on a poly array -> a hash keyed by the block's
         literal [k, v] tail pair (the only shape analyze_infer types):
         string/symbol keys get their own hash kind, anything else a fully
         boxed hash (doom: flats.to_h { |f| [f.name, f] }). */
      if (sp_streq(name, "to_h") && argc == 0 && nt_ref(nt, id, "block") >= 0) {
        int blk = nt_ref(nt, id, "block");
        const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        int tail = bn > 0 ? bb[bn - 1] : -1;
        const char *tty = tail >= 0 ? nt_type(nt, tail) : NULL;
        int pairn = 0;
        const int *pair = (tty && sp_streq(tty, "ArrayNode")) ? nt_arr(nt, tail, "elements", &pairn) : NULL;
        const char *hn = pair && pairn == 2 ? ty_hash_cname(res) : NULL;
        if (hn) {
          TyKind kt = comp_ntype(c, pair[0]);
          int trecv = ++g_tmp, ti = ++g_tmp, th = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_PolyArray *_t%d = %s; SP_GC_ROOT(_t%d);\n", trecv, rb.p ? rb.p : "NULL", trecv); free(rb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);\n", hn, th, hn, th);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n",
                     ti, ti, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf kb; memset(&kb, 0, sizeof kb);
          if (kt == TY_STRING) {
            /* A TY_STRING slot can carry nil (NULL); a Str-keyed hash can't
               store it (NULL marks an empty bucket and sp_str_hash reads
               k[-1]), so raise instead of segfaulting on a nil key. */
            int tk = ++g_tmp;
            buf_printf(&kb, "({ const char *_t%d = sp_str_dup(", tk);
            emit_expr(c, pair[0], &kb);
            buf_printf(&kb, "); if (!_t%d) sp_raise_cls(\"TypeError\", \"nil key in a string-keyed Hash\"); _t%d; })", tk, tk);
          }
          else if (kt == TY_SYMBOL) emit_expr(c, pair[0], &kb);
          else emit_boxed(c, pair[0], &kb);
          Buf vb; memset(&vb, 0, sizeof vb); emit_boxed(c, pair[1], &vb);
          g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "sp_%sHash_set(_t%d, %s, %s);\n", hn, th, kb.p ? kb.p : "", vb.p ? vb.p : "");
          free(kb.p); free(vb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", th);
          return 1;
        }
      }
    }
  }
  return 0;
}

/* Emit a statement-expression materializing a hash's entries as a PolyArray of
   [key, value] poly pairs in insertion order. The source hash is GC-rooted
   because each pair allocates inside the walk. Shared by Hash#to_a/#entries and
   Hash#sort. */
static void emit_hash_pairs_expr(Compiler *c, int recv, TyKind rt, const char *hn, Buf *b) {
  int th = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tp = ++g_tmp;
  TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
  buf_printf(b, "({ sp_%sHash *_t%d = ", hn, th); emit_expr(c, recv, b);
  buf_printf(b, "; SP_GC_ROOT(_t%d);", th);
  buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
  buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
  buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tp, tp);
  if (kt == TY_SYMBOL)
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym(_t%d->order[_t%d]));", tp, th, ti);
  else if (kt == TY_STRING)
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(_t%d->order[_t%d]));", tp, th, ti);
  else if (kt == TY_INT)
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(_t%d->order[_t%d]));", tp, th, ti);
  else
    buf_printf(b, " sp_PolyArray_push(_t%d, _t%d->keys[_t%d->order[_t%d]]);", tp, th, th, ti);
  if (rt == TY_POLY_POLY_HASH)
    buf_printf(b, " sp_PolyArray_push(_t%d, _t%d->vals[_t%d->order[_t%d]]);", tp, th, th, ti);
  else if (vt == TY_POLY)
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_%sHash_get(_t%d, _t%d->order[_t%d]));", tp, hn, th, th, ti);
  else if (vt == TY_INT)
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_%sHash_get(_t%d, _t%d->order[_t%d])));", tp, hn, th, th, ti);
  else
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_%sHash_get(_t%d, _t%d->order[_t%d])));", tp, hn, th, th, ti);
  buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));", tr, tp);
  buf_printf(b, " } _t%d; })", tr);
}

int emit_hash_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  if (recv >= 0 && ty_is_hash(rt)) {
    /* compare_by_identity? is always false for a value-keyed hash; the mutating
       compare_by_identity cannot be honored (keys are compared by value) and is
       rejected loudly rather than silently no-op'd. */
    if (sp_streq(name, "compare_by_identity?") && argc == 0) { buf_puts(b, "0"); return 1; }
    if (sp_streq(name, "compare_by_identity"))  /* any arity: identity hashing is unsupported */
      unsupported(c, id, "Hash#compare_by_identity (identity-keyed hashing)");
    const char *hn = ty_hash_cname(rt);
    if (hn) {
      /* select! / filter! / reject! / keep_if / delete_if { |k, v| cond } in
         expression position (the statement form lives in emit_iteration_stmt).
         Mutates in place; `!` forms yield nil when nothing was removed else self,
         keep_if/delete_if always yield self. */
      if ((sp_streq(name, "delete_if") || sp_streq(name, "reject!") || sp_streq(name, "select!") ||
           sp_streq(name, "filter!") || sp_streq(name, "keep_if")) &&
          nt_ref(nt, id, "block") >= 0 && rt != TY_POLY_POLY_HASH) {
        int block = nt_ref(nt, id, "block");
        int is_rej = sp_streq(name, "delete_if") || sp_streq(name, "reject!");
        const char *p0_raw = block_param_name(c, block, 0);
        const char *p1_raw = block_param_name(c, block, 1);
        const char *kp = p0_raw ? rename_local(p0_raw) : NULL;
        const char *vp = p1_raw ? rename_local(p1_raw) : NULL;
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          Scope *hs = comp_scope_of(c, block);
          TyKind hkt = ty_hash_key(rt), hvt = ty_hash_val(rt);
          LocalVar *klv = (kp && hs) ? scope_local(hs, p0_raw) : NULL;
          LocalVar *vlv = (vp && hs) ? scope_local(hs, p1_raw) : NULL;
          TyKind ksaved = klv ? klv->type : TY_UNKNOWN;
          TyKind vsaved = vlv ? vlv->type : TY_UNKNOWN;
          if (klv) klv->type = hkt;
          if (vlv) vlv->type = hvt;
          for (int j = 0; j < bn; j++) infer_type(c, bb[j]);
          int tr = ++g_tmp, ti = ++g_tmp, torig = ++g_tmp, twp = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", tr, rb.p ? rb.p : "NULL"); free(rb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash();\n", tr);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = _t%d ? _t%d->len : 0;\n", torig, tr, tr);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d && _t%d < _t%d->len; ) {\n", ti, tr, ti, tr);
          if (kp) {
            emit_indent(g_pre, g_indent + 1); emit_ctype(c, hkt, g_pre);
            buf_printf(g_pre, " lv_%s = _t%d->order[_t%d];\n", kp, tr, ti);
          }
          if (vp) {
            emit_indent(g_pre, g_indent + 1); emit_ctype(c, hvt, g_pre);
            buf_printf(g_pre, " lv_%s = sp_%sHash_get(_t%d, _t%d->order[_t%d]);\n", vp, hn, tr, tr, ti);
          }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          Buf *sp_save = g_pre; int gi_save = g_indent;
          Buf cpre; memset(&cpre, 0, sizeof cpre); g_pre = &cpre; g_indent = gi_save + 1;
          Buf cexpr = expr_buf(c, bb[bn - 1]);
          g_pre = sp_save; g_indent = gi_save;
          if (cpre.p) { buf_puts(g_pre, cpre.p); free(cpre.p); }
          emit_indent(g_pre, g_indent + 1);
          if (is_rej) buf_printf(g_pre, "if (%s) {\n", cexpr.p ? cexpr.p : "0");
          else        buf_printf(g_pre, "if (!(%s)) {\n", cexpr.p ? cexpr.p : "0");
          free(cexpr.p);
          emit_indent(g_pre, g_indent + 2);
          buf_printf(g_pre, "sp_%sHash_delete(_t%d, _t%d->order[_t%d]);\n", hn, tr, tr, ti);
          emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "} else {\n");
          emit_indent(g_pre, g_indent + 2); buf_printf(g_pre, "_t%d++;\n", ti);
          emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = _t%d ? _t%d->len : 0;\n", twp, tr, tr);
          if (klv) klv->type = ksaved;
          if (vlv) vlv->type = vsaved;
          char box[96]; snprintf(box, sizeof box, "sp_box_obj(_t%d, %s)", tr, hash_box_cls(rt));
          emit_filter_bang_result(name, tr, torig, twp, box, b);
          return 1;
        }
      }
      /* Hash#to_proc: a Proc mapping a key to the hash value, closing over the
         hash. Emit a per-variant lookup fn matching the sp_proc_call ABI. */
      if (sp_streq(name, "to_proc") && argc == 0) {
        TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
        int pn = ++g_proc_counter;
        const char *keyexpr = (kt == TY_SYMBOL) ? "(sp_sym)args[0]"
                            : (kt == TY_STRING) ? "(const char *)(uintptr_t)args[0]"
                            : "args[0]";
        buf_printf(&g_proc_protos, "static mrb_int _hashproc_%d(void *cap, mrb_int argc, mrb_int *args);\n", pn);
        buf_printf(&g_procs, "static mrb_int _hashproc_%d(void *cap, mrb_int argc, mrb_int *args) {\n", pn);
        buf_printf(&g_procs, "  if (argc < 1) return 0;\n");
        buf_printf(&g_procs, "  sp_%sHash *_h = (sp_%sHash *)cap;\n", hn, hn);
        if (vt == TY_POLY) {
          if (!g_needs_proc_poly_retslot) {
            g_needs_proc_poly_retslot = 1;
            buf_puts(&g_proc_protos, "static SP_TLS sp_RbVal _sp_proc_poly_ret;\n");
          }
          buf_printf(&g_procs, "  _sp_proc_poly_ret = sp_%sHash_get(_h, %s);\n  return 0;\n}\n", hn, keyexpr);
        }
        else if (vt == TY_STRING) {
          buf_printf(&g_procs, "  return (mrb_int)(uintptr_t)sp_%sHash_get(_h, %s);\n}\n", hn, keyexpr);
        }
        else {
          buf_printf(&g_procs, "  return (mrb_int)sp_%sHash_get(_h, %s);\n}\n", hn, keyexpr);
        }
        buf_printf(b, "sp_proc_new_meta((void *)_hashproc_%d, (void *)(", pn);
        emit_expr(c, recv, b);
        buf_puts(b, "), sp_hashproc_cap_scan, 1, FALSE, 1, NULL, NULL)");
        return 1;
      }
      if ((sp_streq(name, "dup") || sp_streq(name, "clone")) && argc == 0) {
        buf_printf(b, "sp_%sHash_dup(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "[]") && argc == 1) {
        TyKind arg_kt = comp_ntype(c, argv[0]);
        TyKind hash_kt = ty_hash_key(rt);
        /* key type mismatch: sym key on str-keyed hash (or vice versa) -- the key
           can never exist in the hash, so always return the hash's default value.
           Exception: a symbol key on a string-keyed hash is coerced to its name
           (the Hash.new{} StrPolyHash model), so it is NOT a mismatch. */
        if (hash_kt != TY_POLY && hash_kt != TY_UNKNOWN &&
            arg_kt != TY_POLY && arg_kt != TY_UNKNOWN && arg_kt != hash_kt &&
            !(hash_kt == TY_STRING && arg_kt == TY_SYMBOL)) {
          TyKind vt = ty_hash_val(rt);
          int t = ++g_tmp;
          buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t); emit_expr(c, recv, b); buf_puts(b, "; ");
          if (vt == TY_INT) buf_printf(b, "_t%d ? _t%d->default_v : SP_INT_NIL; })", t, t);
          else if (vt == TY_STRING) buf_printf(b, "_t%d && _t%d->default_v ? _t%d->default_v : (&(\"\\xff\")[1]); })", t, t, t);
          else buf_printf(b, "_t%d ? _t%d->default_v : sp_box_nil(); })", t, t);
          return 1;
        }
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
        return 1;
      }
      if (sp_streq(name, "dig") && argc >= 1) {
        TyKind vt = ty_hash_val(rt);
        TyKind kt = ty_hash_key(rt);
        /* Static key-type mismatch (string key on sym hash, etc.) -> nil. */
        TyKind arg0t = comp_ntype(c, argv[0]);
        if ((kt == TY_SYMBOL && arg0t == TY_STRING) ||
            (kt == TY_STRING && arg0t == TY_SYMBOL)) {
          if (vt == TY_INT) buf_puts(b, "SP_INT_NIL");
          else if (vt == TY_STRING) buf_puts(b, "NULL");
          else buf_puts(b, "sp_box_nil()");
          return 1;
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
            /* For poly-keyed hashes (e.g. PolyPolyHash), infer sub-key type
               from the argument itself rather than from the parent key type. */
            TyKind dkt = (kt == TY_POLY || kt == TY_UNKNOWN)
                         ? comp_ntype(c, argv[di]) : kt;
            if (dkt == TY_SYMBOL) {
              buf_printf(b, " sp_sym _t%d = ", tk);
              emit_expr(c, argv[di], b);
              buf_printf(b, "; _t%d = sp_poly_get_sym(_t%d, _t%d);", tr, tr, tk);
            }
            else if (dkt == TY_STRING) {
              buf_printf(b, " const char *_t%d = ", tk);
              emit_expr(c, argv[di], b);
              buf_printf(b, "; _t%d = sp_poly_get_str(_t%d, _t%d);", tr, tr, tk);
            }
            else {
              buf_printf(b, " mrb_int _t%d = ", tk);
              emit_expr(c, argv[di], b);
              buf_printf(b, "; _t%d = sp_poly_arr_get_hash(_t%d, _t%d);", tr, tr, tk);
            }
          }
          buf_printf(b, " _t%d; })", tr);
        }
        return 1;
      }
      if ((sp_streq(name, "values_at") || sp_streq(name, "fetch_values")) && argc >= 1) {
        /* collect looked-up values into a poly array; values_at yields nil for
           a missing key, fetch_values raises KeyError */
        int is_fetch = sp_streq(name, "fetch_values");
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
        return 1;
      }
      if (sp_streq(name, "fetch") && argc == 1) {
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
          }
else {
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
          return 1;
        }
        /* fetch(key) with no default raises KeyError on a miss */
        TyKind vt = ty_hash_val(rt);
        int th = ++g_tmp, tk = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
        buf_printf(b, "; %s _t%d = ", c_type_name(ty_hash_key(rt)), tk); emit_hash_key(c, argv[0], ty_hash_key(rt), b);
        buf_printf(b, "; sp_%sHash_has_key(_t%d, _t%d) ? sp_%sHash_get(_t%d, _t%d)"
                      " : (sp_raise_cls(\"KeyError\", \"key not found\"), %s); })",
                   hn, th, tk, hn, th, tk, vt == TY_POLY ? "sp_box_nil()" : default_value(vt));
        return 1;
      }
      if (sp_streq(name, "fetch") && argc == 2) {
        /* fetch(key, default) -> has_key? ? value : default */
        TyKind vt = ty_hash_val(rt);
        TyKind dt = comp_ntype(c, argv[1]);
        /* Empty `{}` default infers TY_UNKNOWN but is a hash — incompatible with int/str etc. */
        if (dt == TY_UNKNOWN) {
          const char *atn = nt_type(c->nt, argv[1]);
          if (atn && (sp_streq(atn, "HashNode") || sp_streq(atn, "KeywordHashNode")))
            dt = TY_POLY_POLY_HASH;
        }
        int needs_box = (vt != TY_POLY && ty_unify(vt, dt) == TY_POLY);
        int th = ++g_tmp, tk = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
        buf_printf(b, "; %s _t%d = ", c_type_name(ty_hash_key(rt)), tk); emit_hash_key(c, argv[0], ty_hash_key(rt), b);
        if (needs_box) {
          buf_printf(b, "; sp_%sHash_has_key(_t%d, _t%d) ? ", hn, th, tk);
          Buf _bx; memset(&_bx, 0, sizeof _bx);
          buf_printf(&_bx, "sp_%sHash_get(_t%d, _t%d)", hn, th, tk);
          emit_boxed_text(c, vt, _bx.p, b);
          free(_bx.p);
          buf_puts(b, " : "); emit_boxed(c, argv[1], b);
        }
        else {
          buf_printf(b, "; sp_%sHash_has_key(_t%d, _t%d) ? sp_%sHash_get(_t%d, _t%d) : ", hn, th, tk, hn, th, tk);
          if (vt == TY_POLY && dt != TY_POLY) emit_boxed(c, argv[1], b);
          else emit_expr(c, argv[1], b);
        }
        buf_puts(b, "; })");
        return 1;
      }
      if ((sp_streq(name, "length") || sp_streq(name, "size") ||
           (sp_streq(name, "count") && nt_ref(nt, id, "block") < 0)) && argc == 0) {
        buf_printf(b, "sp_%sHash_length(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "empty?") && argc == 0) {
        buf_printf(b, "(sp_%sHash_length(", hn); emit_expr(c, recv, b); buf_puts(b, ") == 0)");
        return 1;
      }
      if (sp_streq(name, "clear") && argc == 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t);
        emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sHash_clear(_t%d); _t%d; })", hn, t, t);
        return 1;
      }
      if ((sp_streq(name, "has_key?") || sp_streq(name, "key?") ||
           sp_streq(name, "include?") || sp_streq(name, "member?")) && argc == 1) {
        TyKind arg_kt = comp_ntype(c, argv[0]);
        TyKind hash_kt = ty_hash_key(rt);
        if (hash_kt != TY_POLY && arg_kt != TY_POLY && arg_kt != TY_UNKNOWN && arg_kt != hash_kt) {
          /* type mismatch (e.g. string arg on sym-keyed hash): always false */
          buf_puts(b, "0"); return 1;
        }
        buf_printf(b, "sp_%sHash_has_key(", hn);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_hash_key(c, argv[0], hash_kt, b); buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "value?") || sp_streq(name, "has_value?")) && argc == 1) {
        int poly = (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH);
        buf_printf(b, "sp_%sHash_has_value(", hn);
        emit_expr(c, recv, b); buf_puts(b, ", ");
        if (poly) emit_boxed(c, argv[0], b); else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      /* Hash#key(value): the first key mapping to value (sym-keyed hash). */
      if (sp_streq(name, "key") && argc == 1 && rt == TY_SYM_POLY_HASH) {
        buf_puts(b, "sp_SymPolyHash_key(");
        emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_boxed(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "replace") && argc == 1 && comp_ntype(c, argv[0]) == rt) {
        buf_printf(b, "sp_%sHash_replace(", hn);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "default") && argc == 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t); emit_expr(c, recv, b);
        if (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH) {
          buf_printf(b, "; _t%d ? _t%d->default_v : sp_box_nil(); })", t, t);
        }
        else if (rt == TY_STR_INT_HASH || rt == TY_INT_INT_HASH) {
          buf_printf(b, "; (_t%d && _t%d->default_v != SP_INT_NIL) ? sp_box_int(_t%d->default_v) : sp_box_nil(); })", t, t, t);
        }
        else if (rt == TY_STR_STR_HASH || rt == TY_INT_STR_HASH) {
          buf_printf(b, "; (_t%d && _t%d->default_v) ? sp_box_str(_t%d->default_v) : sp_box_nil(); })", t, t, t);
        }
        else {
          buf_printf(b, "; (void)_t%d; sp_box_nil(); })", t);
        }
        return 1;
      }
      if (sp_streq(name, "default=") && argc == 1) {
        int t = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t); emit_expr(c, recv, b);
        if (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH) {
          buf_printf(b, "; if (_t%d) _t%d->default_v = ", t, t); emit_boxed(c, argv[0], b); buf_puts(b, "; ");
        }
        else if (rt == TY_STR_INT_HASH || rt == TY_INT_INT_HASH) {
          buf_printf(b, "; if (_t%d) _t%d->default_v = ", t, t); emit_expr(c, argv[0], b); buf_puts(b, "; ");
        }
        else if (rt == TY_STR_STR_HASH || rt == TY_INT_STR_HASH) {
          buf_printf(b, "; if (_t%d) _t%d->default_v = ", t, t); emit_expr(c, argv[0], b); buf_puts(b, "; ");
        }
        emit_expr(c, argv[0], b); buf_puts(b, "; })"); return 1;
      }
      if (sp_streq(name, "keys") && argc == 0 && rt == TY_SYM_POLY_HASH) {
        /* runtime returns sym ids as an IntArray; box into a poly (sym) array */
        int ki = ++g_tmp, kp = ++g_tmp, ii = ++g_tmp;
        buf_printf(b, "({ sp_IntArray *_t%d = sp_SymPolyHash_keys(", ki); emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ki, kp, kp);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++)"
                      " sp_PolyArray_push(_t%d, sp_box_sym((sp_sym)sp_IntArray_get(_t%d, _t%d)));",
                   ii, ii, ki, ii, kp, ki, ii);
        buf_printf(b, " _t%d; })", kp);
        return 1;
      }
      if (sp_streq(name, "keys") && argc == 0) {
        buf_printf(b, "sp_%sHash_keys(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "values") && argc == 0) {
        buf_printf(b, "sp_%sHash_values(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "inspect") || sp_streq(name, "to_s")) && argc == 0) {
        buf_printf(b, "sp_%sHash_inspect(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "merge!") || sp_streq(name, "update")) && argc == 1) {
        /* In-place merge: insert each key of `other` into the receiver (a
           conflict-resolution block, if present, picks the kept value), then
           yield the receiver. A typed receiver can't change variant in place,
           so only a same-variant argument is accepted; any other argument
           (including a poly-boxed hash, which can't be variant-checked here
           without risking type confusion) falls through to the unsupported
           path rather than silently dropping or mistyping. */
        TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
        /* merging an empty hash literal is a no-op; yield the receiver. (An
           empty `{}` has no inferable variant, so it can't take the loop.) */
        const char *aty0 = nt_type(nt, argv[0]);
        if (aty0 && (sp_streq(aty0, "HashNode") || sp_streq(aty0, "KeywordHashNode"))) {
          int en = 0; nt_arr(nt, argv[0], "elements", &en);
          if (en == 0) { emit_expr(c, recv, b); return 1; }
        }
        TyKind at = comp_ntype(c, argv[0]);
        if (at != rt) return 0;
        int blk = nt_ref(nt, id, "block");
        int tr = ++g_tmp, to = ++g_tmp, ti = ++g_tmp, tk = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), tr); emit_expr(c, recv, b); buf_puts(b, ";");
        buf_printf(b, " %s _t%d = ", c_type_name(rt), to); emit_expr(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, to, ti);
        buf_printf(b, " %s _t%d = _t%d->order[_t%d];", c_type_name(kt), tk, to, ti);
        if (blk >= 0) {
          const char *bp0 = block_param_name(c, blk, 0);
          const char *bp1 = block_param_name(c, blk, 1);
          const char *bp2 = block_param_name(c, blk, 2);
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
          buf_printf(b, "); } else { sp_%sHash_set(_t%d, _t%d, sp_%sHash_get(_t%d, _t%d)); }", hn, tr, tk, hn, to, tk);
        }
        else {
          buf_printf(b, " sp_%sHash_set(_t%d, _t%d, sp_%sHash_get(_t%d, _t%d));", hn, tr, tk, hn, to, tk);
        }
        buf_printf(b, " } _t%d; })", tr);
        return 1;
      }
      if (sp_streq(name, "merge") && argc == 1 && nt_ref(nt, id, "block") >= 0) {
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
        return 1;
      }
      if (sp_streq(name, "merge") && argc == 1 &&
          (rt == TY_STR_INT_HASH || rt == TY_STR_POLY_HASH || rt == TY_SYM_POLY_HASH ||
           rt == TY_STR_STR_HASH || rt == TY_POLY_POLY_HASH || rt == TY_INT_INT_HASH ||
           rt == TY_INT_STR_HASH)) {
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
          return 1;
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
        return 1;
      }
      if (sp_streq(name, "invert") && argc == 0) {
        if (rt == TY_STR_STR_HASH) {
          buf_printf(b, "sp_StrStrHash_invert("); emit_expr(c, recv, b); buf_puts(b, ")");
        }
        else if (rt == TY_STR_INT_HASH) {
          buf_printf(b, "sp_StrIntHash_invert_poly("); emit_expr(c, recv, b); buf_puts(b, ")");
        }
        else if (rt == TY_INT_STR_HASH) {
          buf_printf(b, "sp_IntStrHash_invert("); emit_expr(c, recv, b); buf_puts(b, ")");
        }
        else {
          /* generic: build PolyPolyHash by swapping key/value of each entry */
          int th = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
          buf_printf(b, "({ sp_%sHash *_t%d = ", hn, th); emit_expr(c, recv, b);
          buf_printf(b, "; sp_PolyPolyHash *_t%d = sp_PolyPolyHash_new(); SP_GC_ROOT(_t%d);", tr, tr);
          buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
          /* key and value access depend on the hash variant */
          TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
          /* emit key as sp_RbVal */
          if (kt == TY_SYMBOL)
            buf_printf(b, " sp_RbVal _k%d = sp_box_sym(_t%d->order[_t%d]);", ti, th, ti);
          else if (kt == TY_STRING)
            buf_printf(b, " sp_RbVal _k%d = sp_box_str(_t%d->order[_t%d]);", ti, th, ti);
          else if (kt == TY_INT)
            buf_printf(b, " sp_RbVal _k%d = sp_box_int(_t%d->order[_t%d]);", ti, th, ti);
          else
            buf_printf(b, " sp_RbVal _k%d = _t%d->keys[_t%d->order[_t%d]];", ti, th, th, ti);
          /* emit value as sp_RbVal */
          if (vt == TY_POLY)
            buf_printf(b, " sp_RbVal _v%d = sp_%sHash_get(_t%d, _t%d->order[_t%d]);", ti, hn, th, th, ti);
          else if (vt == TY_INT) {
            buf_printf(b, " sp_RbVal _v%d = sp_box_int(sp_%sHash_get(_t%d, _t%d->order[_t%d]));", ti, hn, th, th, ti);
          }
          else {
            buf_printf(b, " sp_RbVal _v%d = sp_box_str(sp_%sHash_get(_t%d, _t%d->order[_t%d]));", ti, hn, th, th, ti);
          }
          buf_printf(b, " sp_PolyPolyHash_set(_t%d, _v%d, _k%d); }", tr, ti, ti);
          buf_printf(b, " _t%d; })", tr);
        }
        return 1;
      }
      if (sp_streq(name, "flatten") && argc <= 1) {
        /* interleave keys and values into a flat PolyArray */
        int th = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
        TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
        buf_printf(b, "({ sp_%sHash *_t%d = ", hn, th); emit_expr(c, recv, b);
        buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
        if (kt == TY_SYMBOL)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym(_t%d->order[_t%d]));", tr, th, ti);
        else if (kt == TY_STRING)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(_t%d->order[_t%d]));", tr, th, ti);
        else if (kt == TY_INT)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(_t%d->order[_t%d]));", tr, th, ti);
        else
          buf_printf(b, " sp_PolyArray_push(_t%d, _t%d->keys[_t%d->order[_t%d]]);", tr, th, th, ti);
        if (vt == TY_POLY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_%sHash_get(_t%d, _t%d->order[_t%d]));", tr, hn, th, th, ti);
        else if (vt == TY_INT)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_%sHash_get(_t%d, _t%d->order[_t%d])));", tr, hn, th, th, ti);
        else
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_%sHash_get(_t%d, _t%d->order[_t%d])));", tr, hn, th, th, ti);
        buf_printf(b, " } _t%d; })", tr);
        return 1;
      }
      if ((sp_streq(name, "to_a") || sp_streq(name, "entries")) && argc == 0) {
        emit_hash_pairs_expr(c, recv, rt, hn, b);
        return 1;
      }
      if (sp_streq(name, "sort") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        /* sort entries by Array#<=> over each [key, value] pair */
        buf_puts(b, "sp_PolyArray_sort_pairs(");
        emit_hash_pairs_expr(c, recv, rt, hn, b);
        buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "assoc") || sp_streq(name, "rassoc")) && argc == 1) {
        /* find first pair where key==arg (assoc) or value==arg (rassoc); returns [k,v] or nil */
        int is_rassoc = sp_streq(name, "rassoc");
        TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
        int th = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, ta = ++g_tmp;
        buf_printf(b, "({ sp_%sHash *_t%d = ", hn, th); emit_expr(c, recv, b); buf_puts(b, ";");
        /* store argument */
        if (!is_rassoc) {
          buf_printf(b, " %s _t%d = ", c_type_name(kt), ta); emit_hash_key(c, argv[0], kt, b); buf_puts(b, ";");
        }
        else {
          /* rassoc: arg has value type */
          buf_printf(b, " sp_RbVal _t%d = ", ta); emit_boxed(c, argv[0], b); buf_puts(b, ";");
        }
        buf_printf(b, " sp_PolyArray *_t%d = NULL;", tr);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
        if (!is_rassoc) {
          /* assoc: compare key */
          if (rt == TY_POLY_POLY_HASH)
            buf_printf(b, " if (sp_rbval_eql_key(_t%d->keys[_t%d->order[_t%d]], _t%d)) {", th, th, ti, ta);
          else if (kt == TY_STRING)
            buf_printf(b, " if (!strcmp(_t%d->order[_t%d], _t%d)) {", th, ti, ta);
          else
            buf_printf(b, " if (_t%d->order[_t%d] == _t%d) {", th, ti, ta);
        }
        else {
          /* rassoc: compare value (boxed) */
          buf_printf(b, " sp_RbVal _rv%d = ", ti);
          if (vt == TY_POLY) buf_printf(b, "sp_%sHash_get(_t%d, _t%d->order[_t%d]);", hn, th, th, ti);
          else if (vt == TY_INT) buf_printf(b, "sp_box_int(sp_%sHash_get(_t%d, _t%d->order[_t%d]));", hn, th, th, ti);
          else buf_printf(b, "sp_box_str(sp_%sHash_get(_t%d, _t%d->order[_t%d]));", hn, th, th, ti);
          buf_printf(b, " if (sp_poly_eq(_rv%d, _t%d)) {", ti, ta);
        }
        /* build pair */
        buf_printf(b, " _t%d = sp_PolyArray_new();", tr);
        if (kt == TY_SYMBOL)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym(_t%d->order[_t%d]));", tr, th, ti);
        else if (kt == TY_STRING)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(_t%d->order[_t%d]));", tr, th, ti);
        else if (kt == TY_INT)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(_t%d->order[_t%d]));", tr, th, ti);
        else
          buf_printf(b, " sp_PolyArray_push(_t%d, _t%d->keys[_t%d->order[_t%d]]);", tr, th, th, ti);
        if (vt == TY_POLY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_%sHash_get(_t%d, _t%d->order[_t%d]));", tr, hn, th, th, ti);
        else if (vt == TY_INT)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_%sHash_get(_t%d, _t%d->order[_t%d])));", tr, hn, th, th, ti);
        else
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_%sHash_get(_t%d, _t%d->order[_t%d])));", tr, hn, th, th, ti);
        buf_printf(b, " break; } } _t%d; })", tr);  /* NULL = nil in poly context */
        return 1;
      }
      if (sp_streq(name, "compact") && argc == 0) {
        TyKind vt = ty_hash_val(rt);
        if (vt != TY_POLY) {
          /* Non-poly values can't be nil; compact is equivalent to dup */
          buf_printf(b, "sp_%sHash_dup(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        }
        else if (rt == TY_POLY_POLY_HASH) {
          int th = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
          buf_printf(b, "({ sp_PolyPolyHash *_t%d = ", th); emit_expr(c, recv, b);
          buf_printf(b, "; sp_PolyPolyHash *_t%d = sp_PolyPolyHash_new(); SP_GC_ROOT(_t%d);", tr, tr);
          buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
          buf_printf(b, " sp_RbVal _v%d = _t%d->vals[_t%d->order[_t%d]];", ti, th, th, ti);
          buf_printf(b, " if (!sp_poly_nil_p(_v%d)) sp_PolyPolyHash_set(_t%d, _t%d->keys[_t%d->order[_t%d]], _v%d); }", ti, tr, th, th, ti, ti);
          buf_printf(b, " _t%d; })", tr);
        }
        else {
          /* SYM_POLY_HASH or other poly-valued hash */
          int th = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
          buf_printf(b, "({ sp_%sHash *_t%d = ", hn, th); emit_expr(c, recv, b);
          buf_printf(b, "; sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);", hn, tr, hn, tr);
          buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
          buf_printf(b, " sp_RbVal _v%d = sp_%sHash_get(_t%d, _t%d->order[_t%d]);", ti, hn, th, th, ti);
          buf_printf(b, " if (!sp_poly_nil_p(_v%d)) sp_%sHash_set(_t%d, _t%d->order[_t%d], _v%d); }", ti, hn, tr, th, ti, ti);
          buf_printf(b, " _t%d; })", tr);
        }
        return 1;
      }
      if (sp_streq(name, "delete") && argc == 1 &&
          (rt == TY_STR_INT_HASH || rt == TY_STR_STR_HASH || rt == TY_SYM_POLY_HASH ||
           rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH)) {
        /* returns the deleted value (or nil on a miss), then removes the key */
        TyKind vt = ty_hash_val(rt);
        int th = ++g_tmp, tk = ++g_tmp, tv = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
        buf_printf(b, "; %s _t%d = ", c_type_name(ty_hash_key(rt)), tk); emit_hash_key(c, argv[0], ty_hash_key(rt), b);
        buf_printf(b, "; %s _t%d = sp_%sHash_has_key(_t%d, _t%d) ? sp_%sHash_get(_t%d, _t%d) : %s;",
                   c_type_name(vt), tv, hn, th, tk, hn, th, tk, vt == TY_POLY ? "sp_box_nil()" : default_value(vt));
        buf_printf(b, " sp_%sHash_delete(_t%d, _t%d); _t%d; })", hn, th, tk, tv);
        return 1;
      }
    }
  }
  return 0;
}

/* True when nodes a and b are the same side-effect-free lvalue -- the same local
   or instance variable read. Used to resolve `x.equal?(x)` (object identity) for
   receivers whose value identity is not otherwise modeled: re-reading one
   variable yields the same object, so the reflexive case is certainly true,
   while method calls / literals (which would produce fresh objects, or which the
   C compiler merges) are excluded. */
static int same_sefree_lvalue(Compiler *c, int a, int b) {
  if (a < 0 || b < 0) return 0;
  const char *ta = nt_type(c->nt, a), *tb = nt_type(c->nt, b);
  if (!ta || !tb || !sp_streq(ta, tb)) return 0;
  if (!sp_streq(ta, "LocalVariableReadNode") && !sp_streq(ta, "InstanceVariableReadNode")) return 0;
  const char *na = nt_str(c->nt, a, "name"), *nb = nt_str(c->nt, b, "name");
  return na && nb && sp_streq(na, nb);
}

int emit_scalar_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  /* scalar receiver methods: evaluate the receiver once into rs, then
     splice its text (so a literal/complex receiver isn't rebuilt). */
  if (recv >= 0 && (rt == TY_STRING || rt == TY_INT || rt == TY_FLOAT)) {
    Buf rs; memset(&rs, 0, sizeof rs);
    emit_expr(c, recv, &rs);
    const char *r = rs.p ? rs.p : "";
    /* A String-typed receiver that resolved to a poly nil -- e.g. an
       unresolvable chain like `Rails.application.class.to_s` in a method that
       is compiled but never called -- emits sp_box_nil(); coerce it to a
       const char* (yields "" at runtime) so the string ops below type-check. */
    if (rt == TY_STRING && sp_streq(r, "sp_box_nil()")) r = "sp_poly_to_s(sp_box_nil())";
    /* Same shape, but the unresolved-call gate raised (SPINEL_GATE_RAISE): its
       sp_raise_nomethod(...) is a side-effecting poly value, so coerce it (the
       raise diverges before the result is read) rather than feed the raw
       sp_RbVal into a const char* string op. */
    else if (rt == TY_STRING && strncmp(r, "sp_raise_nomethod(", 18) == 0) {
      Buf cb; memset(&cb, 0, sizeof cb); buf_printf(&cb, "sp_poly_to_s(%s)", r); r = cb.p ? cb.p : r;
    }
    int handled = 1;

    if (rt == TY_STRING) {
      /* blockless "a".upto("c") materializes the succ-sequence as an array */
      if (sp_streq(name, "upto") && argc == 1 && nt_ref(nt, id, "block") < 0) {
        buf_printf(b, "sp_StrArray_from_string_range(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", 0)");
      }
      /* string methods taking a regex-literal argument route to the engine */
      else if ((sp_streq(name, "gsub") || sp_streq(name, "sub")) && argc == 2 && re_lit_index(c, argv[0]) >= 0) {
        const char *suf = comp_ntype(c, argv[1]) == TY_STR_STR_HASH ? "_str_str_hash" : "";
        buf_printf(b, "sp_re_%s%s(sp_re_pat_%d, %s, ", name, suf, re_lit_index(c, argv[0]), r);
        emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if ((sp_streq(name, "gsub") || sp_streq(name, "sub")) && argc == 2 &&
               nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "InterpolatedRegularExpressionNode")) {
        Buf rp; memset(&rp, 0, sizeof rp);
        emit_regex_pat_to_buf(c, argv[0], &rp);
        buf_printf(b, "sp_re_%s(%s, %s, ", name, rp.p ? rp.p : "NULL", r);
        emit_expr(c, argv[1], b); buf_puts(b, ")");
        free(rp.p);
      }
      else if (sp_streq(name, "split") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_split(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (sp_streq(name, "split") && argc == 2 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_split_limit(sp_re_pat_%d, %s, ", re_lit_index(c, argv[0]), r);
        emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "scan") && argc == 1 && re_lit_index(c, argv[0]) >= 0 &&
               !re_has_captures(re_lit_src(c, argv[0]))) {
        buf_printf(b, "sp_re_scan(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (sp_streq(name, "scan") && argc == 1 && re_lit_index(c, argv[0]) >= 0 &&
               re_has_captures(re_lit_src(c, argv[0]))) {
        buf_printf(b, "sp_re_scan_poly(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (sp_streq(name, "to_sym") || sp_streq(name, "intern")) buf_printf(b, "sp_sym_intern(%s)", r);
      else if (sp_streq(name, "length") || sp_streq(name, "size")) {
        if (g_hoist_len_var && g_hoist_len_recv && recv >= 0 && nt_type(nt, recv) &&
            sp_streq(nt_type(nt, recv), "LocalVariableReadNode") && nt_str(nt, recv, "name") &&
            sp_streq(nt_str(nt, recv, "name"), g_hoist_len_recv))
          buf_puts(b, g_hoist_len_var);
        else buf_printf(b, "sp_str_length_m(%s)", r);
      }
      else if (sp_streq(name, "bytesize")) buf_printf(b, "sp_str_bytesize_m(%s)", r);
      else if (sp_streq(name, "upcase"))     buf_printf(b, "sp_str_upcase(%s)", r);
      else if (sp_streq(name, "downcase"))   buf_printf(b, "sp_str_downcase(%s)", r);
      else if (sp_streq(name, "capitalize")) buf_printf(b, "sp_str_capitalize(%s)", r);
      else if (sp_streq(name, "swapcase"))   buf_printf(b, "sp_str_swapcase(%s)", r);
      else if (sp_streq(name, "delete_prefix") && argc == 1) { buf_printf(b, "sp_str_delete_prefix(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "delete_suffix") && argc == 1) { buf_printf(b, "sp_str_delete_suffix(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "reverse"))    buf_printf(b, "sp_str_reverse(%s)", r);
      else if (sp_streq(name, "strip"))      buf_printf(b, "sp_str_strip(%s)", r);
      else if (sp_streq(name, "lstrip"))     buf_printf(b, "sp_str_lstrip(%s)", r);
      else if (sp_streq(name, "rstrip"))     buf_printf(b, "sp_str_rstrip(%s)", r);
      else if (sp_streq(name, "chomp") && argc == 1) {
        const char *a0ty = nt_type(nt, argv[0]);
        if (a0ty && sp_streq(a0ty, "NilNode")) {
          /* chomp(nil) returns the string unchanged */
          buf_puts(b, r);
        }
        else {
          buf_printf(b, "sp_str_chomp_sep(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
      }
      else if (sp_streq(name, "chomp"))      buf_printf(b, "sp_str_chomp(%s)", r);
      else if (sp_streq(name, "chop"))       buf_printf(b, "sp_str_chop(%s)", r);
      else if (sp_streq(name, "to_s")) {
        /* NOT the identity: a nullable string carries nil as NULL, and
           CRuby's nil.to_s is "" -- the coalesce keeps `ENV[missing].to_s`
           comparable against "" (#1664). A provably non-nil receiver costs
           one always-taken branch. */
        int tv = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = %s; _t%d ? _t%d : SPL(\"\"); })", tv, r, tv, tv);
      }
      else if (sp_streq(name, "to_str")) {
        /* Unlike to_s, CRuby's nil has no to_str: raise. */
        int tv = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = %s; if (!_t%d) sp_nil_recv(\"to_str\"); _t%d; })", tv, r, tv, tv);
      }
      else if ((sp_streq(name, "dup") || sp_streq(name, "clone")) && argc == 0)
        /* sp_str_dup, not dup_external: the receiver is a spinel string, and
           the byte_len-aware copy carries embedded NULs (dup_external is for
           unmarked C pointers and must stay strlen-based). */
        buf_printf(b, "sp_str_dup(%s)", r);
      else if (sp_streq(name, "inspect"))    { int tv = ++g_tmp; buf_printf(b, "({ const char *_t%d = %s; _t%d ? sp_str_inspect(_t%d) : SPL(\"nil\"); })", tv, r, tv, tv); }
      else if (sp_streq(name, "empty?"))     buf_printf(b, "sp_str_empty_p(%s)", r);
      else if (sp_streq(name, "include?") && argc == 1) {
        buf_printf(b, "sp_str_include(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "start_with?") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        /* s.start_with?(/re/): true when the pattern matches at index 0 */
        buf_printf(b, "(sp_re_match(sp_re_pat_%d, %s) == 0)", re_lit_index(c, argv[0]), r);
      }
      else if (sp_streq(name, "start_with?") && argc == 1) {
        buf_printf(b, "sp_str_start_with(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "end_with?") && argc == 1) {
        buf_printf(b, "sp_str_end_with(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "ascii_only?") && argc == 0) buf_printf(b, "sp_str_ascii_only(%s)", r);
      else if (sp_streq(name, "valid_encoding?") && argc == 0) buf_printf(b, "sp_str_valid_encoding(%s)", r);
      else if (sp_streq(name, "index") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_index_poly(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (sp_streq(name, "index") && argc == 1) {
        /* nil-on-miss carried as the SP_INT_NIL sentinel (a nullable int) */
        buf_printf(b, "sp_str_index_opt(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "index") && argc == 2) {
        buf_printf(b, "sp_str_index_from_opt(%s, ", r);
        emit_expr(c, argv[0], b); buf_puts(b, ", ");
        emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if ((sp_streq(name, "partition") || sp_streq(name, "rpartition")) && argc == 1 &&
               re_lit_index(c, argv[0]) < 0) {
        buf_printf(b, "sp_str_%s(%s, ", name, r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "partition") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
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
      else if (sp_streq(name, "rpartition") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_rpartition(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (sp_streq(name, "rindex") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_rindex_opt(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (sp_streq(name, "rindex") && argc == 1) { buf_printf(b, "sp_str_rindex_opt(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "rindex") && argc == 2) { buf_printf(b, "sp_str_rindex_from(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "crypt") && argc == 1) { buf_printf(b, "sp_str_crypt(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "scrub") && argc == 0) buf_printf(b, "sp_str_scrub(%s, 0)", r);
      else if (sp_streq(name, "scrub") && argc == 1) { buf_printf(b, "sp_str_scrub(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        /* s[/re/] -> the matched substring, or nil (NULL) on no match */
        buf_printf(b, "(sp_re_match(sp_re_pat_%d, %s) >= 0 ? sp_re_match_str : NULL)", re_lit_index(c, argv[0]), r);
      }
      else if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 1 && nt_type(c->nt, argv[0]) &&
               sp_streq(nt_type(c->nt, argv[0]), "RangeNode")) {
        /* s[a..b] / s[a...b]; beginless/endless ranges use 0 / length */
        int rn = argv[0];
        int excl = (int)(nt_int(c->nt, rn, "flags", 0) & 4) ? 1 : 0;
        int lo = nt_ref(c->nt, rn, "left"), hi = nt_ref(c->nt, rn, "right");
        buf_printf(b, "sp_str_sub_range_r(%s, ", r);
        if (lo >= 0) emit_int_expr(c, lo, b); else buf_puts(b, "0");
        buf_puts(b, ", ");
        if (hi >= 0) { emit_int_expr(c, hi, b); buf_printf(b, ", %d)", excl); }
        else buf_printf(b, "(mrb_int)sp_str_length(%s), 0)", r);  /* endless: to the end */
      }
      else if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 2) {
        /* s[start, len] */
        buf_printf(b, "sp_str_sub_range(%s, ", r);
        emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 1 && comp_ntype(c, argv[0]) == TY_STRING) {
        /* s["sub"] -> the substring if present, else nil */
        int tsub = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = ", tsub); emit_str_expr(c, argv[0], b);
        buf_printf(b, "; (strstr(%s, _t%d) ? _t%d : NULL); })", r, tsub, tsub);
      }
      else if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 1) {
        buf_printf(b, "sp_str_char_at_or_nil(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "split") && argc == 0) buf_printf(b, "sp_str_split_ws(%s)", r);
      else if (sp_streq(name, "split") && argc == 1) {
        /* split(nil) and split(" ") are whitespace-mode; split(sep) drops trailing empties */
        const char *aty = nt_type(c->nt, argv[0]);
        int nil_arg = aty && sp_streq(aty, "NilNode");
        int ws = nil_arg || (aty && sp_streq(aty, "StringNode") && nt_str(c->nt, argv[0], "content") &&
                 sp_streq(nt_str(c->nt, argv[0], "content"), " "));
        if (ws) buf_printf(b, "sp_str_split_ws(%s)", r);
        else { buf_printf(b, "sp_str_split_drop_trailing(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      }
      else if (sp_streq(name, "split") && argc == 2) {
        buf_printf(b, "sp_str_split_limit(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "clamp") && (argc == 2 ||
               (argc == 1 && nt_type(c->nt, argv[0]) && sp_streq(nt_type(c->nt, argv[0]), "RangeNode")))) {
        int lo_n, hi_n;
        if (argc == 2) { lo_n = argv[0]; hi_n = argv[1]; }
        else { int rn = argv[0]; lo_n = nt_ref(c->nt, rn, "left"); hi_n = nt_ref(c->nt, rn, "right"); }
        int tc = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = %s; const char *_t%d = ", tc, r, tlo); emit_expr(c, lo_n, b);
        buf_printf(b, "; const char *_t%d = ", thi); emit_expr(c, hi_n, b);
        buf_printf(b, "; strcmp(_t%d, _t%d) < 0 ? _t%d : (strcmp(_t%d, _t%d) > 0 ? _t%d : _t%d); })",
                   tc, tlo, tlo, tc, thi, thi, tc);
      }
      else if (sp_streq(name, "oct") && argc == 0) buf_printf(b, "sp_str_oct(%s)", r);
      else if (sp_streq(name, "hex") && argc == 0) buf_printf(b, "sp_str_to_i_base(%s, 16)", r);
      else if (sp_streq(name, "ord") && argc == 0) buf_printf(b, "sp_str_ord(%s)", r);
      else if ((sp_streq(name, "force_encoding") || sp_streq(name, "b") || sp_streq(name, "encode")) && argc <= 1) buf_printf(b, "(%s)", r);
      else if (sp_streq(name, "encoding") && argc == 0) buf_printf(b, "((void)(%s), sp_box_encoding(sp_encoding_utf8()))", r);
      else if (sp_streq(name, "dump") && argc == 0) buf_printf(b, "sp_str_dump(%s)", r);
      else if (sp_streq(name, "undump") && argc == 0) buf_printf(b, "sp_str_undump(%s)", r);
      else if (sp_streq(name, "casecmp") && argc == 1) { buf_printf(b, "sp_str_casecmp(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "casecmp?") && argc == 1) { buf_printf(b, "(sp_str_casecmp(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ") == 0)"); }
      else if (sp_streq(name, "byteslice") && argc == 2) { buf_printf(b, "sp_str_byteslice(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "byteslice") && argc == 1) { buf_printf(b, "sp_str_byteslice(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", 1)"); }
      else if (sp_streq(name, "setbyte") && argc == 2) { buf_printf(b, "sp_str_setbyte(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "getbyte") && argc == 1) {
        /* inline to a direct byte load (matches the legacy generator): the
           per-byte sp_str_getbyte call recomputes the string length and bounds
           every iteration, which the C compiler can't hoist across an aliasing
           setbyte. An out-of-range index reads adjacent bytes (as in legacy). */
        buf_printf(b, "((mrb_int)(unsigned char)(%s)[", r); emit_int_expr(c, argv[0], b); buf_puts(b, "])");
      }
      else if (sp_streq(name, "squeeze") && argc == 0) buf_printf(b, "sp_str_squeeze(%s)", r);
      else if (sp_streq(name, "squeeze") && argc == 1) { buf_printf(b, "sp_str_squeeze_chars(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "squeeze") && argc >= 2) {
        buf_printf(b, "sp_str_squeeze_n(%s, (const char *[]){", r);
        for (int a = 0; a < argc; a++) { if (a) buf_puts(b, ", "); emit_expr(c, argv[a], b); }
        buf_printf(b, "}, %d)", argc);
      }
      else if ((sp_streq(name, "tr") || sp_streq(name, "tr_s")) && argc == 2) {
        buf_printf(b, "sp_str_%s(%s, ", name, r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "delete") && argc == 0) { buf_printf(b, "(%s)", r); return 1; }
      else if (sp_streq(name, "delete") && argc == 1) { buf_printf(b, "sp_str_delete(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "delete") && argc >= 2) {
        buf_printf(b, "sp_str_delete_n(%s, (const char *[]){", r);
        for (int a = 0; a < argc; a++) { if (a) buf_puts(b, ", "); emit_expr(c, argv[a], b); }
        buf_printf(b, "}, %d)", argc);
      }
      else if (sp_streq(name, "count") && argc == 0) { buf_printf(b, "(sp_raise_cls(\"TypeError\", \"no implicit conversion of nil into String\"), 0LL)"); return 1; }
      else if (sp_streq(name, "count") && argc == 1) { buf_printf(b, "sp_str_count(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "count") && argc >= 2) {
        buf_printf(b, "sp_str_count_n(%s, (const char *[]){", r);
        for (int a = 0; a < argc; a++) { if (a) buf_puts(b, ", "); emit_expr(c, argv[a], b); }
        buf_printf(b, "}, %d)", argc);
      }
      else if (sp_streq(name, "lines") && argc == 0) buf_printf(b, "sp_str_lines(%s)", r);
      else if (sp_streq(name, "lines") && argc == 1 && nt_type(nt, argv[0]) &&
               sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) {
        int chomp_v = struct_kwarg_value(c, argv[0], "chomp");
        int is_chomp = (chomp_v >= 0 && nt_type(nt, chomp_v) &&
                        sp_streq(nt_type(nt, chomp_v), "TrueNode"));
        buf_printf(b, "%s(%s)", is_chomp ? "sp_str_lines_chomp" : "sp_str_lines", r);
      }
      else if (sp_streq(name, "bytes") && argc == 0)   buf_printf(b, "sp_str_bytes(%s)", r);
      else if (sp_streq(name, "codepoints") && argc == 0) buf_printf(b, "sp_str_codepoints(%s)", r);
      else if (sp_streq(name, "unpack") && argc == 1)  { buf_printf(b, "sp_str_unpack(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "unpack1") && argc == 1) {
        /* A literal single-directive integer format fixes the value's type
           (the analyzer's an_unpack1_lit_type): unbox the extracted element. */
        TyKind u1t = comp_ntype(c, id);
        if (u1t == TY_INT) buf_printf(b, "sp_poly_to_i(sp_PolyArray_get(sp_str_unpack(%s, ", r);
        else               buf_printf(b, "sp_PolyArray_get(sp_str_unpack(%s, ", r);
        emit_expr(c, argv[0], b);
        buf_puts(b, u1t == TY_INT ? "), 0))" : "), 0)");
      }
      else if (sp_streq(name, "sum") && argc == 0) {
        /* default 16-bit byte checksum: sum of byte values modulo 2**16 */
        int ts = ++g_tmp, tp = ++g_tmp, tacc = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = %s; mrb_int _t%d = 0; for (const char *_t%d = _t%d; *_t%d; _t%d++)"
                      " _t%d += (unsigned char)*_t%d; _t%d & 0xffff; })", ts, r, tacc, tp, ts, tp, tp, tacc, tp, tacc);
      }
      else if (sp_streq(name, "chars") && argc == 0)   buf_printf(b, "sp_str_chars(%s)", r);
      else if ((sp_streq(name, "succ") || sp_streq(name, "next")) && argc == 0) buf_printf(b, "sp_str_succ(%s)", r);
      else if (sp_streq(name, "to_i") && argc == 0)    buf_printf(b, "sp_str_to_i_cruby(%s)", r);
      else if (sp_streq(name, "to_i") && argc == 1)    { buf_printf(b, "sp_str_to_i_base(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "to_f") && argc == 0)    buf_printf(b, "atof(%s)", r);
      else if (sp_streq(name, "gsub") && argc == 2) {
        buf_printf(b, "sp_str_gsub(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "sub") && argc == 2 && comp_ntype(c, argv[1]) == TY_STR_STR_HASH) {
        buf_printf(b, "sp_str_sub_str_str_hash(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "sub") && argc == 2) {
        buf_printf(b, "sp_str_sub(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "tr") && argc == 2) {
        buf_printf(b, "sp_str_tr(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "center") && argc == 1) {
        buf_printf(b, "sp_str_center(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "center") && argc == 2) {
        buf_printf(b, "sp_str_center2(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "ljust") && argc == 1) {
        buf_printf(b, "sp_str_ljust(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "ljust") && argc == 2) {
        buf_printf(b, "sp_str_ljust2(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "rjust") && argc == 1) {
        buf_printf(b, "sp_str_rjust(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "rjust") && argc == 2) {
        buf_printf(b, "sp_str_rjust2(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      /* String#eql?(x): byte-equal only when x is itself String-typed (no
         coercion, unlike ==). A poly arg checks its tag; any other concrete
         type is never equal. */
      else if (sp_streq(name, "eql?") && argc == 1) {
        if (a0 == TY_STRING) { buf_printf(b, "sp_str_eq(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else if (a0 == TY_POLY) {
          int te = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", te); emit_boxed(c, argv[0], b);
          buf_printf(b, "; _t%d.tag == SP_TAG_STR && sp_str_eq(_t%d.v.s, %s); })", te, te, r);
        }
        else { buf_puts(b, "(("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); }
      }
      /* String#equal?(x): object identity. A String is a `const char *` whose
         literals the C compiler merges at -O2, so raw pointer equality would
         wrongly equate distinct equal-valued literals (`a = "x"; b = "x"`).
         Only the unambiguous reflexive case -- the same side-effect-free local
         or ivar read on both sides (`x.equal?(x)`) -- is certainly identity-
         true; every other form is conservatively false, still evaluating the
         argument for its side effects. */
      else if (sp_streq(name, "equal?") && argc == 1) {
        if (same_sefree_lvalue(c, recv, argv[0])) { buf_puts(b, "(("); emit_expr(c, argv[0], b); buf_puts(b, "), 1)"); }
        else { buf_puts(b, "(("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); }
      }
      else handled = 0;
    }
    else if (rt == TY_INT) {
      /* a nullable int's to_s/inspect tests the value and converts it -- bind
         the receiver to a temp first so a side-effecting `r` (e.g. ARGF.read,
         a method call) is evaluated exactly once, not twice. */
      if (sp_streq(name, "to_s") && argc == 0) {
        int _tn = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = (%s); _t%d == SP_INT_NIL ? SPL(\"\") : sp_int_to_s(_t%d); })", _tn, r, _tn, _tn);
      }
      else if (sp_streq(name, "inspect")) {
        int _tn = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = (%s); _t%d == SP_INT_NIL ? SPL(\"nil\") : sp_int_to_s(_t%d); })", _tn, r, _tn, _tn);
      }
      else if (sp_streq(name, "to_f"))   buf_printf(b, "((mrb_float)(%s))", r);
      else if ((sp_streq(name, "to_i") || sp_streq(name, "to_int") || sp_streq(name, "floor") ||
                sp_streq(name, "ceil") || sp_streq(name, "round") || sp_streq(name, "truncate")) &&
               argc == 0) buf_printf(b, "(%s)", r);
      else if ((sp_streq(name, "floor") || sp_streq(name, "ceil") ||
                sp_streq(name, "round") || sp_streq(name, "truncate")) && argc == 1) {
        buf_printf(b, "sp_int_%s(%s, ", name, r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "abs"))    buf_printf(b, "((%s) < 0 ? -(%s) : (%s))", r, r, r);
      else if (sp_streq(name, "chr"))    buf_printf(b, "sp_int_chr(%s)", r);
      else if (sp_streq(name, "[]") && argc == 1) { buf_printf(b, "(((%s) >> (", r); emit_expr(c, argv[0], b); buf_puts(b, ")) & 1)"); }
      else if (sp_streq(name, "bit_length") && argc == 0) buf_printf(b, "sp_int_bit_length(%s)", r);
      else if (sp_streq(name, "fdiv") && argc == 1) { buf_printf(b, "((mrb_float)(%s) / (", r); emit_float_expr(c, argv[0], b); buf_puts(b, "))"); }
      else if (sp_streq(name, "[]") && argc == 2) {
        /* n[start, len]: the len-bit field starting at bit `start`. Routed
           through a runtime helper that clamps an out-of-range start/len so
           the shift never goes undefined. */
        buf_printf(b, "sp_int_bit_range((%s), ", r); emit_int_expr(c, argv[0], b);
        buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "even?"))  buf_printf(b, "((%s) %% 2 == 0)", r);
      else if (sp_streq(name, "odd?"))   buf_printf(b, "((%s) %% 2 != 0)", r);
      else if (sp_streq(name, "zero?"))  buf_printf(b, "((%s) == 0)", r);
      else if (sp_streq(name, "nonzero?")) buf_printf(b, "((%s) == 0 ? SP_INT_NIL : (%s))", r, r);
      else if (sp_streq(name, "positive?")) buf_printf(b, "((%s) > 0)", r);
      else if (sp_streq(name, "negative?")) buf_printf(b, "((%s) < 0)", r);
      else if (sp_streq(name, "divmod") && argc == 1) {
        int tb = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = ", tb); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; sp_IntArray *_t%d = sp_IntArray_new(); sp_IntArray_push(_t%d, sp_idiv(%s, _t%d));"
                      " sp_IntArray_push(_t%d, sp_imod(%s, _t%d)); _t%d; })", o, o, r, tb, o, r, tb, o);
      }
      else if (sp_streq(name, "div") && argc == 1) { buf_printf(b, "sp_idiv(%s, ", r); emit_int_divisor(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "gcd") && argc == 1) { buf_printf(b, "sp_gcd(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "lcm") && argc == 1) { buf_printf(b, "sp_lcm(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "magnitude") && argc == 0) buf_printf(b, "((%s) < 0 ? -(%s) : (%s))", r, r, r);
      else if (sp_streq(name, "modulo") && argc == 1) { buf_printf(b, "sp_imod(%s, ", r); emit_int_divisor(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "remainder") && argc == 1) { buf_printf(b, "sp_iremainder(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "size") && argc == 0) buf_puts(b, "((mrb_int)sizeof(mrb_int))");
      else if (sp_streq(name, "gcdlcm") && argc == 1) {
        int ta = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = ", ta); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; sp_IntArray *_t%d = sp_IntArray_new(); sp_IntArray_push(_t%d, sp_gcd(%s, _t%d));"
                      " sp_IntArray_push(_t%d, sp_lcm(%s, _t%d)); _t%d; })", o, o, r, ta, o, r, ta, o);
      }
      /* A Float (or runtime-typed poly) bound makes the applied bound or the
         in-range receiver decide the result class at runtime, so box the
         operands and return whichever is chosen unchanged via sp_num_clamp. */
      else if (sp_streq(name, "clamp") && argc == 2 &&
               (comp_ntype(c, argv[0]) == TY_FLOAT || comp_ntype(c, argv[1]) == TY_FLOAT ||
                comp_ntype(c, argv[0]) == TY_POLY || comp_ntype(c, argv[1]) == TY_POLY)) {
        buf_printf(b, "sp_num_clamp(sp_box_int(%s), ", r); emit_boxed(c, argv[0], b); buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "clamp") && argc == 2) { buf_printf(b, "sp_int_clamp_ck(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "clamp") && argc == 1 && nt_type(c->nt, argv[0]) && sp_streq(nt_type(c->nt, argv[0]), "RangeNode")) {
        /* the helper raises on an exclusive range with a real end (CRuby) */
        buf_printf(b, "sp_int_clamp_range_ck(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "digits") && argc == 0) buf_printf(b, "sp_int_digits(%s, 10)", r);
      else if (sp_streq(name, "digits") && argc == 1) { buf_printf(b, "sp_int_digits(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "allbits?") && argc == 1) { int t = ++g_tmp; buf_printf(b, "({ mrb_int _t%d = ", t); emit_int_expr(c, argv[0], b); buf_printf(b, "; (((%s) & _t%d) == _t%d); })", r, t, t); }
      else if (sp_streq(name, "anybits?") && argc == 1) { buf_printf(b, "(((%s) & (", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")) != 0)"); }
      else if (sp_streq(name, "nobits?") && argc == 1) { buf_printf(b, "(((%s) & (", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")) == 0)"); }
      else if (sp_streq(name, "ceildiv") && argc == 1) { buf_printf(b, "sp_ceildiv(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "pow") && argc == 2) { buf_printf(b, "sp_powmod(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "pow") && argc == 1) { buf_printf(b, "sp_int_pow(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "pred") && argc == 0) buf_printf(b, "((%s) - 1)", r);
      else if ((sp_streq(name, "succ") || sp_streq(name, "next")) && argc == 0) buf_printf(b, "((%s) + 1)", r);
      else if (sp_streq(name, "to_s") && argc == 1) { buf_printf(b, "sp_int_to_s_base(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "coerce") && argc == 1) {
        TyKind a0 = comp_ntype(c, argv[0]);
        if (a0 == TY_FLOAT) {
          int ta = ++g_tmp, o = ++g_tmp;
          buf_printf(b, "({ mrb_float _t%d = ", ta); emit_expr(c, argv[0], b);
          buf_printf(b, "; sp_FloatArray *_t%d = sp_FloatArray_new();"
                        " sp_FloatArray_push(_t%d, _t%d);"
                        " sp_FloatArray_push(_t%d, (mrb_float)(%s)); _t%d; })", o, o, ta, o, r, o);
        }
        else {
          int ta = ++g_tmp, o = ++g_tmp;
          buf_printf(b, "({ mrb_int _t%d = ", ta); emit_int_expr(c, argv[0], b);
          buf_printf(b, "; sp_IntArray *_t%d = sp_IntArray_new();"
                        " sp_IntArray_push(_t%d, _t%d);"
                        " sp_IntArray_push(_t%d, (%s)); _t%d; })", o, o, ta, o, r, o);
        }
      }
      /* Integer#eql?/equal?(x): value-equal only when x is itself Integer-typed
         (no numeric coercion -- 1.eql?(1.0) is false). For a fixnum receiver
         equal? is value identity, so it behaves the same as eql?. A Float or
         any other concrete arg is never equal; a poly arg checks its tag. */
      else if ((sp_streq(name, "eql?") || sp_streq(name, "equal?")) && argc == 1) {
        if (a0 == TY_INT) { buf_printf(b, "((%s) == (", r); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
        else if (a0 == TY_POLY) {
          int te = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", te); emit_boxed(c, argv[0], b);
          buf_printf(b, "; _t%d.tag == SP_TAG_INT && _t%d.v.i == (%s); })", te, te, r);
        }
        else { buf_puts(b, "(("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); }
      }
      else handled = 0;
    }
    else { /* TY_FLOAT */
      /* round/ceil/floor/truncate(n>0) -> Float to n decimals; else Integer.
         A non-literal ndigits can't be classified statically; compute the exact
         value at runtime, typed Float (see infer_method_name_type / FLOAT-ROUNDING). */
      int ndig = 0;
      int nonlit = 0;
      if ((sp_streq(name, "floor") || sp_streq(name, "ceil") ||
           sp_streq(name, "round") || sp_streq(name, "truncate")) && argc == 1) {
        const char *aty = nt_type(c->nt, argv[0]);
        if (aty && sp_streq(aty, "IntegerNode")) ndig = (int)nt_int(c->nt, argv[0], "value", 0);
        else nonlit = 1;
      }
      const char *cfn = sp_streq(name, "floor") ? "floor" : sp_streq(name, "ceil") ? "ceil"
                      : sp_streq(name, "truncate") ? "trunc" : "round";
      if ((sp_streq(name, "floor") || sp_streq(name, "ceil") ||
           sp_streq(name, "round") || sp_streq(name, "truncate"))) {
        if (nonlit) {
          /* _f = 10**n at runtime; round(x * _f) / _f gives the right value for
             any n (n<=0 yields a whole number, still Float -- class-only divergence). */
          int tf = ++g_tmp;
          buf_printf(b, "({ double _t%d = pow(10, (double)(", tf);
          emit_int_expr(c, argv[0], b);   /* ndigits: unbox a poly arg to int */
          buf_printf(b, ")); %s((%s) * _t%d) / _t%d; })", cfn, r, tf, tf);
        }
        else if (ndig > 0)
          buf_printf(b, "({ double _f = pow(10, %d); %s((%s) * _f) / _f; })", ndig, cfn, r);
        else if (ndig < 0)  /* round to a power of ten left of the decimal -> Integer */
          buf_printf(b, "({ double _f = pow(10, %d); (mrb_int)(%s((%s) / _f) * _f); })", -ndig, cfn, r);
        else
          buf_printf(b, "((mrb_int)%s(%s))", cfn, r);
      }
      else if (sp_streq(name, "to_i"))  buf_printf(b, "((mrb_int)(%s))", r);
      else if (sp_streq(name, "to_f"))  buf_printf(b, "(%s)", r);
      else if (sp_streq(name, "divmod") && argc == 1) {
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
      else if (sp_streq(name, "to_s"))    buf_printf(b, "sp_float_opt_to_s(%s)", r);
      else if (sp_streq(name, "inspect")) buf_printf(b, "sp_float_opt_inspect(%s)", r);
      else if (sp_streq(name, "to_r") && argc == 0) buf_printf(b, "sp_float_to_rational(%s)", r);
      else if (sp_streq(name, "rationalize") && argc == 0) buf_printf(b, "sp_float_rationalize0(%s)", r);
      else if (sp_streq(name, "rationalize") && argc == 1) {
        buf_printf(b, "sp_float_rationalize(%s, ", r); emit_float_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "abs"))   buf_printf(b, "((%s) < 0 ? -(%s) : (%s))", r, r, r);
      else if (sp_streq(name, "zero?")) buf_printf(b, "((%s) == 0.0)", r);
      else if (sp_streq(name, "nan?"))  buf_printf(b, "(isnan(%s) != 0)", r);
      else if (sp_streq(name, "finite?")) buf_printf(b, "(isfinite(%s) != 0)", r);
      else if (sp_streq(name, "infinite?")) buf_printf(b, "(isinf(%s) ? ((%s) > 0 ? 1LL : -1LL) : SP_INT_NIL)", r, r);
      else if (sp_streq(name, "positive?")) buf_printf(b, "((%s) > 0)", r);
      else if (sp_streq(name, "negative?")) buf_printf(b, "((%s) < 0)", r);
      else if (sp_streq(name, "next_float")) buf_printf(b, "nextafter(%s, INFINITY)", r);
      else if (sp_streq(name, "prev_float")) buf_printf(b, "nextafter(%s, -INFINITY)", r);
      else if (sp_streq(name, "magnitude")) buf_printf(b, "((%s) < 0 ? -(%s) : (%s))", r, r, r);
      else if (sp_streq(name, "modulo") && argc == 1) { buf_printf(b, "sp_fmod(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      /* Float#clamp with float bounds always yields a float (the returned bound
         is itself a float), so emit only when both bounds are float-typed; the
         mixed-bound case (int bound returned as Integer) is poly and left alone.
         Mirrors the inference condition in analyze_infer.c. */
      else if (sp_streq(name, "clamp") && argc == 2 &&
               comp_ntype(c, argv[0]) == TY_FLOAT && comp_ntype(c, argv[1]) == TY_FLOAT) {
        buf_printf(b, "sp_float_clamp_ck(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "coerce") && argc == 1) {
        TyKind a0 = comp_ntype(c, argv[0]);
        int ta = ++g_tmp, o = ++g_tmp;
        if (a0 == TY_INT) {
          buf_printf(b, "({ mrb_int _t%d = ", ta); emit_int_expr(c, argv[0], b);
          buf_printf(b, "; sp_FloatArray *_t%d = sp_FloatArray_new();"
                        " sp_FloatArray_push(_t%d, (mrb_float)_t%d);"
                        " sp_FloatArray_push(_t%d, (%s)); _t%d; })", o, o, ta, o, r, o);
        }
        else {
          buf_printf(b, "({ mrb_float _t%d = ", ta); emit_expr(c, argv[0], b);
          buf_printf(b, "; sp_FloatArray *_t%d = sp_FloatArray_new();"
                        " sp_FloatArray_push(_t%d, _t%d);"
                        " sp_FloatArray_push(_t%d, (%s)); _t%d; })", o, o, ta, o, r, o);
        }
      }
      else if (sp_streq(name, "fdiv") && argc == 1) { buf_printf(b, "((%s) / (", r); emit_float_expr(c, argv[0], b); buf_puts(b, "))"); }
      /* Float#eql?(x): true only when x is itself a Float of equal value (no
         numeric coercion, unlike ==). A float-typed arg compares directly; any
         other arg is boxed and rejected unless it is tagged float at runtime. */
      else if (sp_streq(name, "eql?") && argc == 1) {
        TyKind a0 = comp_ntype(c, argv[0]);
        if (a0 == TY_FLOAT) { buf_printf(b, "((%s) == (", r); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
        else {
          int te = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", te); emit_boxed(c, argv[0], b);
          buf_printf(b, "; _t%d.tag == SP_TAG_FLT && _t%d.v.f == (%s); })", te, te, r);
        }
      }
      else handled = 0;
    }
    free(rs.p);
    if (handled) return 1;
  }
  return 0;
}

int emit_object_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  TyKind res = comp_ntype(c, id);
  /* obj.is_a?/kind_of?/instance_of?(Class): resolved via sp_class_le for
     correctness with module includes; falls back to constant for builtins. */
  if (recv >= 0 && ty_is_object(rt) && argc == 1 &&
      (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") || sp_streq(name, "instance_of?"))) {
    const char *cn = nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ConstantReadNode")
                     ? nt_str(nt, argv[0], "name") : NULL;
    if (cn) {
      int cid = ty_object_class(rt);
      int target = comp_class_index(c, cn);
      if (target >= 0) {
        if (sp_streq(name, "instance_of?")) {
          buf_puts(b, "((void)("); emit_expr(c, recv, b);
          buf_printf(b, "), %d)", cid == target);
        }
        else {
          /* use sp_class_le_mod (via macro) so includes chain is checked */
          buf_puts(b, "((void)("); emit_expr(c, recv, b);
          buf_printf(b, "), sp_class_le(((sp_Class){%d}),((sp_Class){%d})))", cid, target);
        }
        return 1;
      }
      else {
        buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), 0)");
        return 1;
      }
    }
    /* Dynamic klass argument typed as TY_CLASS: runtime sp_class_le check */
    if (comp_ntype(c, argv[0]) == TY_CLASS) {
      int cid = ty_object_class(rt);
      int k = ++g_tmp;
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), ");
      buf_printf(b, "({ sp_Class _t%d = ", k); emit_expr(c, argv[0], b); buf_printf(b, "; ");
      if (sp_streq(name, "instance_of?"))
        buf_printf(b, "((sp_Class){%d}).cls_id == _t%d.cls_id; }))", cid, k);
      else
        buf_printf(b, "sp_class_le(((sp_Class){%d}),_t%d); }))", cid, k);
      return 1;
    }
  }

  /* Comparable#clamp(lo, hi) on a user object: dispatch the user `<=>` through
     sp_obj_clamp. The result is self or the APPLIED BOUND, so it keeps the
     receiver's class only when both bounds are statically that class (the
     inference arm matches); otherwise it stays boxed. */
  if (recv >= 0 && ty_is_object(rt) && sp_streq(name, "clamp") && argc == 2 &&
      comp_method_in_chain(c, ty_object_class(rt), "<=>", NULL) >= 0) {
    TyKind clo = comp_ntype(c, argv[0]), chi = comp_ntype(c, argv[1]);
    int same_cls = (clo == rt || clo == TY_NIL) && (chi == rt || chi == TY_NIL);
    if (same_cls) { buf_puts(b, "(("); emit_ctype(c, rt, b); buf_puts(b, ")"); }
    buf_puts(b, "sp_obj_clamp(");
    emit_boxed(c, recv, b); buf_puts(b, ", ");
    emit_boxed(c, argv[0], b); buf_puts(b, ", ");
    emit_boxed(c, argv[1], b);
    buf_puts(b, ")");
    if (same_cls) buf_puts(b, ".v.p)");
    return 1;
  }
  /* Comparable#clamp(range) on a user object: int endpoints become bounds fed
     to the user `<=>`; beginless/endless clamp one-sided; an exclusive range
     with a real end raises (CRuby). A clamped result IS the Integer endpoint
     itself, so the value stays boxed (inference: TY_POLY). */
  if (recv >= 0 && ty_is_object(rt) && sp_streq(name, "clamp") && argc == 1 &&
      comp_ntype(c, argv[0]) == TY_RANGE &&
      comp_method_in_chain(c, ty_object_class(rt), "<=>", NULL) >= 0) {
    buf_puts(b, "sp_obj_clamp_range(");
    emit_boxed(c, recv, b); buf_puts(b, ", ");
    emit_expr(c, argv[0], b);
    buf_puts(b, ")");
    return 1;
  }

  /* Struct instance methods (to_h / to_a / values / members / dig). */
  if (recv >= 0 && ty_is_object(rt) && c->classes[ty_object_class(rt)].is_struct) {
    ClassInfo *sc = &c->classes[ty_object_class(rt)];
    /* #inspect / #to_s -> the generated (or user-overridden) struct/data stringifier */
    if ((sp_streq(name, "inspect") || sp_streq(name, "to_s")) && argc == 0) {
      const char *cn = obj_str_cname(c, ty_object_class(rt), sp_streq(name, "inspect"));
      if (cn) { buf_printf(b, "sp_%s_%s((sp_%s *)", cn, name, cn); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
    }
    int is_to_a = (sp_streq(name, "to_a") || sp_streq(name, "values") || sp_streq(name, "deconstruct"));
    if (is_to_a && argc == 0) {
      int t = ++g_tmp; int rt2 = ++g_tmp;
      Buf rb = expr_buf(c, recv);
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
      return 1;
    }
    if (sp_streq(name, "to_h") && argc == 0) {
      int block = nt_ref(nt, id, "block");
      int t = ++g_tmp, rh = ++g_tmp;
      Buf rb = expr_buf(c, recv);
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
        if (last >= 0 && nt_type(nt, last) && sp_streq(nt_type(nt, last), "ArrayNode")) {
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
      return 1;
    }
    if ((sp_streq(name, "members")) && argc == 0) {
      int rm = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", rm, rm);
      for (int i = 0; i < sc->nivars; i++)
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym((sp_sym)%d));", rm, comp_sym_intern(c, sc->ivars[i] + 1));
      buf_printf(b, " _t%d; })", rm);
      return 1;
    }
    if (sp_streq(name, "with") && sc->is_data) {
      /* Data#with copy-update: a new instance with the given members
         overridden, the rest copied from the receiver. Members are passed to
         the generated constructor in declaration order. */
      int wargs = nt_ref(nt, id, "arguments");
      int wargc = 0; const int *wargv = wargs >= 0 ? nt_arr(nt, wargs, "arguments", &wargc) : NULL;
      int wkwh = -1;
      if (wargv && wargc >= 1) {
        const char *lty = nt_type(nt, wargv[wargc - 1]);
        if (lty && sp_streq(lty, "KeywordHashNode")) wkwh = wargv[wargc - 1];
      }
      /* Data#with takes keyword arguments only; a positional argument (the only
         arg, or one alongside the keyword hash) is an ArgumentError in CRuby. */
      if (wargc > 0 && (wkwh < 0 || wargc > 1)) {
        unsupported(c, id, "Data#with with a positional argument (keywords only)");
        return 0;
      }
      if (wkwh >= 0) {
        int en = 0; const int *els = nt_arr(nt, wkwh, "elements", &en);
        for (int e = 0; e < en; e++) {
          int key = nt_ref(nt, els[e], "key");
          const char *kty = key >= 0 ? nt_type(nt, key) : NULL;
          const char *kn = (kty && sp_streq(kty, "SymbolNode")) ? nt_str(nt, key, "value") : NULL;
          char ivn[256];
          if (kn) snprintf(ivn, sizeof ivn, "@%s", kn);
          if (!kn || comp_ivar_index(sc, ivn) < 0) { unsupported(c, id, "with on a non-member keyword"); return 0; }
        }
      }
      int t = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      buf_printf(b, "({ sp_%s *_t%d = %s; sp_%s_new(", sc->name, t, rb.p ? rb.p : "", sc->name); free(rb.p);
      for (int i = 0; i < sc->nivars; i++) {
        if (i) buf_puts(b, ", ");
        int val = wkwh >= 0 ? kwh_lookup(nt, wkwh, sc->ivars[i] + 1) : -1;
        if (val >= 0) {
          TyKind mt = sc->ivar_types[i];
          if (mt == TY_POLY && comp_ntype(c, val) != TY_POLY) emit_boxed(c, val, b);
          else emit_expr(c, val, b);
        } else {
          buf_printf(b, "_t%d->iv_%s", t, sc->ivars[i] + 1);
        }
      }
      buf_puts(b, "); })");
      return 1;
    }
    if (sp_streq(name, "dig") && argc >= 1) {
      /* literal key resolves a member at compile time */
      int mi = -1;
      const char *kty = nt_type(nt, argv[0]);
      if (kty && sp_streq(kty, "SymbolNode")) {
        char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", nt_str(nt, argv[0], "value"));
        mi = comp_ivar_index(sc, ivn);
      }
      else if (kty && sp_streq(kty, "IntegerNode")) {
        int v = (int)nt_int(nt, argv[0], "value", -1);
        if (v >= 0 && v < sc->nivars) mi = v;
      }
      if (mi >= 0) {
        int t = ++g_tmp;
        Buf rb = expr_buf(c, recv);
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
        return 1;
      }
    }
    if (sp_streq(name, "[]") && argc == 1) {
      /* struct[:sym] or struct[int_literal]: return member value boxed to poly */
      int mi = -1;
      const char *kty = nt_type(nt, argv[0]);
      if (kty && sp_streq(kty, "SymbolNode") && nt_str(nt, argv[0], "value")) {
        char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", nt_str(nt, argv[0], "value"));
        mi = comp_ivar_index(sc, ivn);
      }
      else if (kty && sp_streq(kty, "IntegerNode")) {
        long long v = (long long)nt_int(nt, argv[0], "value", 0);
        if (v < 0) v += (long long)sc->nivars;
        if (v >= 0 && v < sc->nivars) mi = (int)v;
      }
      if (mi >= 0) {
        int t = ++g_tmp;
        Buf rb = expr_buf(c, recv);
        buf_printf(b, "({ sp_%s *_t%d = %s; ", sc->name, t, rb.p ? rb.p : ""); free(rb.p);
        buf_printf(b, "_t%d->iv_%s; })", t, sc->ivars[mi] + 1);
        return 1;
      }
      /* general: generate chain of comparisons */
      if (sc->nivars > 0) {
        int t = ++g_tmp, tk = ++g_tmp;
        Buf rb = expr_buf(c, recv);
        buf_printf(b, "({ sp_%s *_t%d = %s; sp_RbVal _t%d = ", sc->name, t, rb.p ? rb.p : "", tk);
        free(rb.p);
        emit_boxed(c, argv[0], b);
        buf_puts(b, ";");
        for (int i = 0; i < sc->nivars; i++) {
          buf_printf(b, " if(sp_rbval_eql_key(_t%d,sp_box_sym((sp_sym)%d))||sp_rbval_eql_key(_t%d,sp_box_int(%dLL))){",
                     tk, comp_sym_intern(c, sc->ivars[i]+1), tk, (long long)i);
          char fld2[300]; snprintf(fld2, sizeof fld2, "_t%d->iv_%s", t, sc->ivars[i] + 1);
          emit_boxed_text(c, sc->ivar_types[i], fld2, b);
          buf_printf(b, ";} else");
        }
        buf_puts(b, " sp_box_nil(); })");
        return 1;
      }
    }
  }

  /* object method call: sp_<DefClass>_<m>((sp_<DefClass>*)&recv, args) */
  if (recv >= 0 && ty_is_object(rt)) {
    int cid = ty_object_class(rt);
    /* undef'd method: raise NoMethodError */
    if (comp_is_undeffed_in_chain(c, cid, name)) {
      TyKind ret_ty = comp_ntype(c, id);
      buf_printf(b, "(sp_raise_cls(\"NoMethodError\",\"undefined method '%s' for an instance of %s\"),%s)",
                 name, c->classes[cid].name,
                 ret_ty == TY_RANGE ? "(sp_Range){0}" : default_value(ret_ty));
      return 1;
    }
    /* instance_variable_get(:@x) / instance_variable_set(:@x, v) with a literal
       symbol or string name. A name present in the known layout lowers to a
       direct field read/write. An undefined-but-valid `@`-name reads as nil
       (get), matching CRuby; a name without a leading `@` raises NameError at
       runtime, also matching CRuby. A dynamic name -- or instance_variable_set
       to a valid name absent from the fixed object layout (no field to write) --
       cannot be represented and is diagnosed. */
    if ((sp_streq(name, "instance_variable_get") || sp_streq(name, "instance_variable_set")) &&
        argc >= 1 && nt_type(nt, argv[0]) &&
        (sp_streq(nt_type(nt, argv[0]), "SymbolNode") || sp_streq(nt_type(nt, argv[0]), "StringNode"))) {
      const char *a0ty = nt_type(nt, argv[0]);
      const char *sym = sp_streq(a0ty, "SymbolNode")
                          ? nt_str(nt, argv[0], "value") : nt_str(nt, argv[0], "content");
      int is_set = sp_streq(name, "instance_variable_set");
      /* Arity is statically known: get takes just the name, set the name and a
         value. A wrong count is a clear diagnostic rather than falling through to
         the misleading by-value-receiver message below. */
      if (is_set && argc != 2) { unsupported(c, id, "instance_variable_set takes exactly 2 arguments"); return 1; }
      if (!is_set && argc != 1) { unsupported(c, id, "instance_variable_get takes exactly 1 argument"); return 1; }
      int is_val = comp_ty_value_obj(c, rt);
      const char *rty = nt_type(nt, recv);
      int recv_lvalue = rty && (sp_streq(rty, "LocalVariableReadNode") ||
                                sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "SelfNode"));
      /* A name without a leading `@` is never a valid ivar name: raise NameError
         at runtime (evaluating the receiver first for its side effects). */
      if (!sym || sym[0] != '@') {
        if (recv >= 0) { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), "); }
        else buf_puts(b, "(");
        buf_printf(b, "sp_raise_cls(\"NameError\", \"'%s' is not allowed as an instance variable name\"), sp_box_nil())",
                   sym ? sym : "");
        return 1;
      }
      int mi = -1;
      for (int i = 0; i < c->classes[cid].nivars; i++)
        if (sp_streq(c->classes[cid].ivars[i], sym)) { mi = i; break; }
      if (mi >= 0) {
        /* A value object is passed by value, so a field write only sticks when
           the receiver is an lvalue (a local / ivar / self); a pointer object
           can be mutated through any reference. */
        if (is_set && is_val && !recv_lvalue) {
          unsupported(c, id, "instance_variable_set on a by-value object requires an lvalue receiver");
          return 1;
        }
        TyKind mt = c->classes[cid].ivar_types[mi];
        const char *acc = is_val ? "." : "->";
        if (is_set) {
          buf_puts(b, "(("); emit_expr(c, recv, b);
          buf_printf(b, ")%siv_%s = ", acc, sym + 1);
          if (mt == TY_POLY) emit_boxed(c, argv[1], b); else emit_expr(c, argv[1], b);
          buf_puts(b, ")");
        }
        else {
          buf_puts(b, "("); emit_expr(c, recv, b);
          buf_printf(b, ")%siv_%s", acc, sym + 1);
        }
        return 1;
      }
      /* A valid `@`-name not in the layout: get reads as nil (CRuby returns nil
         for an unset ivar); set has no field to write under the fixed layout. */
      if (is_set) {
        unsupported(c, id, "instance_variable_set to an ivar absent from the fixed object layout");
        return 1;
      }
      if (recv >= 0) { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), sp_box_nil())"); }
      else buf_puts(b, "sp_box_nil()");
      return 1;
    }

    /* attr reader -> field access (recv).iv_x, UNLESS an explicit method of
       the same name overrides it at an equal-or-more-derived class. CRuby:
       attr_reader defines an ordinary method, so a subclass `def x` (or a
       same-class `def x`) overrides it via normal dispatch rather than
       reading the field. Whichever definition sits in the more-derived class
       wins; on a same-class tie the explicit method wins. */
    int rdc = -1, mdc = -1;
    if (comp_reader_in_chain(c, cid, name, &rdc)) {
      int reader_wins = 1;
      if (comp_method_in_chain(c, cid, name, &mdc) >= 0) {
        for (int k = cid; k >= 0; k = c->classes[k].parent) {
          if (k == mdc) { reader_wins = 0; break; }
          if (k == rdc) { reader_wins = 1; break; }
        }
      }
      if (reader_wins) {
        const char *rn2 = comp_resolve_alias(c, cid, name);
        buf_puts(b, "("); emit_expr(c, recv, b);
        buf_printf(b, ")%siv_%s", comp_ty_value_obj(c, rt) ? "." : "->", rn2);
        return 1;
      }
    }
    int mi = comp_method_in_chain(c, cid, name, NULL);
    if (mi >= 0) {
      /* a value-type receiver is passed by value; an ordinary object by
         pointer. For a value recv we hand emit_dispatch the value expression
         (lvalue or hoisted temp); the method takes `self` by value. */
      if (comp_ty_value_obj(c, rt)) {
        char selfv[64];
        const char *rty = nt_type(nt, recv);
        if (rty && (sp_streq(rty, "LocalVariableReadNode") || sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "SelfNode"))) {
          Buf rb = expr_buf(c, recv);
          snprintf(selfv, sizeof selfv, "%s", rb.p ? rb.p : ""); free(rb.p);
        }
        else {
          int t = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = ", t); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
          snprintf(selfv, sizeof selfv, "_t%d", t);
        }
        emit_dispatch(c, cid, name, selfv, nt_ref(nt, id, "arguments"), nt_ref(nt, id, "block"), b);
        return 1;
      }
      /* receiver is a pointer; reuse it directly if it's a simple lvalue,
         else stash in a temp (the virtual-dispatch switch references it
         multiple times) */
      char selfptr[64];
      const char *rty = nt_type(nt, recv);
      if (rty && (sp_streq(rty, "LocalVariableReadNode") || sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "SelfNode"))) {
        Buf rb = expr_buf(c, recv);
        snprintf(selfptr, sizeof selfptr, "%s", rb.p ? rb.p : "");
        free(rb.p);
      }
      else {
        int t = ++g_tmp;
        /* emit the receiver first so any setup it pushes into g_pre is fully
           flushed before we write this temp's declaration line */
        Buf rb = expr_buf(c, recv);
        emit_indent(g_pre, g_indent);
        emit_ctype(c, rt, g_pre);
        buf_printf(g_pre, " _t%d = ", t);
        buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
        /* Root the hoisted receiver: a freshly constructed object (e.g.
           Scene.new.render(...)) must survive any GC the call triggers. */
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
        snprintf(selfptr, sizeof selfptr, "_t%d", t);
      }
      emit_dispatch(c, cid, name, selfptr, nt_ref(nt, id, "arguments"), nt_ref(nt, id, "block"), b);
      return 1;
    }
  }
  return 0;
}

int emit_value_recv_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  /* Time instance methods: sp_Time is a value -- splice the receiver once. */
  if (recv >= 0 && rt == TY_TIME) {
    Buf rs = expr_buf(c, recv);
    const char *r = rs.p ? rs.p : "";
    int done = 1;
    if (sp_streq(name, "utc") || sp_streq(name, "gmtime") || sp_streq(name, "getutc")) buf_printf(b, "sp_time_utc(%s)", r);
    else if (sp_streq(name, "localtime") || sp_streq(name, "getlocal")) buf_printf(b, "sp_time_localtime(%s)", r);
    else if (sp_streq(name, "year"))  buf_printf(b, "sp_time_year(%s)", r);
    else if (sp_streq(name, "mon") || sp_streq(name, "month")) buf_printf(b, "sp_time_mon(%s)", r);
    else if (sp_streq(name, "day") || sp_streq(name, "mday"))  buf_printf(b, "sp_time_mday(%s)", r);
    else if (sp_streq(name, "hour")) buf_printf(b, "sp_time_hour(%s)", r);
    else if (sp_streq(name, "min"))  buf_printf(b, "sp_time_min(%s)", r);
    else if (sp_streq(name, "sec"))  buf_printf(b, "sp_time_sec(%s)", r);
    else if (sp_streq(name, "wday")) buf_printf(b, "sp_time_wday(%s)", r);
    else if (sp_streq(name, "yday")) buf_printf(b, "sp_time_yday(%s)", r);
    else if (sp_streq(name, "to_i") || sp_streq(name, "tv_sec")) buf_printf(b, "(%s).tv_sec", r);
    else if (sp_streq(name, "to_f")) buf_printf(b, "((mrb_float)(%s).tv_sec + (mrb_float)(%s).tv_nsec / 1e9)", r, r);
    else if (sp_streq(name, "subsec")) buf_printf(b, "((mrb_float)(%s).tv_nsec / 1e9)", r);
    else if (sp_streq(name, "tv_usec") || sp_streq(name, "usec")) buf_printf(b, "((mrb_int)(%s).tv_nsec / 1000)", r);
    else if (sp_streq(name, "tv_nsec") || sp_streq(name, "nsec")) buf_printf(b, "((mrb_int)(%s).tv_nsec)", r);
    else if (sp_streq(name, "utc?") || sp_streq(name, "gmt?")) buf_printf(b, "((%s).is_utc != 0)", r);
    else if (sp_streq(name, "dst?") || sp_streq(name, "isdst")) buf_printf(b, "(sp_time_isdst(%s) != 0)", r);
    else if (sp_streq(name, "utc_offset") || sp_streq(name, "gmt_offset") || sp_streq(name, "gmtoff")) buf_printf(b, "sp_time_utc_offset(%s)", r);
    else if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) buf_printf(b, "sp_time_inspect_v(%s)", r);
    else if (sp_streq(name, "iso8601") && sp_feature_enabled("time")) buf_printf(b, "sp_time_iso8601(%s)", r);
    else if (sp_streq(name, "zone")) buf_printf(b, "sp_time_zone(%s)", r);
    else if (sp_streq(name, "class")) buf_puts(b, "SPL(\"Time\")");
    else if (sp_streq(name, "strftime") && argc == 1) { buf_printf(b, "sp_time_strftime(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if ((sp_streq(name, "+") || sp_streq(name, "-")) && argc == 1) {
      buf_printf(b, "sp_time_add(%s, %s(mrb_float)(", r, name[0] == '-' ? "-" : "");
      emit_expr(c, argv[0], b); buf_puts(b, "))");
    }
    else if ((sp_streq(name, "<") || sp_streq(name, ">") || sp_streq(name, "<=") ||
              sp_streq(name, ">=") || sp_streq(name, "==") || sp_streq(name, "!=")) && argc == 1) {
      int tt = ++g_tmp, tu = ++g_tmp;
      buf_puts(b, "({ sp_Time _t"); buf_printf(b, "%d = %s; sp_Time _t%d = ", tt, r, tu);
      emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_time_cmp(_t%d, _t%d) %s 0; })", tt, tu, name);
    }
    else if (sp_streq(name, "<=>") && argc == 1) {
      int tt = ++g_tmp, tu = ++g_tmp;
      buf_puts(b, "({ sp_Time _t"); buf_printf(b, "%d = %s; sp_Time _t%d = ", tt, r, tu);
      emit_expr(c, argv[0], b);
      buf_printf(b, "; (mrb_int)sp_time_cmp(_t%d, _t%d); })", tt, tu);
    }
    else done = 0;
    free(rs.p);
    if (done) return 1;
  }

  /* StringScanner instance methods. String-returning methods may yield NULL
     (nil) on a miss; the NULL-aware string output operators render that. */
  if (recv >= 0 && rt == TY_STRINGSCANNER) {
    Buf rs = expr_buf(c, recv);
    const char *r = rs.p ? rs.p : "";
    int done = 1;
    if ((sp_streq(name, "scan") || sp_streq(name, "check") || sp_streq(name, "scan_until")) &&
        argc == 1 && re_lit_index(c, argv[0]) >= 0) {
      buf_printf(b, "sp_StringScanner_%s(%s, sp_re_pat_%d)", name, r, re_lit_index(c, argv[0]));
    }
    else if (sp_streq(name, "matched")) buf_printf(b, "sp_StringScanner_matched(%s)", r);
    else if (sp_streq(name, "matched?")) buf_printf(b, "sp_StringScanner_matched_p(%s)", r);
    else if (sp_streq(name, "pre_match")) buf_printf(b, "sp_StringScanner_pre_match(%s)", r);
    else if (sp_streq(name, "post_match")) buf_printf(b, "sp_StringScanner_post_match(%s)", r);
    else if (sp_streq(name, "pos") || sp_streq(name, "charpos")) buf_printf(b, "sp_StringScanner_pos(%s)", r);
    else if (sp_streq(name, "pos=") && argc == 1) { buf_printf(b, "sp_StringScanner_pos_set(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (sp_streq(name, "rest")) buf_printf(b, "sp_StringScanner_rest(%s)", r);
    else if (sp_streq(name, "rest?")) buf_printf(b, "sp_StringScanner_rest_p(%s)", r);
    else if (sp_streq(name, "rest_size")) buf_printf(b, "sp_StringScanner_rest_size(%s)", r);
    else if (sp_streq(name, "string")) buf_printf(b, "sp_StringScanner_string(%s)", r);
    else if (sp_streq(name, "eos?")) buf_printf(b, "sp_StringScanner_eos_p(%s)", r);
    else if (sp_streq(name, "getch")) buf_printf(b, "sp_StringScanner_getch(%s)", r);
    else if (sp_streq(name, "peek") && argc == 1) { buf_printf(b, "sp_StringScanner_peek(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (sp_streq(name, "[]") && argc == 1) { buf_printf(b, "sp_StringScanner_aref(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (sp_streq(name, "reset")) buf_printf(b, "(sp_StringScanner_reset(%s), %s)", r, r);
    else if (sp_streq(name, "terminate")) buf_printf(b, "(sp_StringScanner_terminate(%s), %s)", r, r);
    else if (sp_streq(name, "unscan")) buf_printf(b, "(sp_StringScanner_unscan(%s), %s)", r, r);
    else done = 0;
    free(rs.p);
    if (done) return 1;
  }

  /* MatchData instance methods (sp_MatchData *, nullable on no-match). */
  if (recv >= 0 && rt == TY_MATCHDATA) {
    Buf rs = expr_buf(c, recv);
    const char *r = rs.p ? rs.p : "";
    if (sp_streq(name, "[]") && argc == 1) {
      /* A Symbol/String key selects a named capture group; an Integer key is a
         positional group (the existing path). */
      TyKind kt = comp_ntype(c, argv[0]);
      if (kt == TY_SYMBOL) { buf_printf(b, "sp_MatchData_aref_name(%s, sp_sym_to_s(", r); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
      else if (kt == TY_STRING) { buf_printf(b, "sp_MatchData_aref_name(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else { buf_printf(b, "sp_MatchData_aref(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    }
    else if (sp_streq(name, "named_captures") && argc == 0) buf_printf(b, "sp_md_named_captures(%s)", r);
    else if (sp_streq(name, "names") && argc == 0) buf_printf(b, "sp_MatchData_names(%s)", r);
    else if (sp_streq(name, "pre_match"))  buf_printf(b, "sp_MatchData_pre_match(%s)", r);
    else if (sp_streq(name, "post_match")) buf_printf(b, "sp_MatchData_post_match(%s)", r);
    else if (sp_streq(name, "to_s"))       buf_printf(b, "sp_MatchData_to_s(%s)", r);
    else if ((sp_streq(name, "length") || sp_streq(name, "size")) && argc == 0)
      buf_printf(b, "sp_MatchData_length(%s)", r);
    else if (sp_streq(name, "begin") && argc == 1) {
      buf_printf(b, "sp_MatchData_begin(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (sp_streq(name, "end") && argc == 1) {
      buf_printf(b, "sp_MatchData_end(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (sp_streq(name, "offset") && argc == 1) {
      buf_printf(b, "sp_MatchData_offset(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (sp_streq(name, "bytebegin") && argc == 1) {
      buf_printf(b, "sp_MatchData_bytebegin(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (sp_streq(name, "byteend") && argc == 1) {
      buf_printf(b, "sp_MatchData_byteend(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (sp_streq(name, "byteoffset") && argc == 1) {
      buf_printf(b, "sp_MatchData_byteoffset(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (sp_streq(name, "values_at") && argc >= 1) {
      /* values_at(i, ...) -> a poly array of the selected groups (nil when a
         group did not participate), mirroring MatchData#[] per index. */
      int mt = ++g_tmp, at = ++g_tmp;
      buf_printf(b, "({ sp_MatchData *_t%d = %s; SP_GC_ROOT(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);",
                 mt, r, mt, at, at);
      for (int i = 0; i < argc; i++) {
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_nullable_str(sp_MatchData_aref(_t%d, ", at, mt);
        emit_expr(c, argv[i], b);
        buf_puts(b, ")));");
      }
      buf_printf(b, " _t%d; })", at);
    }
    else if (sp_streq(name, "captures"))  buf_printf(b, "sp_MatchData_captures(%s)", r);
    else if (sp_streq(name, "to_a"))      buf_printf(b, "sp_MatchData_to_a(%s)", r);
    else if (sp_streq(name, "nil?"))      buf_printf(b, "(%s == 0)", r);
    else unsupported(c, id, "MatchData method");
    free(rs.p);
    return 1;
  }

  /* StringIO instance methods (a non-GC heap buffer behind sp_StringIO *). */
  if (recv >= 0 && rt == TY_STRINGIO) {
    Buf rs = expr_buf(c, recv);
    const char *r = rs.p ? rs.p : "";
    int done = 1;
    if (sp_streq(name, "string")) buf_printf(b, "sp_StringIO_string(%s)", r);
    else if (sp_streq(name, "pos") || sp_streq(name, "tell")) buf_printf(b, "sp_StringIO_pos(%s)", r);
    else if (sp_streq(name, "size") || sp_streq(name, "length")) buf_printf(b, "sp_StringIO_size(%s)", r);
    else if (sp_streq(name, "lineno")) buf_printf(b, "(%s)->lineno", r);
    else if (sp_streq(name, "puts") && argc == 0) buf_printf(b, "sp_StringIO_puts_empty(%s)", r);
    else if (sp_streq(name, "puts") && argc == 1) { buf_printf(b, "sp_StringIO_puts(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (sp_streq(name, "print") && argc == 1) { buf_printf(b, "sp_StringIO_print(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if ((sp_streq(name, "write") || sp_streq(name, "<<")) && argc == 1) { buf_printf(b, "sp_StringIO_write(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (sp_streq(name, "putc") && argc == 1) {
      if (comp_ntype(c, argv[0]) == TY_STRING) { buf_printf(b, "sp_StringIO_putc(%s, (mrb_int)(unsigned char)(", r); emit_expr(c, argv[0], b); buf_puts(b, ")[0])"); }
      else { buf_printf(b, "sp_StringIO_putc(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    }
    else if (sp_streq(name, "fsync") || sp_streq(name, "fileno") || sp_streq(name, "pid")) buf_printf(b, "((void)(%s), 0)", r);
    else if (sp_streq(name, "read") && argc == 0) buf_printf(b, "sp_StringIO_read(%s)", r);
    else if (sp_streq(name, "read") && argc == 1) { buf_printf(b, "sp_StringIO_read_n(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (sp_streq(name, "gets")) buf_printf(b, "sp_box_nullable_str(sp_StringIO_gets(%s))", r);
    else if (sp_streq(name, "getc")) buf_printf(b, "sp_box_nullable_str(sp_StringIO_getc(%s))", r);
    else if (sp_streq(name, "getbyte")) buf_printf(b, "sp_StringIO_getbyte(%s)", r);
    else if (sp_streq(name, "rewind")) buf_printf(b, "sp_StringIO_rewind(%s)", r);
    else if (sp_streq(name, "seek") && argc >= 1) { buf_printf(b, "sp_StringIO_seek(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (sp_streq(name, "truncate") && argc == 1) { buf_printf(b, "sp_StringIO_truncate(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (sp_streq(name, "eof?") || sp_streq(name, "eof")) buf_printf(b, "sp_StringIO_eof_p(%s)", r);
    else if (sp_streq(name, "close")) buf_printf(b, "sp_StringIO_close(%s)", r);
    else if (sp_streq(name, "closed?")) buf_printf(b, "sp_StringIO_closed_p(%s)", r);
    else if (sp_streq(name, "flush")) buf_printf(b, "sp_StringIO_flush(%s)", r);
    else if (sp_streq(name, "sync")) buf_printf(b, "sp_StringIO_sync(%s)", r);
    else if (sp_streq(name, "isatty") || sp_streq(name, "tty?")) buf_printf(b, "sp_StringIO_isatty(%s)", r);
    else done = 0;
    free(rs.p);
    if (done) return 1;
  }
  return 0;
}

/* Emit the expression that materialises (range).step(k) as a typed array,
   returning its array TyKind. A float step -- or a literal range with float
   bounds -- yields a FloatArray; sp_Range stores mrb_int bounds, so a literal
   float-bounded range reads begin/end from the AST to keep the float values.
   Integer steps use the faithful int helper (step 0 raises ArgumentError, a
   negative step descends, an exclusive range drops the endpoint). Shared by the
   no-block materialisation and the block walk so both yield identical values. */
TyKind emit_range_step_array(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (argc < 1) { buf_puts(b, "sp_IntArray_new()"); return TY_INT_ARRAY; }
  int rn = unwrap_parens(c, recv);
  int is_lit = rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "RangeNode");
  int lo = is_lit ? nt_ref(nt, rn, "left") : -1;
  int hi = is_lit ? nt_ref(nt, rn, "right") : -1;
  int excl = (is_lit && (nt_int(nt, rn, "flags", 0) & 4)) ? 1 : 0;
  int is_float = comp_ntype(c, argv[0]) == TY_FLOAT ||
                 (lo >= 0 && comp_ntype(c, lo) == TY_FLOAT) ||
                 (hi >= 0 && comp_ntype(c, hi) == TY_FLOAT);
  if (is_float && is_lit && lo >= 0 && hi >= 0) {
    buf_puts(b, "sp_FloatArray_from_step(");
    emit_float_expr(c, lo, b); buf_puts(b, ", ");
    emit_float_expr(c, hi, b); buf_puts(b, ", ");
    emit_float_expr(c, argv[0], b); buf_printf(b, ", %d)", excl);
    return TY_FLOAT_ARRAY;
  }
  int t = ++g_tmp;
  Buf rb = expr_buf(c, recv);
  if (is_float)
    buf_printf(b, "({ sp_Range _t%d = %s; sp_FloatArray_from_step((mrb_float)_t%d.first, (mrb_float)_t%d.last, ",
               t, rb.p ? rb.p : "", t, t);
  else
    buf_printf(b, "({ sp_Range _t%d = %s; sp_IntArray_from_range_step(_t%d.first, _t%d.last, ",
               t, rb.p ? rb.p : "", t, t);
  if (is_float) emit_float_expr(c, argv[0], b); else emit_int_expr(c, argv[0], b);
  buf_printf(b, ", _t%d.excl); })", t);
  free(rb.p);
  return is_float ? TY_FLOAT_ARRAY : TY_INT_ARRAY;
}

int emit_range_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  /* range value methods (evaluate the range once into a temp) */
  if (recv >= 0 && rt == TY_RANGE) {
    int block = nt_ref(nt, id, "block");
    if (sp_streq(name, "step") && argc == 1 && block < 0) {
      emit_range_step_array(c, id, b);
      return 1;
    }
    if (sp_streq(name, "each") && block < 0) {  /* external enumerator, or to_a materialize */
      int t = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      if (comp_ntype(c, id) == TY_ENUMERATOR) {
        buf_printf(b, "sp_Enumerator_new_from(sp_box_int_array(({ sp_Range _t%d = %s; sp_IntArray_from_range(_t%d.first, _t%d.last - _t%d.excl); })))",
                   t, rb.p ? rb.p : "", t, t, t);
      }
      else {
        buf_printf(b, "({ sp_Range _t%d = %s; sp_IntArray_from_range(_t%d.first, _t%d.last - _t%d.excl); })",
                   t, rb.p ? rb.p : "", t, t, t);
      }
      free(rb.p);
      return 1;
    }
    static const char *const rmeths[] = {
      "to_a", "include?", "member?", "cover?", "===", "sum", "min", "max",
      "first", "last", "size", "count", "begin", "end",
      "exclude_end?", "eql?", "minmax", "overlap?", NULL };
    int known = 0;
    for (int i = 0; rmeths[i]; i++) if (sp_streq(name, rmeths[i])) known = 1;
    /* `count` with a block or argument is Enumerable#count, not Range#size:
       let it fall through to the int-array redispatch below. */
    if (sp_streq(name, "count") && (block >= 0 || argc >= 1)) known = 0;
    if (known) {
      /* size/count on a string-literal range: no integer size -> nil, skip creating sp_Range */
      if ((sp_streq(name, "size") || sp_streq(name, "count")) && argc == 0) {
        int rn = unwrap_parens(c, recv);
        if (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "RangeNode")) {
          int lo = nt_ref(nt, rn, "left");
          if (lo >= 0 && comp_ntype(c, lo) == TY_STRING) {
            buf_puts(b, "SP_INT_NIL"); return 1;
          }
        }
      }
      int t = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Range _t%d = ", t);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      if (sp_streq(name, "to_a"))
        buf_printf(b, "sp_IntArray_from_range(_t%d.first, _t%d.last - _t%d.excl)", t, t, t);
      else if (sp_streq(name, "include?") || sp_streq(name, "member?") ||
               sp_streq(name, "cover?") || sp_streq(name, "===")) {
        /* cover?(range) checks that both endpoints of the arg fit inside self */
        if (sp_streq(name, "cover?") && argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE) {
          int t2 = ++g_tmp;
          buf_printf(b, "({ sp_Range _t%d = ", t2); emit_expr(c, argv[0], b);
          buf_printf(b, "; _t%d.first >= _t%d.first && (_t%d.last - _t%d.excl) <= (_t%d.last - _t%d.excl); })",
                     t2, t, t2, t2, t, t);
        }
        else {
          /* sp_range_include takes mrb_int; a float arg (`(1..).include?(2.4)`)
             needs an explicit cast, else clang -Werror flags the implicit
             float-literal->int conversion (gcc truncates silently). A poly arg
             (e.g. under --int-overflow=promote) is coerced with sp_poly_to_i. */
          TyKind at0 = comp_ntype(c, argv[0]);
          int arg_is_float = at0 == TY_FLOAT;
          int arg_is_poly = at0 == TY_POLY;
          buf_printf(b, "sp_range_include(&_t%d, ", t);
          if (arg_is_float) buf_puts(b, "(mrb_int)(");
          if (arg_is_poly) buf_puts(b, "sp_poly_to_i(");
          emit_expr(c, argv[0], b);
          if (arg_is_poly) buf_puts(b, ")");
          if (arg_is_float) buf_puts(b, ")");
          buf_puts(b, ")");
        }
      }
      else if (sp_streq(name, "first") || sp_streq(name, "min") || sp_streq(name, "begin")) {
        if (argc == 1) {
          /* first(n): collect up to n elements starting at first */
          int tf = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp;
          buf_printf(b, "({ sp_IntArray *_t%d = sp_IntArray_new(); mrb_int _t%d = ", tf, tn);
          emit_expr(c, argv[0], b);
          buf_printf(b, "; for (mrb_int _t%d = _t%d.first; _t%d <= _t%d.last - _t%d.excl && _t%d - _t%d.first < _t%d; _t%d++)"
                        " sp_IntArray_push(_t%d, _t%d); _t%d; })",
                     ti, t, ti, t, t, ti, t, tn, ti, tf, ti, tf);
        }
        else buf_printf(b, "(_t%d.first)", t);
      }
      else if (sp_streq(name, "max"))  /* max element: end minus the exclusive bound */
        buf_printf(b, "(_t%d.last - _t%d.excl)", t, t);
      else if (sp_streq(name, "last") || sp_streq(name, "end")) {
        if (argc == 1 && sp_streq(name, "last")) {
          /* last(n): collect up to n elements ending at last */
          int tf = ++g_tmp, tn = ++g_tmp, ts = ++g_tmp, te = ++g_tmp;
          buf_printf(b, "({ mrb_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
          buf_printf(b, "; mrb_int _t%d = _t%d.last - _t%d.excl;", te, t, t);
          buf_printf(b, " mrb_int _t%d = _t%d - _t%d + 1; if (_t%d < _t%d.first) _t%d = _t%d.first;", ts, te, tn, ts, t, ts, t);
          buf_printf(b, " sp_IntArray *_t%d = sp_IntArray_new(); for (mrb_int _i%d = _t%d; _i%d <= _t%d; _i%d++)"
                        " sp_IntArray_push(_t%d, _i%d); _t%d; })",
                     tf, tf, ts, tf, te, tf, tf, tf, tf);
        }
        else buf_printf(b, "(_t%d.last)", t);
      }
      else if (sp_streq(name, "size") || sp_streq(name, "count"))
        buf_printf(b, "(_t%d.last - _t%d.excl - _t%d.first + 1)", t, t, t);
      else if (sp_streq(name, "sum"))
        buf_printf(b, "sp_IntArray_sum(sp_IntArray_from_range(_t%d.first, _t%d.last - _t%d.excl), 0)", t, t, t);
      else if (sp_streq(name, "exclude_end?"))
        buf_printf(b, "(_t%d.excl != 0)", t);
      else if (sp_streq(name, "eql?")) {
        buf_printf(b, "sp_range_eq(_t%d, ", t); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "overlap?")) {
        int t2 = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", t2); emit_expr(c, argv[0], b);
        buf_printf(b, "; (_t%d.first <= _t%d.last - _t%d.excl && _t%d.first <= _t%d.last - _t%d.excl); })",
                   t, t2, t2, t2, t, t, t);
      }
      else if (sp_streq(name, "minmax")) {
        int ma = ++g_tmp;
        buf_printf(b, "({ sp_IntArray *_t%d = sp_IntArray_new(); sp_IntArray_push(_t%d, _t%d.first);"
                      " sp_IntArray_push(_t%d, _t%d.last - _t%d.excl); _t%d; })", ma, ma, t, ma, t, t, ma);
      }
      return 1;
    }
  }
  /* Enumerable method on a Range that arrays support but Range does not handle
     natively (reduce(:sym), group_by, partition, flat_map, count(&block), ...):
     materialize the range into an int array once, then re-dispatch the call as
     an array by overriding the receiver's emission and type. Inference already
     typed the call as the array version (range_enum_redispatch). */
  if (recv >= 0 && rt == TY_RANGE && range_enum_redispatch(c, id) &&
      g_n_argov < MAX_ARG_OVERRIDE) {
    int ta = ++g_tmp, tr = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_IntArray *_t%d = ({ sp_Range _t%d = %s; "
                      "sp_IntArray_from_range(_t%d.first, _t%d.last - _t%d.excl); }); SP_GC_ROOT(_t%d);\n",
               ta, tr, rb.p ? rb.p : "", tr, tr, tr, ta);
    free(rb.p);
    g_argov_node[g_n_argov] = recv;
    snprintf(g_argov_text[g_n_argov], sizeof g_argov_text[0], "_t%d", ta);
    g_n_argov++;
    TyKind sv = c->ntype[recv]; c->ntype[recv] = TY_INT_ARRAY;
    emit_call(c, id, b);
    c->ntype[recv] = sv;
    g_n_argov--;
    return 1;
  }
  return 0;
}

/* If `recv` is an index expression `outer[oidx]` (a `[]` CallNode with a single
   int argument), set *outer/*oidx and return 1. Lets a `[]=`/splice on such a
   receiver write a promoted array back into outer's slot instead of dropping the
   write-back (which would lose a typed->poly promotion for a computed receiver). */
static int splice_recv_index_slot(Compiler *c, int recv, int *outer, int *oidx) {
  const NodeTable *nt = c->nt;
  const char *rty = nt_type(nt, recv);
  if (!rty || !sp_streq(rty, "CallNode")) return 0;
  const char *rn = nt_str(nt, recv, "name");
  if (!rn || !sp_streq(rn, "[]")) return 0;
  int ro = nt_ref(nt, recv, "receiver");
  if (ro < 0) return 0;
  int rargc; const int *rargv = call_args(nt, recv, &rargc);
  if (rargc != 1 || comp_ntype(c, rargv[0]) != TY_INT) return 0;
  *outer = ro; *oidx = rargv[0];
  return 1;
}

int emit_poly_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  /* Hash#compare_by_identity switches a hash to identity (equal?/object_id)
     key comparison. Spinel's hash machinery compares keys by value, so the
     mutator can't take effect; emitting it as a no-op would silently diverge
     (subsequent lookups behave as a value-keyed hash). Reject loudly instead.
     The `compare_by_identity?` predicate is left to report false, which is
     correct for any hash this mutator never (successfully) ran on. */
  if (sp_streq(name, "compare_by_identity"))  /* any arity: identity hashing is unsupported */
    unsupported(c, id, "Hash#compare_by_identity (identity-keyed hashing)");
  /* encoding.name -> the encoding name string */
  if (sp_streq(name, "name") && argc == 0 && recv >= 0 && comp_ntype(c, recv) == TY_POLY) {
    const char *rty2 = nt_type(nt, recv);
    int is_enc = (rty2 && sp_streq(rty2, "SourceEncodingNode")) ||
                 (rty2 && sp_streq(rty2, "CallNode") &&
                  nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "encoding"));
    if (is_enc) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
  }

  /* instance_variable_get(:@x) on a POLY receiver with a literal symbol or
     string name: dispatch the field read over every instantiated class that
     has the slot, boxing per the slot's declared type (the poly twin of the
     concrete lowering; see the matching inference rule in analyze_infer.c).
     A receiver whose runtime class lacks the slot reads as nil, matching
     CRuby's unset-ivar behavior; the SP_TAG_OBJ guard keeps a boxed scalar
     (cls_id 0) from aliasing the user class at index 0 (cf. issue #1576). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "instance_variable_get") &&
      argc == 1 && nt_ref(nt, id, "block") < 0 && nt_type(nt, argv[0]) &&
      (sp_streq(nt_type(nt, argv[0]), "SymbolNode") || sp_streq(nt_type(nt, argv[0]), "StringNode"))) {
    const char *a0ty = nt_type(nt, argv[0]);
    const char *sym = sp_streq(a0ty, "SymbolNode")
                        ? nt_str(nt, argv[0], "value") : nt_str(nt, argv[0], "content");
    if (sym && sym[0] == '@') {
      TyKind res = comp_ntype(c, id);
      int tv = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", tv);
      emit_expr(c, recv, b);
      buf_printf(b, "; sp_RbVal _ivg%d = sp_box_nil(); if (_t%d.tag == SP_TAG_OBJ) switch (_t%d.cls_id) {",
                 tv, tv, tv);
      for (int k = 0; k < c->nclasses; k++) {
        if (!c->classes[k].instantiated) continue;
        int iv = comp_ivar_index(&c->classes[k], sym);
        if (iv < 0) continue;
        TyKind t = c->classes[k].ivar_types[iv];
        char fld[320];
        snprintf(fld, sizeof fld, "((sp_%s *)_t%d.v.p)->iv_%s", c->classes[k].name, tv, sym + 1);
        buf_printf(b, " case %d: _ivg%d = ", k, tv);
        emit_boxed_text(c, t, fld, b);
        buf_puts(b, "; break;");
      }
      buf_puts(b, " } ");
      if (res != TY_POLY && res != TY_UNKNOWN) {
        char ivn[24]; snprintf(ivn, sizeof ivn, "_ivg%d", tv);
        emit_unbox_text(c, res, ivn, b);
        buf_puts(b, "; })");
      }
      else buf_printf(b, "_ivg%d; })", tv);
      return 1;
    }
  }

  /* poly receiver: nil? / conversions / a few type-agnostic queries */
  if (recv >= 0 && rt == TY_POLY && argc == 0) {
    if (sp_streq(name, "nil?")) { buf_puts(b, "sp_poly_nil_p("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
    /* Hash#keys / #values on a poly value (e.g. an evidence-free empty `{}` that
       stayed poly). Skip when a user class defines keys/values so its method wins. */
    if (sp_streq(name, "keys") || sp_streq(name, "values")) {
      int has_user = 0;
      for (int kk = 0; kk < c->nclasses && !has_user; kk++)
        if (comp_method_in_chain(c, kk, name, NULL) >= 0) has_user = 1;
      if (!has_user) {
        buf_printf(b, "sp_poly_%s(", name); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
      }
    }
    if (sp_streq(name, "length") || sp_streq(name, "size") || sp_streq(name, "empty?")) {
      int has_user_len = 0;
      const char *lcheck = (sp_streq(name, "empty?")) ? "length" : name;
      for (int kk = 0; kk < c->nclasses && !has_user_len; kk++)
        if (comp_method_in_chain(c, kk, lcheck, NULL) >= 0) has_user_len = 1;
      if (sp_streq(name, "empty?") && !has_user_len)
        for (int kk = 0; kk < c->nclasses && !has_user_len; kk++)
          if (comp_method_in_chain(c, kk, "empty?", NULL) >= 0) has_user_len = 1;
      if (!has_user_len) {
        if (sp_streq(name, "empty?")) {
          buf_puts(b, "(sp_poly_length("); emit_expr(c, recv, b); buf_puts(b, ") == 0)");
        }
        else {
          buf_puts(b, "sp_poly_length("); emit_expr(c, recv, b); buf_puts(b, ")");
        }
        return 1;
      }
    }
    if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) {
      int has_user_method = 0;
      for (int k = 0; k < c->nclasses; k++)
        if (comp_method_in_chain(c, k, name, NULL) >= 0) { has_user_method = 1; break; }
      if (!has_user_method) {
        buf_printf(b, "%s(", sp_streq(name, "to_s") ? "sp_poly_to_s" : "sp_poly_inspect");
        emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
      }
    }
    if (sp_streq(name, "to_i")) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
    if (sp_streq(name, "to_f")) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
    /* Numeric queries / rounding: dispatch on the runtime tag (a non-numeric
       tag raises CRuby's NoMethodError). A user method or attr reader with
       the same name wins -- the poly method dispatch handles it instead. */
    {
      const char *pfn =
        sp_streq(name, "nan?")      ? "sp_poly_nan_p" :
        sp_streq(name, "finite?")   ? "sp_poly_finite_p" :
        sp_streq(name, "infinite?") ? "sp_poly_infinite" :
        sp_streq(name, "zero?")     ? "sp_poly_zero_p" :
        sp_streq(name, "positive?") ? "sp_poly_positive_p" :
        sp_streq(name, "negative?") ? "sp_poly_negative_p" :
        sp_streq(name, "abs")       ? "sp_poly_abs" :
        sp_streq(name, "floor")     ? "sp_poly_floor" :
        sp_streq(name, "ceil")      ? "sp_poly_ceil" :
        sp_streq(name, "round")     ? "sp_poly_round" :
        sp_streq(name, "truncate")  ? "sp_poly_truncate" :
        sp_streq(name, "bytesize")  ? "sp_poly_bytesize" :
        sp_streq(name, "ord")       ? "sp_poly_ord" :
        sp_streq(name, "bit_length") ? "sp_poly_bit_length" : NULL;
      if (pfn) {
        int has_user = 0;
        for (int kk = 0; kk < c->nclasses && !has_user; kk++)
          if (comp_method_in_chain(c, kk, name, NULL) >= 0 ||
              comp_reader_in_chain(c, kk, name, NULL)) has_user = 1;
        if (!has_user) {
          buf_printf(b, "%s(", pfn); emit_expr(c, recv, b); buf_puts(b, ")");
          return 1;
        }
      }
    }
    if (sp_streq(name, "upcase"))     { buf_puts(b, "sp_box_str(sp_str_upcase(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return 1; }
    if (sp_streq(name, "downcase"))   { buf_puts(b, "sp_box_str(sp_str_downcase(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return 1; }
    if (sp_streq(name, "capitalize")) { buf_puts(b, "sp_box_str(sp_str_capitalize(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return 1; }
    if (sp_streq(name, "swapcase"))   { buf_puts(b, "sp_box_str(sp_str_swapcase(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return 1; }
    if (sp_streq(name, "strip"))      { buf_puts(b, "sp_box_str(sp_str_strip(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return 1; }
    if (sp_streq(name, "reverse"))    { buf_puts(b, "sp_box_str(sp_str_reverse(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return 1; }
    if (sp_streq(name, "chomp"))      { buf_puts(b, "sp_box_str(sp_str_chomp(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return 1; }
    if (sp_streq(name, "chop"))       { buf_puts(b, "sp_box_str(sp_str_chop(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return 1; }
    if (sp_streq(name, "chr"))        { buf_puts(b, "sp_box_str(sp_str_chr(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return 1; }
    if (sp_streq(name, "freeze"))     { emit_expr(c, recv, b); return 1; }
  }
  /* poly receiver: String#getbyte (a non-string tag raises NoMethodError).
     A user method or attr reader with the same name wins. */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && sp_streq(name, "getbyte")) {
    int has_user = 0;
    for (int kk = 0; kk < c->nclasses && !has_user; kk++)
      if (comp_method_in_chain(c, kk, name, NULL) >= 0 ||
          comp_reader_in_chain(c, kk, name, NULL)) has_user = 1;
    if (!has_user) {
      buf_puts(b, "sp_poly_getbyte("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
  }
  /* poly receiver: arr[start, len] = src -- 3-arg splice assign
     Skip Fiber/Fiber.current storage receivers (handled later). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "[]=") && argc == 3 &&
      !sp_is_fiber_storage_recv(nt, recv)) {
    int tv = ++g_tmp;
    const char *rcvty = nt_type(nt, recv);
    int recv_is_lvalue = rcvty && (sp_streq(rcvty, "LocalVariableReadNode") ||
                                   sp_streq(rcvty, "InstanceVariableReadNode"));
    int outer, oidx;
    TyKind rty2 = comp_ntype(c, argv[2]);
    int tam = splice_to_ary_mi(c, rty2);
    buf_puts(b, "({ ");
    if (tam >= 0) {
      /* object RHS with to_ary: splice the coercion; the OBJECT is the value */
      Buf call; memset(&call, 0, sizeof call);
      TyKind cty = emit_splice_to_ary_src(c, argv[2], rty2, tam, tv, b, &call);
      buf_printf(b, "sp_RbVal _t%d = ", tv);
      emit_boxed_text(c, cty, call.p ? call.p : "", b);
      buf_puts(b, "; ");
      free(call.p);
    }
    else { buf_printf(b, "sp_RbVal _t%d = ", tv); emit_boxed(c, argv[2], b); buf_puts(b, "; "); }
    /* Store the possibly-promoted array back into the receiver so a typed->poly
       promotion survives: assign to a local/ivar lvalue, or write to outer's slot
       for a computed `outer[idx]` receiver; otherwise splice in place. */
    if (recv_is_lvalue) {
      emit_expr(c, recv, b); buf_puts(b, " = sp_poly_splice("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b);
    }
    else if (splice_recv_index_slot(c, recv, &outer, &oidx)) {
      buf_puts(b, "sp_poly_slot_splice("); emit_boxed(c, outer, b); buf_puts(b, ", "); emit_int_expr(c, oidx, b);
      buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b);
    }
    else {
      buf_puts(b, "sp_poly_splice("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b);
    }
    if (tam >= 0)
      buf_printf(b, ", _t%d); sp_box_obj(_tq%d, %d); })", tv, tv, ty_object_class(rty2));
    else
      buf_printf(b, ", _t%d); _t%d; })", tv, tv);
    return 1;
  }
  /* poly receiver: []= with symbol, string, int, or poly key -> runtime dispatch
     Skip Fiber/Fiber.current storage receivers (handled later). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "[]=") && argc == 2 &&
      !sp_is_fiber_storage_recv(nt, recv)) {
    /* arr[range] = rhs on a poly receiver: a splice over the range's span. */
    if (comp_ntype(c, argv[0]) == TY_RANGE) {
      int tv = ++g_tmp;
      const char *rcvty = nt_type(nt, recv);
      int recv_is_lvalue = rcvty && (sp_streq(rcvty, "LocalVariableReadNode") ||
                                     sp_streq(rcvty, "InstanceVariableReadNode"));
      int outer, oidx;
      TyKind rty1 = comp_ntype(c, argv[1]);
      int tam = splice_to_ary_mi(c, rty1);
      buf_puts(b, "({ ");
      if (tam >= 0) {
        /* object RHS with to_ary: splice the coercion; the OBJECT is the value */
        Buf call; memset(&call, 0, sizeof call);
        TyKind cty = emit_splice_to_ary_src(c, argv[1], rty1, tam, tv, b, &call);
        buf_printf(b, "sp_RbVal _t%d = ", tv);
        emit_boxed_text(c, cty, call.p ? call.p : "", b);
        buf_puts(b, "; ");
        free(call.p);
      }
      else { buf_printf(b, "sp_RbVal _t%d = ", tv); emit_boxed(c, argv[1], b); buf_puts(b, "; "); }
      if (recv_is_lvalue) {
        emit_expr(c, recv, b); buf_puts(b, " = sp_poly_splice_range("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b);
      }
      else if (splice_recv_index_slot(c, recv, &outer, &oidx)) {
        buf_puts(b, "sp_poly_slot_splice_range("); emit_boxed(c, outer, b); buf_puts(b, ", "); emit_int_expr(c, oidx, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b);
      }
      else {
        buf_puts(b, "sp_poly_splice_range("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b);
      }
      if (tam >= 0)
        buf_printf(b, ", _t%d); sp_box_obj(_tq%d, %d); })", tv, tv, ty_object_class(rty1));
      else
        buf_printf(b, ", _t%d); _t%d; })", tv, tv);
      return 1;
    }
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
      /* widen_and_set returns a *different* boxed value when a typed array is
         promoted to a PolyArray (element-kind mismatch); otherwise it mutates in
         place. Store the result back so promotion survives: assign to a
         local/ivar lvalue, or write to outer's slot for a computed `outer[idx]`
         receiver; a receiver we cannot address falls back to in-place mutation. */
      const char *rcvty = nt_type(nt, recv);
      int recv_is_lvalue = rcvty && (sp_streq(rcvty, "LocalVariableReadNode") ||
                                     sp_streq(rcvty, "InstanceVariableReadNode"));
      int outer, oidx;
      if (recv_is_lvalue) {
        emit_expr(c, recv, b);
        buf_puts(b, " = sp_poly_arr_widen_and_set("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_int_expr(c, argv[0], b);
      }
      else if (splice_recv_index_slot(c, recv, &outer, &oidx)) {
        buf_puts(b, "sp_poly_slot_set("); emit_boxed(c, outer, b); buf_puts(b, ", "); emit_int_expr(c, oidx, b);
        buf_puts(b, ", "); emit_int_expr(c, argv[0], b);
      }
      else {
        buf_puts(b, "sp_poly_arr_widen_and_set("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_int_expr(c, argv[0], b);
      }
    }
    else {
      buf_printf(b, "sp_poly_set_poly("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_boxed(c, argv[0], b);
    }
    buf_printf(b, ", _t%d); _t%d; })", tv, tv);
    (void)vt;
    return 1;
  }
  /* poly receiver: [] with symbol or string key -> runtime dispatch */
  /* poly receiver: arr[start, len] -> sp_poly_slice (string or typed array) */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "[]") && argc == 2) {
    /* The runtime dispatches on the receiver's tag: a string/array does a
       two-arg slice, a bound Method (optcarrot's poke handlers) is called with
       both int args. Both operands are raw integers. */
    buf_puts(b, "sp_poly_slice("); emit_expr(c, recv, b); buf_puts(b, ", ");
    emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
    return 1;
  }
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "[]") && argc == 1) {
    /* `@table[i][j]` dispatch table narrowed to int (poly_double_index_int):
       call the entry (bound method / int array) for an unboxed int result. */
    if (comp_ntype(c, id) == TY_INT) {
      buf_puts(b, "sp_poly_index_int("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    TyKind at = comp_ntype(c, argv[0]);
    /* Only use the fast single-call path when no user class defines [].
       If any user class has its own [] method, fall through to the per-class
       poly dispatch (line ~4640) which generates both user and builtin arms. */
    int has_user_aref = 0;
    for (int k = 0; k < c->nclasses; k++)
      if (comp_method_in_chain(c, k, "[]", NULL) >= 0) { has_user_aref = 1; break; }
    if (!has_user_aref) {
      if (at == TY_SYMBOL) {
        buf_puts(b, "sp_poly_get_sym("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (at == TY_STRING) {
        buf_puts(b, "sp_poly_get_str("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (at == TY_INT) {
        buf_puts(b, "sp_poly_arr_get_hash("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (at == TY_POLY) {
        buf_puts(b, "sp_poly_index_poly("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      /* a non-poly key (e.g. a Method): box it, then index polymorphically */
      if (at != TY_UNKNOWN) {
        buf_puts(b, "sp_poly_index_poly("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
    }
  }
  /* poly receiver: join */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "join")) {
    buf_puts(b, "sp_poly_join("); emit_expr(c, recv, b);
    buf_puts(b, ", "); if (argc >= 1) emit_expr(c, argv[0], b); else buf_puts(b, "\"\"");
    buf_puts(b, ")"); return 1;
  }
  /* poly receiver: clamp(lo, hi) tag-dispatches int/float at runtime; the range
     form clamp(a..b) routes through the same helper with boxed bounds. */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "clamp") && argc == 2) {
    buf_puts(b, "sp_poly_clamp("); emit_boxed(c, recv, b);
    buf_puts(b, ", "); emit_boxed(c, argv[0], b);
    buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ")");
    return 1;
  }
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "clamp") && argc == 1 &&
      nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode")) {
    /* raises on an exclusive range with a real end; routes a user-object
       receiver through the `<=>` hook (sp_obj_clamp_range) */
    buf_puts(b, "sp_poly_clamp_range("); emit_boxed(c, recv, b);
    buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }
  /* poly receiver: replace(other) -> runtime dispatch (nullable array). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "replace") && argc == 1) {
    buf_puts(b, "sp_poly_replace("); emit_expr(c, recv, b);
    buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }
  /* poly receiver: pack(fmt) -> runtime dispatch (nullable array). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "pack") && argc == 1) {
    buf_puts(b, "sp_poly_pack("); emit_expr(c, recv, b);
    buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }
  /* poly receiver: delete(chars) -> String#delete on the unboxed payload.
     Mirrors the analyzer's poly rule (result is a concrete TY_STRING): a
     string that widened to poly -- `data[offset, 8].delete("\x00")` stripping
     NUL padding off a fixed-width WAD name field in doom's texture parser.
     Like the analyzer rule, skipped when a user class defines `delete` (the
     per-class poly dispatch below then generates the proper arms). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "delete") && argc == 1 &&
      comp_ntype(c, id) == TY_STRING) {
    int has_user_delete = 0;
    for (int k = 0; k < c->nclasses; k++)
      if (comp_method_in_chain(c, k, "delete", NULL) >= 0) { has_user_delete = 1; break; }
    if (!has_user_delete) {
      buf_puts(b, "sp_str_delete(sp_poly_to_s("); emit_expr(c, recv, b);
      buf_puts(b, "), "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
  }

  /* poly receiver: gsub/sub with a regex literal -- extract the string
     payload (poly values reaching here are strings) and route to the
     engine, just like a TY_STRING receiver. */
  if (recv >= 0 && rt == TY_POLY && (sp_streq(name, "gsub") || sp_streq(name, "sub")) &&
      argc == 2 && re_lit_index(c, argv[0]) >= 0) {
    const char *suf = comp_ntype(c, argv[1]) == TY_STR_STR_HASH ? "_str_str_hash" : "";
    buf_printf(b, "sp_re_%s%s(sp_re_pat_%d, sp_poly_to_s(", name, suf, re_lit_index(c, argv[0]));
    emit_expr(c, recv, b); buf_puts(b, "), ");
    emit_expr(c, argv[1], b); buf_puts(b, ")");
    return 1;
  }
  return 0;
}
