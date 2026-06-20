#include "analyze_internal.h"
#include <stdio.h>
#include <stdlib.h>

/* True when `recv` reads an ivar/local that has at least one direct write of a
   numeric scalar (an Integer/Float literal, or a numeric-inferred value). Used
   to disambiguate `x << y`: a numeric-assigned slot means Integer#<< (a bit
   shift), not Array#push, so the slot must not be promoted to an array. The
   Integer/Float *literal* test is syntactic and therefore stable across
   fixpoint iterations, which keeps an early push pass from corrupting the slot
   to an array before the numeric assignment has been folded in (after which the
   array would unify with the int writes to poly and break every later shift). */
static int recv_has_scalar_numeric_write(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  const char *rty = recv >= 0 ? nt_type(nt, recv) : NULL;
  if (!rty) return 0;
  int is_ivar = !strcmp(rty, "InstanceVariableReadNode");
  int is_local = !strcmp(rty, "LocalVariableReadNode");
  if (!is_ivar && !is_local) return 0;
  const char *rnm = nt_str(nt, recv, "name");
  if (!rnm) return 0;
  Scope *rscope = is_local ? comp_scope_of(c, recv) : NULL;
  const char *wkind = is_ivar ? "InstanceVariableWriteNode" : "LocalVariableWriteNode";
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, wkind)) continue;
    const char *wnm = nt_str(nt, id, "name");
    if (!wnm || strcmp(wnm, rnm)) continue;
    if (is_local && comp_scope_of(c, id) != rscope) continue;
    int v = nt_ref(nt, id, "value");
    if (v < 0) continue;
    const char *vty = nt_type(nt, v);
    if (vty && (!strcmp(vty, "IntegerNode") || !strcmp(vty, "FloatNode"))) return 1;
    TyKind vt = infer_type(c, v);
    if (vt == TY_INT || vt == TY_FLOAT || vt == TY_BIGINT) return 1;
  }
  return 0;
}

/* Unified value type of `recv[k] = v` writes that target the same ivar/local
   as `recv`. Lets a hash promoted via a string-key READ inherit the value type
   its `[]=` writes establish (e.g. `@h[s] = int` -> str_int_hash) instead of
   defaulting to a str_poly slot that can never narrow. TY_UNKNOWN if there is
   no such write (caller falls back to poly). */
static TyKind aset_value_type(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  const char *rty = nt_type(nt, recv);
  if (!rty) return TY_UNKNOWN;
  int is_ivar = !strcmp(rty, "InstanceVariableReadNode");
  int is_local = !strcmp(rty, "LocalVariableReadNode");
  if (!is_ivar && !is_local) return TY_UNKNOWN;
  const char *rnm = nt_str(nt, recv, "name");
  if (!rnm) return TY_UNKNOWN;
  Scope *rsc = comp_scope_of(c, recv);
  int rcls = rsc ? rsc->class_id : -1;
  TyKind acc = TY_UNKNOWN;
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || strcmp(nm, "[]=")) continue;
    int wrecv = nt_ref(nt, id, "receiver");
    if (wrecv < 0) continue;
    const char *wn = nt_str(nt, wrecv, "name");
    if (!wn || strcmp(wn, rnm)) continue;
    if (is_ivar) {
      if (nt_kind(nt, wrecv) != NK_InstanceVariableReadNode) continue;
      Scope *ws = comp_scope_of(c, wrecv);
      if ((ws ? ws->class_id : -1) != rcls) continue;
    }
    else {
      if (nt_kind(nt, wrecv) != NK_LocalVariableReadNode) continue;
      if (comp_scope_of(c, wrecv) != rsc) continue;
    }
    int args = nt_ref(nt, id, "arguments");
    int an = 0;
    const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an < 2) continue;
    acc = ty_unify(acc, infer_type(c, av[1]));
  }
  return acc;
}

/* Seed a hash parameter's value type from its own `param[k] = v` writes. The
   usage-driven hash promotion skips parameters (they are typed from call
   sites), so a param filled internally by `p[s] = int` and read back via
   `p.fetch(s)` defaults to str_poly through a monotonic cycle. Narrow it to
   the concrete hash its writes establish.

   ONLY string/symbol-keyed writes are considered: an int-keyed `p[i] = v` is
   ambiguous with array element assignment (e.g. an int_array RAM param filled
   by `ram[i] = b`), so int keys must not be read as hash evidence. */
int infer_param_hash_value(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    for (int p = 0; p < sc->nparams; p++) {
      if (p == sc->rest_idx || p == sc->kwrest_idx) continue;
      LocalVar *lv = scope_local(sc, sc->pnames[p]);
      if (!lv || lv->is_block_param) continue;
      TyKind cur = lv->type;
      int seedable = cur == TY_UNKNOWN || cur == TY_POLY ||
                     (ty_is_hash(cur) && ty_hash_val(cur) == TY_POLY);
      if (!seedable || lv->rbs_seeded) continue;
      TyKind kt = TY_UNKNOWN, vt = TY_UNKNOWN;
      int saw = 0, ambiguous = 0;
      for (int id = 0; id < nt->count; id++) {
        if (nt_kind(nt, id) != NK_CallNode) continue;
        const char *nm = nt_str(nt, id, "name");
        if (!nm || strcmp(nm, "[]=")) continue;
        int wr = nt_ref(nt, id, "receiver");
        if (wr < 0 || nt_kind(nt, wr) != NK_LocalVariableReadNode) continue;
        const char *wn = nt_str(nt, wr, "name");
        if (!wn || strcmp(wn, sc->pnames[p]) || comp_scope_of(c, wr) != sc) continue;
        int args = nt_ref(nt, id, "arguments");
        int an = 0;
        const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
        if (an < 2) continue;
        TyKind k = infer_type(c, av[0]);
        if (k != TY_STRING && k != TY_SYMBOL) { ambiguous = 1; break; }
        kt = ty_unify(kt, k);
        vt = ty_unify(vt, infer_type(c, av[1]));
        saw = 1;
      }
      if (!saw || ambiguous) continue;
      TyKind hv = ty_hash_of(kt, vt);
      if (hv == TY_UNKNOWN) continue;
      if (hv != cur && ty_hash_val(hv) != TY_POLY) { lv->type = hv; changed = 1; }
    }
  }
  return changed;
}

/* 1 if local `name` in scope `sc` has at least one write and every write
   assigns an empty `{}` hash literal -- i.e. it is a hash container whose
   contents come from elsewhere (passed by reference into a callee). Such a
   local can safely adopt a hash type from a parameter it is passed to. */
static int local_all_writes_empty_hash(Compiler *c, Scope *sc, const char *name) {
  const NodeTable *nt = c->nt;
  int saw = 0;
  for (int id = 0; id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_LocalVariableWriteNode) continue;
    const char *wn = nt_str(nt, id, "name");
    if (!wn || strcmp(wn, name) || comp_scope_of(c, id) != sc) continue;
    int v = nt_ref(nt, id, "value");
    if (v < 0 || nt_kind(nt, v) != NK_HashNode) return 0;
    int hn = 0; nt_arr(nt, v, "elements", &hn);
    if (hn != 0) return 0;
    saw = 1;
  }
  return saw;
}

/* Per-pass index of local-variable write nodes keyed by (scope, name). The
   usage-driven promotion scans in infer_write_types ask "does local X in scope
   S have any write / an array-typed write"; without this index each such query
   re-scanned the entire node table, making a program with M such sites
   O(M * nodes) -- the dominant cost on large auto-generated model graphs. The
   index groups the write nodes once so each query walks only its own bucket. */
typedef struct {
  int *node;   /* local-write node ids */
  int *next;   /* chain: next record in the same bucket, or -1 */
  int *head;   /* hash buckets: head record index into node[], or -1 */
  int cap;     /* bucket count (power of two) */
} LWIndex;

static int lw_is_write_kind(NodeKind k) {
  return k == NK_LocalVariableWriteNode || k == NK_LocalVariableOrWriteNode ||
         k == NK_LocalVariableAndWriteNode || k == NK_LocalVariableOperatorWriteNode;
}

static unsigned lw_hash(const char *name, int scope) {
  unsigned h = 2166136261u ^ (unsigned)scope;
  for (const char *p = name; p && *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
  return h;
}

static void lw_index_build(Compiler *c, LWIndex *ix) {
  const NodeTable *nt = c->nt;
  int n = 0;
  for (int id = 0; id < nt->count; id++)
    if (lw_is_write_kind(nt_kind(nt, id))) n++;
  int cap = 16;
  while (cap < n * 2) cap <<= 1;
  ix->cap = cap;
  ix->node = (int *)malloc(sizeof(int) * (n > 0 ? n : 1));
  ix->next = (int *)malloc(sizeof(int) * (n > 0 ? n : 1));
  ix->head = (int *)malloc(sizeof(int) * cap);
  for (int i = 0; i < cap; i++) ix->head[i] = -1;
  int k = 0;
  for (int id = 0; id < nt->count; id++) {
    if (!lw_is_write_kind(nt_kind(nt, id))) continue;
    const char *nm = nt_str(nt, id, "name");
    int sc = (int)(comp_scope_of(c, id) - c->scopes);
    unsigned h = lw_hash(nm, sc) & (unsigned)(cap - 1);
    ix->node[k] = id;
    ix->next[k] = ix->head[h];
    ix->head[h] = k;
    k++;
  }
}

static void lw_index_free(LWIndex *ix) {
  free(ix->node); free(ix->next); free(ix->head);
}

/* First record index in the bucket for (scope, name); iterate via ix->next.
   Callers must still confirm scope/name (hash collisions) and node kind. */
static int lw_index_first(const LWIndex *ix, const char *name, int scope) {
  return ix->head[lw_hash(name, scope) & (unsigned)(ix->cap - 1)];
}

/* Per-pass index of instance-variable write nodes keyed by ivar name -- the
   ivar analogue of LWIndex. The usage-driven promotion scans below ask "does
   @x have a non-empty-hash / typed write"; without this each query re-scanned
   the whole node table, the dominant cost on ivar-heavy model graphs (the
   #1302 from_hash/to_hash shape). Indexes both plain and `||=` ivar writes;
   callers filter by node kind and class. Reuses LWIndex's layout (scope-less,
   so the bucket key is name only). */
static int ivw_is_write_kind(NodeKind k) {
  return k == NK_InstanceVariableWriteNode || k == NK_InstanceVariableOrWriteNode;
}

static unsigned ivw_hash(const char *name) {
  unsigned h = 2166136261u;
  for (const char *p = name; p && *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
  return h;
}

static void ivw_index_build(Compiler *c, LWIndex *ix) {
  const NodeTable *nt = c->nt;
  int n = 0;
  for (int id = 0; id < nt->count; id++)
    if (ivw_is_write_kind(nt_kind(nt, id))) n++;
  int cap = 16;
  while (cap < n * 2) cap <<= 1;
  ix->cap = cap;
  ix->node = (int *)malloc(sizeof(int) * (n > 0 ? n : 1));
  ix->next = (int *)malloc(sizeof(int) * (n > 0 ? n : 1));
  ix->head = (int *)malloc(sizeof(int) * cap);
  for (int i = 0; i < cap; i++) ix->head[i] = -1;
  int k = 0;
  for (int id = 0; id < nt->count; id++) {
    if (!ivw_is_write_kind(nt_kind(nt, id))) continue;
    const char *nm = nt_str(nt, id, "name");
    unsigned h = ivw_hash(nm) & (unsigned)(cap - 1);
    ix->node[k] = id;
    ix->next[k] = ix->head[h];
    ix->head[h] = k;
    k++;
  }
}

/* First record index in the bucket for ivar `name`; iterate via ix->next.
   Callers must still confirm name (hash collisions), node kind, and class. */
static int ivw_index_first(const LWIndex *ix, const char *name) {
  return ix->head[ivw_hash(name) & (unsigned)(ix->cap - 1)];
}

int infer_write_types(Compiler *c) {
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
  /* Re-seed loop-growth bigint locals inside the recompute frame (the
     reset above would otherwise wipe the promotion each iteration). */
  infer_bigint_loop_locals(c);

  /* Index local-write nodes by (scope, name) for the usage-driven promotion
     scans further down (see the per-scope write-site lookups below). */
  LWIndex lw_ix;
  lw_index_build(c, &lw_ix);
  /* Index ivar-write nodes by name for the empty-hash / typed-write promotion
     guards on the InstanceVariableReadNode branches below. */
  LWIndex ivw_ix;
  ivw_index_build(c, &ivw_ix);

  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    const char *nm = NULL;
    TyKind newt = TY_UNKNOWN;
    if (!strcmp(ty, "LocalVariableWriteNode")) {
      nm = nt_str(nt, id, "name");
      int val_id = nt_ref(nt, id, "value");
      newt = infer_type(c, val_id);
      /* a `x = nil` write doesn't pin the type: flow it as TY_NIL so ty_unify
         can narrow it against an object write (NULL encodes nil); a purely-nil
         local is mapped to poly by a post-fixpoint backstop. */
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
    /* A void value assigned in value position (`v = always_raising_method`)
       is nil-ish: type the slot poly so it is declarable. The RHS call is
       emitted via emit_boxed, which evaluates it (it diverges) and yields nil. */
    if (newt == TY_VOID) newt = TY_POLY;
    if (!nm) continue;
    LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
    if (!lv || lv->is_block_param) continue;
    /* Params are typed from call sites (monotonic widen); a body assignment
       of a different type widens them too (e.g. `x = "s"` in an int param's
       body -> poly). Only widen -- never let an unknown RHS reset them. */
    if (lv->is_param) {
      if (newt != TY_UNKNOWN && !lv->rbs_seeded) {
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
    /* `r, w = IO.pipe` -> both targets are IO handles. */
    if (ln == 2 && vty && !strcmp(vty, "CallNode") && nt_str(nt, value, "name") &&
        !strcmp(nt_str(nt, value, "name"), "pipe")) {
      int vrecv = nt_ref(nt, value, "receiver");
      if (vrecv >= 0 && nt_type(nt, vrecv) && !strcmp(nt_type(nt, vrecv), "ConstantReadNode") &&
          nt_str(nt, vrecv, "name") && !strcmp(nt_str(nt, vrecv, "name"), "IO")) {
        for (int i = 0; i < 2; i++) {
          if (strcmp(nt_type(nt, lefts[i]) ? nt_type(nt, lefts[i]) : "", "LocalVariableTargetNode")) continue;
          const char *lnm = nt_str(nt, lefts[i], "name");
          LocalVar *lv = lnm ? scope_local_intern(comp_scope_of(c, id), lnm) : NULL;
          if (lv && lv->type != TY_IO) { lv->type = TY_IO; changed = 1; }
        }
        continue;
      }
    }
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
              if (iv_ms < 0 || class_ivar_pinned(&c->classes[ms_arr->class_id], ivnm)) continue;
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
              if (iv_ms2 < 0 || class_ivar_pinned(&c->classes[ms_arr->class_id], ivnm2)) continue;
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
        if (iv_idx < 0 || class_ivar_pinned(&c->classes[iv_cid], ivnm)) continue;
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
        if (iv_idx3 < 0 || class_ivar_pinned(&c->classes[iv_cid3], ivnm3)) continue;
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
      else if (!strcmp(pty, "FindPatternNode")) {
        /* in [*head, a, b, *tail] -- the two splats bind to arrays of the
           scrutinee's element type; required LV targets bind to an element. */
        TyKind arr_t = ty_is_array(scrutinee_t) ? scrutinee_t : TY_POLY_ARRAY;
        TyKind elem_t = ty_is_array(scrutinee_t) ? ty_array_elem(scrutinee_t) : TY_POLY;
        int sides[2] = { nt_ref(nt, pat, "left"), nt_ref(nt, pat, "right") };
        for (int sidx = 0; sidx < 2; sidx++) {
          int sp = sides[sidx];
          if (sp < 0 || !nt_type(nt, sp) || strcmp(nt_type(nt, sp), "SplatNode")) continue;
          int inner = nt_ref(nt, sp, "expression");
          if (inner < 0 || !nt_type(nt, inner) ||
              strcmp(nt_type(nt, inner), "LocalVariableTargetNode")) continue;
          const char *snm = nt_str(nt, inner, "name");
          LocalVar *lv = snm ? scope_local(ms, snm) : NULL;
          if (!lv || lv->is_param || lv->is_block_param) continue;
          TyKind mg = ty_unify(lv->type, arr_t);
          if (mg != lv->type) { lv->type = mg; changed = 1; }
        }
        int rn = 0;
        const int *reqs = nt_arr(nt, pat, "requireds", &rn);
        for (int k = 0; k < rn; k++) {
          const char *lty2 = nt_type(nt, reqs[k]);
          if (!lty2 || strcmp(lty2, "LocalVariableTargetNode")) continue;
          const char *lnm = nt_str(nt, reqs[k], "name");
          LocalVar *lv = lnm ? scope_local(ms, lnm) : NULL;
          if (!lv || lv->is_param || lv->is_block_param) continue;
          TyKind et = (elem_t != TY_UNKNOWN) ? elem_t : TY_POLY;
          TyKind mg = ty_unify(lv->type, et);
          if (mg != lv->type) { lv->type = mg; changed = 1; }
        }
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
        /* `<<` is ambiguous (Array#push vs Integer#<< shift): a numeric-assigned
           receiver is a shift, so don't promote its slot to an array. */
        if (!strcmp(name, "<<") && recv_has_scalar_numeric_write(c, recv)) continue;
        is_push = 1; vt = infer_type(c, argv[0]);
      }
      else if (name && !strcmp(name, "[]=") && an == 2) {
        is_idx_write = 1; kt = infer_type(c, argv[0]); vt = infer_type(c, argv[1]);
      }
      else if (name && (!strcmp(name, "fetch") ||
                        (!strcmp(name, "[]") && an == 1)) && an >= 1) {
        /* hash.fetch(key,..) / hash[key]: promote TY_UNKNOWN local to a typed hash.
           Only fires when the slot is currently TY_UNKNOWN (empty hash).
           A 2-arg [] is a string/array slice, never a hash read — only the
           1-arg form is key-lookup evidence (fetch keeps >=1: (key, default)). */
        TyKind rslot = TY_UNKNOWN;
        const char *rrty = nt_type(nt, recv);
        const char *rnm2 = NULL;
        if (rrty && !strcmp(rrty, "LocalVariableReadNode")) {
          rnm2 = nt_str(nt, recv, "name");
          LocalVar *lv2 = rnm2 ? scope_local(comp_scope_of(c, recv), rnm2) : NULL;
          if (lv2) rslot = lv2->type;
        }
        else if (rrty && !strcmp(rrty, "InstanceVariableReadNode")) {
          /* an already-typed ivar hash must not be re-promoted: unifying e.g.
             a str_str_hash with the promotion's str_poly target would widen the
             slot to poly. Only an untyped (empty-{}) ivar promotes here. */
          rslot = infer_type(c, recv);
          /* Fixpoint-ordering hazard: a param-fed ivar (`@s = s`) reads
             UNKNOWN before the param's call-site type arrives, and a read
             like `@s[i]` would mis-promote it to a hash. Promote from reads
             only when every assignment to the ivar is an empty `{}` literal
             (the actual empty-hash case) — a syntactic test that is stable
             across fixpoint iterations. */
          if (rslot == TY_UNKNOWN) {
            const char *pin = nt_str(nt, recv, "name");
            int blocked = 0;
            for (int _r = ivw_index_first(&ivw_ix, pin); _r >= 0 && !blocked; _r = ivw_ix.next[_r]) {
              int wi = ivw_ix.node[_r];
              const char *wnm = nt_str(nt, wi, "name");
              if (!wnm || !pin || strcmp(wnm, pin)) continue;
              int wv = nt_ref(nt, wi, "value");
              const char *wvty = wv >= 0 ? nt_type(nt, wv) : NULL;
              int is_empty_hash = 0;
              if (wvty && !strcmp(wvty, "HashNode")) {
                int hn = 0; nt_arr(nt, wv, "elements", &hn);
                if (hn == 0) is_empty_hash = 1;
              }
              if (!is_empty_hash) blocked = 1;
            }
            if (blocked) continue;
          }
        }
        if (rslot != TY_UNKNOWN) continue;  /* already typed, skip */
        /* Only promote via [] read if the receiver local has at least one
           write site in its scope. Pure block params have no write site and
           get their type from infer_block_params; promoting them here to
           TY_STR_POLY_HASH before is_block_param is set creates a TY_POLY
           that ty_unify can never narrow back to the yield arg type. */
        if (rrty && !strcmp(rrty, "LocalVariableReadNode") && rnm2) {
          Scope *recv_scope = comp_scope_of(c, recv);
          int recv_sid = (int)(recv_scope - c->scopes);
          int has_write = 0;
          for (int _r = lw_index_first(&lw_ix, rnm2, recv_sid); _r >= 0 && !has_write; _r = lw_ix.next[_r]) {
            int _wi = lw_ix.node[_r];
            if (comp_scope_of(c, _wi) != recv_scope) continue;
            const char *_wnm = nt_str(nt, _wi, "name");
            if (_wnm && !strcmp(_wnm, rnm2)) has_write = 1;
          }
          if (!has_write) continue;
        }
        kt = infer_type(c, argv[0]);
        if (kt == TY_SYMBOL) { vt = TY_INT; /* dummy: sym hash val is always poly */ }
        else if (kt == TY_STRING) {
          /* Seed the value type from the hash's `[]=` writes so an int-valued
             string-keyed hash filled by `@h[s] = int` stays str_int_hash
             instead of widening to str_poly (which never narrows back). */
          TyKind wv = aset_value_type(c, recv);
          vt = (wv == TY_INT || wv == TY_STRING) ? wv : TY_POLY;
        }
        /* An int-key bare read (`x[i]`) is NOT strong hash evidence: arrays
           index by int too, and an array-returning method assigned to `x` may
           not have settled its element type yet, so promoting here would lock
           the slot to a hash before the array write is recognized. A genuine
           int-keyed hash is typed by its `[]=` writes or literal instead. */
        else continue;
      }
      else continue;
    }
    else if (!strcmp(ty, "IndexOperatorWriteNode") ||
             !strcmp(ty, "IndexOrWriteNode") ||
             !strcmp(ty, "IndexAndWriteNode")) {
      /* h[k] op= v / h[k] ||= v / h[k] &&= v: same promotion as h[k] = v. */
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
    const char *watch_nm = NULL;  /* ivar name for SP_IVWATCH, NULL for locals */
    if (rty && !strcmp(rty, "LocalVariableReadNode")) {
      const char *rnm = nt_str(nt, recv, "name");
      Scope *lsc = rnm ? comp_scope_of(c, recv) : NULL;
      LocalVar *lv = lsc ? scope_local(lsc, rnm) : NULL;
      if (!lv || lv->is_param || lv->is_block_param) continue;
      /* A bare `x[i]` read OR an `x[i] = v` element assignment must not promote
         `x` to a hash if `x` elsewhere gets an array-typed write (`x = a.split`
         etc.): it is an array indexed/assigned by position, and the hash type
         would otherwise collide with it. Mirrors the ivar guard. */
      if (!is_push && lv->type == TY_UNKNOWN) {
        int lsc_sid = (int)(lsc - c->scopes);
        int has_array_write = 0;
        for (int _r = lw_index_first(&lw_ix, rnm, lsc_sid); _r >= 0 && !has_array_write; _r = lw_ix.next[_r]) {
          int w = lw_ix.node[_r];
          if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
          const char *wn = nt_str(nt, w, "name");
          if (!wn || strcmp(wn, rnm) || comp_scope_of(c, w) != lsc) continue;
          int wv = nt_ref(nt, w, "value");
          if (wv >= 0 && ty_is_array(infer_type(c, wv))) has_array_write = 1;
        }
        if (has_array_write) continue;
      }
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
      watch_nm = inm;
      /* If the slot is TY_UNKNOWN but has a direct InstanceVariableWriteNode
         that assigns a typed value OR an empty array/hash literal (e.g.
         @buf = [nil]*7 or @free = []), skip usage-driven hash promotion
         (but allow push-driven array promotion through). Without this guard,
         @free[0] read promotes @free to poly_poly_hash before @free = []
         has been processed as an array. */
      /* A typed (non-nil) construction write — `@a = [x]*n`, `@a = arr.map{}`,
         or an `@a = []` literal — means this ivar is an array filled by index,
         not a hash. Skip usage-driven hash promotion for both plain reads and
         `@a[k]=v` index-writes. A genuine hash (`@h = {}`) infers UNKNOWN from
         its empty literal and is unaffected. */
      if (!is_push && *slot == TY_UNKNOWN && inm) {
        int has_typed_write = 0;
        for (int _r = ivw_index_first(&ivw_ix, inm); _r >= 0 && !has_typed_write; _r = ivw_ix.next[_r]) {
          int _wi = ivw_ix.node[_r];
          if (nt_kind(nt, _wi) != NK_InstanceVariableWriteNode) continue;
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
    sp_ivwatch(watch_nm, is_push ? "usage_push" : (is_idx_write ? "usage_idxwrite" : "usage_read"), before, *slot);
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
  lw_index_free(&lw_ix);
  lw_index_free(&ivw_ix);
  return changed;
}

/* Unify a call's argument types into method scope `mi`'s parameters. */
int bind_call_params(Compiler *c, int call_id, int mi) {
  if (mi < 0) return 0;
  const NodeTable *nt = c->nt;
  Scope *m = &c->scopes[mi];
  int args = nt_ref(nt, call_id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  int changed = 0;
  /* `callee(...)`: the arg list is a single ForwardingArgumentsNode. Bind the
     callee's params from the enclosing `def foo(...)` method's synthesized
     __fwd_* params, positionally, so the callee's return type resolves (#1288). */
  if (argc == 1 && argv && nt_type(nt, argv[0]) &&
      !strcmp(nt_type(nt, argv[0]), "ForwardingArgumentsNode")) {
    Scope *encl = comp_scope_of(c, argv[0]);
    if (!encl) return 0;
    int n = m->nparams < encl->nparams ? m->nparams : encl->nparams;
    for (int k = 0; k < n; k++) {
      LocalVar *p = scope_local(m, m->pnames[k]);
      LocalVar *ep = scope_local(encl, encl->pnames[k]);
      if (!p || p->rbs_seeded || !ep || ep->type == TY_UNKNOWN) continue;
      TyKind merged = ty_unify(p->type, ep->type);
      if (merged != p->type) { p->type = merged; changed = 1; }
    }
    return changed;
  }
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
    }
else {
      at = infer_type(c, argv[k]);
    }
    LocalVar *p = scope_local(m, m->pnames[k]);
    if (!p || p->rbs_seeded) continue;
    /* A void arg (`sink(always_raising_method)`) is nil-ish in value position:
       bind the param poly so it is declarable; the arg is emitted via
       emit_boxed (it diverges and yields nil). */
    if (at == TY_VOID) at = TY_POLY;
    /* A nil arg narrows against an object param (NULL encodes nil) but widens
       any non-object param to poly. Pass nil through to ty_unify only while
       the param is still unknown or already an object. */
    if (at == TY_NIL && p->type != TY_UNKNOWN && p->type != TY_NIL && !ty_is_object(p->type)) at = TY_POLY;
    TyKind merged = ty_unify(p->type, at);
    if (merged != p->type) { p->type = merged; changed = 1; }
    /* Reverse binding: an empty-`{}`-only local passed to a hash parameter is
       that hash container, filled inside the callee through the reference.
       Type the local as the param's hash so it is constructed (sp_<H>Hash_new)
       rather than passed as a NULL-deref'ing poly nil. */
    if (ty_is_hash(p->type) && apty && !strcmp(apty, "LocalVariableReadNode")) {
      const char *an = nt_str(nt, argv[k], "name");
      Scope *asc = an ? comp_scope_of(c, argv[k]) : NULL;
      LocalVar *al = asc ? scope_local(asc, an) : NULL;
      if (al && !al->is_param && !al->is_block_param &&
          (al->type == TY_UNKNOWN || al->type == TY_POLY) &&
          local_all_writes_empty_hash(c, asc, an)) {
        al->type = p->type; changed = 1;
      }
    }
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
      LocalVar *p = scope_local(m, m->pnames[pi]);
      if (!p || p->rbs_seeded) continue;
      TyKind at = infer_type(c, argv[ai]);
      if (at == TY_NIL && p->type != TY_UNKNOWN && p->type != TY_NIL && !ty_is_object(p->type)) at = TY_POLY;
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
        if (!p || p->rbs_seeded) continue;
        TyKind merged = ty_unify(p->type, at);
        if (merged != p->type) { p->type = merged; changed = 1; }
      }
    }
else {
      int any_kw_bound = 0;
      for (int e = 0; e < en; e++) {
        int key = nt_ref(nt, elems[e], "key");
        int val = nt_ref(nt, elems[e], "value");
        if (key < 0 || val < 0) continue;
        const char *kty = nt_type(nt, key);
        const char *kname = (kty && !strcmp(kty, "SymbolNode")) ? nt_str(nt, key, "value") : NULL;
        if (!kname) continue;
        LocalVar *p = scope_local(m, kname);
        if (!p || p->rbs_seeded) continue;
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
        if (p && !p->rbs_seeded) {
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
int propagate_prep_params(Compiler *c) {
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
        if (!fp || !tp || fp->type == TY_UNKNOWN || tp->rbs_seeded) continue;
        TyKind merged = ty_unify(tp->type, fp->type);
        if (merged != tp->type) { tp->type = merged; changed = 1; }
      }
    }
  }
  return changed;
}

/* Optional parameters get a type from their default value too. */
int infer_default_param_types(Compiler *c) {
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
      if (!p || p->rbs_seeded) continue;
      TyKind merged = ty_unify(p->type, dt);
      if (merged != p->type) { p->type = merged; changed = 1; }
    }
  }
  return changed;
}

/* Methods that only Strings respond to -- definitive evidence that a
   receiver is a String. (length/size/etc are shared with containers and so
   are deliberately excluded to keep the inference conservative.) */
int is_string_only_method(const char *m) {
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
int infer_params_from_ivar_hash_ops(Compiler *c) {
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
int infer_hash_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  static const char *const hash_only_meths[] = {
    "keys","values","each_pair","merge","has_key?","key?","fetch","store",
    "delete","transform_values","transform_keys","to_h","each_with_object",NULL
  };
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    /* Index-or-write on an unknown param hash (h[k] ||= v / &&= / op=): infer the
       variant from the key + value types, mirroring the []= case below. These are
       not CallNodes, so the CallNode path never reaches them; without this a hash
       passed in empty (`{}` infers TY_UNKNOWN) stays unresolved and the method's
       return type degrades to poly, which the caller then rejects. */
    if (!strcmp(ty, "IndexOrWriteNode") || !strcmp(ty, "IndexAndWriteNode") ||
        !strcmp(ty, "IndexOperatorWriteNode")) {
      int wrecv = nt_ref(nt, id, "receiver");
      if (wrecv < 0) continue;
      const char *wrty = nt_type(nt, wrecv);
      if (!wrty || strcmp(wrty, "LocalVariableReadNode")) continue;
      const char *wrnm = nt_str(nt, wrecv, "name");
      if (!wrnm) continue;
      Scope *ws = comp_scope_of(c, id);
      LocalVar *wlv = scope_local(ws, wrnm);
      if (!wlv || !wlv->is_param || wlv->type != TY_UNKNOWN) continue;
      int wargs = nt_ref(nt, id, "arguments");
      int wan = 0; const int *wargv = wargs >= 0 ? nt_arr(nt, wargs, "arguments", &wan) : NULL;
      if (wan < 1) continue;
      TyKind wkt = infer_type(c, wargv[0]);
      TyKind wvt = infer_type(c, nt_ref(nt, id, "value"));
      TyKind wwant = TY_UNKNOWN;
      if (wkt == TY_STRING) wwant = (wvt == TY_STRING) ? TY_STR_STR_HASH : TY_STR_POLY_HASH;
      else if (wkt == TY_SYMBOL) wwant = TY_SYM_POLY_HASH;
      else if (wkt == TY_INT) wwant = TY_POLY_POLY_HASH;
      if (wwant == TY_UNKNOWN) continue;
      wlv->type = wwant; changed = 1;
      continue;
    }
    if (strcmp(ty, "CallNode")) continue;
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
int infer_array_params(Compiler *c) {
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
    if (lv && lv->is_param && !lv->rbs_seeded && (lv->type == TY_UNKNOWN || ty_is_hash(lv->type))) { lv->type = TY_POLY_ARRAY; changed = 1; }
  }
  return changed;
}

/* Infer a still-unknown parameter as String when the body calls a
   String-only method on it (a param with no concrete-typed caller). */
int infer_string_params(Compiler *c) {
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

int infer_param_types(Compiler *c) {
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
          if (!src || !dst || dst->rbs_seeded) continue;
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
      if (!pp || pp->rbs_seeded) continue;
      TyKind at2 = infer_type(c, val);
      TyKind mg2 = ty_unify(pp->type, at2);
      if (mg2 != pp->type) { pp->type = mg2; changed = 1; }
      continue;
    }
    if (strcmp(ty, "CallNode")) continue;
    const char *name = nt_str(nt, id, "name");
    int recv = nt_ref(nt, id, "receiver");

    /* `raise Cls, arg` constructs `Cls.new(arg)` for a user exception
       subclass, so seed Cls#initialize's first param from arg's type --
       without this the param stays TY_UNKNOWN and the constructor gets
       marked unreachable, dropping the initialize call (#1415). */
    if (recv < 0 && name && !strcmp(name, "raise")) {
      int rargs = nt_ref(nt, id, "arguments");
      int ran = 0; const int *rav = rargs >= 0 ? nt_arr(nt, rargs, "arguments", &ran) : NULL;
      if (ran >= 2 && nt_type(nt, rav[0]) &&
          (!strcmp(nt_type(nt, rav[0]), "ConstantReadNode") || !strcmp(nt_type(nt, rav[0]), "ConstantPathNode"))) {
        const char *rcn = nt_str(nt, rav[0], "name");
        int rci = rcn ? comp_class_index(c, rcn) : -1;
        if (rci >= 0 && class_is_exc_subclass(c, rci)) {
          int imi = comp_method_in_chain(c, rci, "initialize", NULL);
          if (imi >= 0 && c->scopes[imi].nparams >= 1) {
            LocalVar *ip = scope_local(&c->scopes[imi], c->scopes[imi].pnames[0]);
            TyKind at = infer_type(c, rav[1]);
            if (ip && !ip->rbs_seeded && at != TY_UNKNOWN) {
              TyKind m = ty_unify(ip->type, at);
              if (m != ip->type) { ip->type = m; changed = 1; }
            }
          }
        }
      }
    }

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
    /* `Module.accessor.cmethod(args)` folded to a constant: bind args to the
       resolved class method's params (so it is not dropped as untyped). */
    {
      int fold_ci = comp_sg_reader_const(c, recv);
      if (fold_ci >= 0) {
        int fmi = comp_cmethod_in_chain(c, fold_ci, name, NULL);
        if (fmi >= 0) { changed |= bind_call_params(c, id, fmi); continue; }
      }
      int cand[32];
      int ncand = comp_sg_reader_candidates(c, recv, cand, 32);
      if (ncand >= 2) {
        int bound = 0;
        for (int k = 0; k < ncand; k++) {
          int cmi = comp_cmethod_in_chain(c, cand[k], name, NULL);
          if (cmi >= 0) { changed |= bind_call_params(c, id, cmi); bound = 1; }
        }
        if (bound) continue;
      }
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
              if (class_ivar_pinned(cls, cls->ivars[a])) continue;
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
int infer_for_index(Compiler *c) {
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
int first_yield(Compiler *c, int si) {
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (ty && !strcmp(ty, "YieldNode") && c->nscope[id] == si) return id;
  }
  return -1;
}

/* Arguments node of the first `<&block-param>.call(...)` in scope `si`, or
   -1. Lets block-param inference treat block.call like a yield. */
int first_block_call_args(Compiler *c, int si) {
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

/* Arguments node of the first receiverless `instance_exec(args, &<blk>)` in
   scope `si` that forwards the scope's own block param, or -1. A receiverless
   instance_exec invokes the block with `args` (self is unchanged by the
   rebind), so it types the block exactly like a yield of `args`. */
int first_ie_exec_args(Compiler *c, int si) {
  Scope *m = &c->scopes[si];
  if (!m->blk_param || !m->blk_param[0]) return -1;
  for (int id = 0; id < c->nt->count; id++) {
    if (c->nscope[id] != si) continue;
    const char *ty = nt_type(c->nt, id);
    if (!ty || strcmp(ty, "CallNode") || nt_ref(c->nt, id, "receiver") >= 0) continue;
    const char *nm = nt_str(c->nt, id, "name");
    if (!nm || strcmp(nm, "instance_exec")) continue;
    int blk = nt_ref(c->nt, id, "block");
    if (blk < 0 || !nt_type(c->nt, blk) || strcmp(nt_type(c->nt, blk), "BlockArgumentNode")) continue;
    int expr = nt_ref(c->nt, blk, "expression");
    if (expr < 0 || !nt_type(c->nt, expr) || strcmp(nt_type(c->nt, expr), "LocalVariableReadNode")) continue;
    const char *en = nt_str(c->nt, expr, "name");
    if (en && !strcmp(en, m->blk_param)) return nt_ref(c->nt, id, "arguments");
  }
  return -1;
}

int a_proc_params_node(Compiler *c, int create); /* forward decl */

/* Follow a chain of pure `...` forwarders (a method whose whole body is a
   single `target(...)` call) starting at `mi` until reaching the method that
   actually yields (or owns the &block). Returns that method's index, or -1.
   Lets a block passed to a forwarder be typed from the real yielder's args. */
static int forwarding_yield_target(Compiler *c, int mi, int depth) {
  if (mi < 0 || depth > 16) return -1;
  Scope *m = &c->scopes[mi];
  if (m->yields || (m->blk_param && m->blk_param[0])) return mi;
  int body = m->body;
  if (body < 0 || nt_kind(c->nt, body) != NK_StatementsNode) return -1;
  int n = 0; const int *st = nt_arr(c->nt, body, "body", &n);
  if (n != 1) return -1;
  int call = st[0];
  if (nt_kind(c->nt, call) != NK_CallNode || nt_ref(c->nt, call, "receiver") >= 0) return -1;
  int args = nt_ref(c->nt, call, "arguments");
  int ac = 0; const int *av = args >= 0 ? nt_arr(c->nt, args, "arguments", &ac) : NULL;
  if (ac != 1 || !av || !nt_type(c->nt, av[0]) ||
      strcmp(nt_type(c->nt, av[0]), "ForwardingArgumentsNode")) return -1;
  const char *tn = nt_str(c->nt, call, "name");
  if (!tn) return -1;
  int t = comp_method_index(c, tn);
  if (t < 0 && m->class_id >= 0) t = comp_method_in_chain(c, m->class_id, tn, NULL);
  return forwarding_yield_target(c, t, depth + 1);
}

/* Bind block parameter types for supported iteration methods. */
/* Desugar a forwarded callable *value* -- `recv.<iter>(&f)` where `f` is a Proc
   value or a Method object rather than the active inlined &block -- into the
   equivalent literal block `recv.<iter> { |__fwd_k...| f.call(__fwd_k...) }`.
   The existing literal-block emitters then lower it for ANY iterator, instead of
   a per-callable, per-iterator special case. This mirrors Ruby's own `&obj` =>
   `obj.to_proc` model: once a callable is wrapped as a block, it is just a
   block. The synthetic block's param arity and types come from ty_block_yield
   (the builtin block-protocol oracle), so hash `each` (2 params),
   each_with_index, ranges etc. desugar correctly -- not only 1-arg array maps.
   (A `&:sym` block already lowers via its own to_proc path and is left alone.)

   The callable expression is re-evaluated once per element, so this is
   restricted to side-effect-free forms: a local or ivar read, or a `method(:m)`
   call (a deterministic method-object lookup). The active inlined &block (a
   forward whose expression names the enclosing method's block param) is left to
   the inline-forward path. Runs in the inference fixpoint; once a call is
   rewritten its block is a BlockNode, so it is never revisited. */
/* Required-param count of a forwarded callable expression `ex`, or -1 if it
   cannot be determined statically. Chooses the hash-pair calling convention: a
   1-param callable receives the [k,v] pair as one array, a 2-param one is called
   positionally (matching CRuby's proc auto-splat of the yielded pair). */
static int fwd_callable_arity(Compiler *c, int ex) {
  NodeTable *nt = (NodeTable *)c->nt;
  const char *exty = nt_type(nt, ex);
  if (!exty) return -1;
  int create = -1;
  if (!strcmp(exty, "LambdaNode") || is_proc_create(c, ex)) create = ex;
  else if (!strcmp(exty, "LocalVariableReadNode")) {
    const char *vn = nt_str(nt, ex, "name");
    Scope *sc = vn ? comp_scope_of(c, ex) : NULL;
    for (int w = 0; vn && w < nt->count; w++) {
      const char *wty = nt_type(nt, w);
      if (!wty || strcmp(wty, "LocalVariableWriteNode")) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || strcmp(wn, vn) || comp_scope_of(c, w) != sc) continue;
      int val = nt_ref(nt, w, "value");
      if (val >= 0 && is_proc_create(c, val)) { create = val; break; }
    }
  }
  if (create < 0) return -1;
  int pn = a_proc_params_node(c, create);
  if (pn < 0) return -1;
  int rn = 0; nt_arr(nt, pn, "requireds", &rn);
  return rn;
}

/* `send(:m, args)` / `__send__("m", args)` / `public_send(:m, args)` with NO
   explicit receiver -> a direct implicit-self call to `m` with the remaining
   args. The literal symbol/string name resolves statically, the same model as
   the textual `recv.send(:m)` receiver rewrite in spinel_parse.c; a non-literal
   name (`send(meth)`) has no static target and is left alone. Done on the AST,
   not textually, so a `send(:` inside a string or comment can't be mis-matched
   (the bare token has no `.` anchor). #1261. */
int desugar_implicit_send(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || strcmp(nt_type(nt, id), "CallNode")) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;        /* implicit self only */
    const char *nm = nt_str(nt, id, "name");
    if (!nm || (strcmp(nm, "send") && strcmp(nm, "__send__") &&
                strcmp(nm, "public_send"))) continue;
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int argc = 0; const int *argv = nt_arr(nt, args, "arguments", &argc);
    if (argc < 1 || !argv) continue;
    const char *a0ty = nt_type(nt, argv[0]);
    const char *mname = NULL;
    if (a0ty && !strcmp(a0ty, "SymbolNode")) mname = nt_str(nt, argv[0], "value");
    else if (a0ty && !strcmp(a0ty, "StringNode")) mname = nt_str(nt, argv[0], "content");
    if (!mname || !*mname) continue;                      /* non-literal name: leave it */
    if (!strcmp(mname, "send") || !strcmp(mname, "__send__") ||
        !strcmp(mname, "public_send")) continue;          /* don't re-trigger next pass */
    int nrest = argc - 1;
    if (nrest > 64) continue;                             /* absurd arity: leave it */
    int rest[64];
    for (int k = 0; k < nrest; k++) rest[k] = argv[k + 1];  /* copy before realloc */
    char namebuf[256];
    snprintf(namebuf, sizeof namebuf, "%s", mname);        /* copy before realloc */
    int base = nt->count;
    int newargs = nt_new_node(nt, "ArgumentsNode");
    if (newargs < 0) continue;
    nt_node_set_arr(nt, newargs, "arguments", rest, nrest);
    nt_node_set_str(nt, id, "name", namebuf);              /* retarget the call */
    nt_node_set_ref(nt, id, "arguments", newargs);         /* drop the name arg */
    comp_grow_node_arrays(c);
    int encl = c->nscope[id];
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    changed = 1;
  }
  return changed;
}

/* Resolve a forwarded callable reference (`&inline_lambda` / `&proc_var` /
   `&method(:m)`) to the body statements and parameters of its definition.
   Returns 1 with *out_body / *out_pn set, else 0. Mirrors fwd_callable_arity's
   resolution but exposes the body so a caller can inspect how a param is used. */
static int fwd_callable_def(Compiler *c, int ref, int *out_body, int *out_pn) {
  NodeTable *nt = (NodeTable *)c->nt;
  const char *ty = nt_type(nt, ref);
  if (!ty) return 0;
  if (!strcmp(ty, "CallNode") && nt_str(nt, ref, "name") &&
      !strcmp(nt_str(nt, ref, "name"), "method")) {
    int mi = method_obj_target_mi(c, ref);
    if (mi < 0) return 0;
    int dn = c->scopes[mi].def_node;
    *out_body = c->scopes[mi].body;
    *out_pn = dn >= 0 ? nt_ref(nt, dn, "parameters") : -1;
    return *out_body >= 0;
  }
  int create = -1;
  if (!strcmp(ty, "LambdaNode") || is_proc_create(c, ref)) create = ref;
  else if (!strcmp(ty, "LocalVariableReadNode")) {
    const char *vn = nt_str(nt, ref, "name");
    Scope *sc = vn ? comp_scope_of(c, ref) : NULL;
    for (int w = 0; vn && w < nt->count; w++) {
      const char *wty = nt_type(nt, w);
      if (!wty || strcmp(wty, "LocalVariableWriteNode")) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || strcmp(wn, vn) || comp_scope_of(c, w) != sc) continue;
      int val = nt_ref(nt, w, "value");
      if (val >= 0 && is_proc_create(c, val)) { create = val; break; }
    }
  }
  if (create < 0) return 0;
  *out_body = a_proc_body(c, create);
  *out_pn = a_proc_params_node(c, create);
  return *out_body >= 0;
}

/* Unify into *acc the element type pushed onto a local named `memo` (`memo << e`
   / `memo.push(e)`) anywhere in the subtree rooted at `id`. */
static void ewo_scan_pushes(Compiler *c, int id, const char *memo, TyKind *acc) {
  NodeTable *nt = (NodeTable *)c->nt;
  if (id < 0) return;
  const char *ty = nt_type(nt, id);
  if (ty && !strcmp(ty, "CallNode")) {
    const char *nm = nt_str(nt, id, "name");
    int rcv = nt_ref(nt, id, "receiver");
    const char *rty = rcv >= 0 ? nt_type(nt, rcv) : NULL;
    if (nm && rty && !strcmp(rty, "LocalVariableReadNode") &&
        nt_str(nt, rcv, "name") && !strcmp(nt_str(nt, rcv, "name"), memo) &&
        (!strcmp(nm, "<<") || !strcmp(nm, "push"))) {
      int args = nt_ref(nt, id, "arguments");
      int an = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      for (int k = 0; k < an; k++) *acc = ty_unify(*acc, infer_type(c, argv[k]));
    }
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(nt, id, i); if (ch >= 0) ewo_scan_pushes(c, ch, memo, acc); }
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) if (ids[k] >= 0) ewo_scan_pushes(c, ids[k], memo, acc); }
}

/* The element type an `each_with_object([])` array accumulator is filled with,
   inferred from how its memo param (block param 1) is used. Scans the block body
   for pushes onto memo; when the body merely forwards to a callable
   (`callable.call(elem, memo)` -- the value-forwarding desugar), follows into the
   callable's definition and scans its 2nd param the same way. Returns the unified
   pushed element type, or TY_UNKNOWN when no push is found (callers keep the
   empty-`[]` int_array default). */
TyKind ewo_memo_elem_type(Compiler *c, int callid) {
  NodeTable *nt = (NodeTable *)c->nt;
  int block = nt_ref(nt, callid, "block");
  const char *bty = block >= 0 ? nt_type(nt, block) : NULL;
  if (!bty || strcmp(bty, "BlockNode")) return TY_UNKNOWN;  /* not yet a literal block */
  const char *memo = block_param_name(c, block, 1);
  int body = nt_ref(nt, block, "body");
  if (!memo || body < 0) return TY_UNKNOWN;

  /* Direct: the block body itself fills memo. */
  TyKind acc = TY_UNKNOWN;
  ewo_scan_pushes(c, body, memo, &acc);
  if (acc != TY_UNKNOWN) return acc;

  /* Forwarded: a single `callable.call(elem, memo)` -- follow into the callable. */
  int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
  if (bn != 1 || !bb) return TY_UNKNOWN;
  int call = bb[0];
  if (!nt_type(nt, call) || strcmp(nt_type(nt, call), "CallNode")) return TY_UNKNOWN;
  if (!nt_str(nt, call, "name") || strcmp(nt_str(nt, call, "name"), "call")) return TY_UNKNOWN;
  int rcv = nt_ref(nt, call, "receiver");
  int cargs = nt_ref(nt, call, "arguments");
  int cn = 0; const int *cargv = cargs >= 0 ? nt_arr(nt, cargs, "arguments", &cn) : NULL;
  if (rcv < 0 || cn < 1 || !cargv) return TY_UNKNOWN;
  int last = cargv[cn - 1];
  if (!nt_type(nt, last) || strcmp(nt_type(nt, last), "LocalVariableReadNode")) return TY_UNKNOWN;
  if (!nt_str(nt, last, "name") || strcmp(nt_str(nt, last, "name"), memo)) return TY_UNKNOWN;

  int cb_body = -1, cb_pn = -1;
  if (!fwd_callable_def(c, rcv, &cb_body, &cb_pn) || cb_pn < 0) return TY_UNKNOWN;
  int rn = 0; const int *reqs = nt_arr(nt, cb_pn, "requireds", &rn);
  if (rn < 2 || !reqs) return TY_UNKNOWN;  /* the callable's memo is its 2nd param */
  const char *cb_memo = nt_str(nt, reqs[1], "name");
  if (!cb_memo) return TY_UNKNOWN;
  TyKind acc2 = TY_UNKNOWN;
  ewo_scan_pushes(c, cb_body, cb_memo, &acc2);
  return acc2;
}

/* The arity and body-return type of the proc a curry was built from. */
static int curry_proc_base(Compiler *c, int recv, int *arity, TyKind *ret) {
  NodeTable *nt = (NodeTable *)c->nt;
  int body = -1, pn = -1;
  if (!fwd_callable_def(c, recv, &body, &pn)) return 0;
  int rn = 0; if (pn >= 0) nt_arr(nt, pn, "requireds", &rn);
  *arity = rn;
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  *ret = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_NIL;
  return 1;
}

/* Walk a curry chain to its base proc, counting args applied through `node`
   (`proc.curry` -> 0, each `[arg]` / `.call(arg)` adds 1, a var resolves to its
   assigned curry expression). Sets *applied, *arity, *ret on success. */
static int curry_chain(Compiler *c, int node, int *applied, int *arity, TyKind *ret, int depth) {
  NodeTable *nt = (NodeTable *)c->nt;
  if (depth > 64) return 0;  /* guard against cyclic var assignments (a=b; b=a) */
  const char *ty = nt_type(nt, node);
  if (!ty) return 0;
  if (!strcmp(ty, "CallNode")) {
    const char *nm = nt_str(nt, node, "name");
    int recv = nt_ref(nt, node, "receiver");
    if (!nm || recv < 0) return 0;
    if (!strcmp(nm, "curry")) {
      if (!curry_proc_base(c, recv, arity, ret)) return 0;
      *applied = 0;
      return 1;
    }
    if (!strcmp(nm, "[]") || !strcmp(nm, "call") || !strcmp(nm, "()")) {
      if (!curry_chain(c, recv, applied, arity, ret, depth + 1)) return 0;
      (*applied)++;
      return 1;
    }
    return 0;
  }
  if (!strcmp(ty, "LocalVariableReadNode")) {
    const char *vn = nt_str(nt, node, "name");
    Scope *sc = vn ? comp_scope_of(c, node) : NULL;
    for (int w = 0; vn && w < nt->count; w++) {
      if (!nt_type(nt, w) || strcmp(nt_type(nt, w), "LocalVariableWriteNode")) continue;
      const char *wn = nt_str(nt, w, "name");
      if (!wn || strcmp(wn, vn) || comp_scope_of(c, w) != sc) continue;
      int val = nt_ref(nt, w, "value");
      if (val >= 0) return curry_chain(c, val, applied, arity, ret, depth + 1);
    }
    return 0;
  }
  return 0;
}

/* Does applying one more arg at curry-application `node` reach the base proc's
   arity (completing it)? Sets *out_ret to the proc's return type. Returns 1 when
   `node` is a recognized curry chain. */
int curry_apply_info(Compiler *c, int node, int *out_complete, TyKind *out_ret) {
  int applied = 0, arity = 0; TyKind ret = TY_UNKNOWN;
  if (!curry_chain(c, node, &applied, &arity, &ret, 0)) return 0;
  *out_complete = (arity > 0 && applied >= arity);
  *out_ret = ret;
  return 1;
}

int desugar_value_callable_forwards(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;  /* snapshot: synthetic nodes are appended past here */
  for (int id = 0; id < n0; id++) {
    if (!nt_type(nt, id) || strcmp(nt_type(nt, id), "CallNode")) continue;
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || !nt_type(nt, blk) || strcmp(nt_type(nt, blk), "BlockArgumentNode")) continue;
    int ex = nt_ref(nt, blk, "expression");
    if (ex < 0) continue;  /* anonymous `&`: inline-forward path, not a value */
    const char *exty = nt_type(nt, ex);
    if (!exty) continue;
    int simple_ref = !strcmp(exty, "LocalVariableReadNode") ||
                     !strcmp(exty, "InstanceVariableReadNode");
    /* `&method(:m)`: a deterministic method-object lookup, safe to re-evaluate */
    int method_obj = !strcmp(exty, "CallNode") && nt_str(nt, ex, "name") &&
                     !strcmp(nt_str(nt, ex, "name"), "method");
    /* `&->(x){...}`: an inline lambda literal, equivalent to the block itself;
       building it per element has no observable side effect */
    int inline_lambda = !strcmp(exty, "LambdaNode");
    if (!simple_ref && !method_obj && !inline_lambda) continue;
    TyKind ct = infer_type(c, ex);
    if (ct != TY_PROC && ct != TY_METHOD) continue;  /* Proc / Method values */
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    const char *name = nt_str(nt, id, "name");
    if (!name) continue;
    int encl = c->nscope[id];
    TyKind rt = infer_type(c, recv);
    /* Hash `each`/`each_pair` yields the [k,v] pair, forwarded as a single array
       argument `c.call([k, v])` (built below) -- correct for every arity: a
       1-param callable gets the pair, a 2-param proc auto-splats it, a 2-param
       lambda raises exactly as CRuby's `Hash#each(&lambda)` does. A Method object
       takes the pair via its array ABI; a proc/lambda value's param is typed as
       the pair array by the call-site argument inference (a container arg
       overrides the bare-int default). */
    TyKind pty[4];
    int arity;
    if (!strcmp(name, "each_with_object")) {
      /* each_with_object(init) { |elem, memo| }: two params, the element and the
         accumulator. Array receivers only (a `{}` hash memo is unsupported even
         for a literal block). The memo type is recovered from how the callable
         fills it (ewo_memo_elem_type) by infer_block_params, so seed it UNKNOWN;
         the wrap_pair / hash logic below does not apply. */
      if (!ty_is_array(rt)) continue;
      arity = 2;
      pty[0] = ty_array_elem(rt);
      pty[1] = TY_UNKNOWN;
    } else {
      arity = ty_block_yield(rt, name, pty, 4);
      if (arity < 1) continue;  /* not a context-free iterator (or recv unresolved) */
    }

    /* Hash forwarding. A Method object takes any hash iterator's yield directly
       through its array ABI (the [k,v] pair as one array for `each`, the bare
       key/value for `each_key`/`each_value`). A proc/lambda VALUE is reliable
       only for the `each` pair forwarded to a single param, whose pair-array
       type the call-site inference recovers (a container overriding the bare-int
       default). Every other hash + proc/lambda combination -- inline lambdas
       (cloned, no write to type from), multi-param procs (CRuby auto-splat), and
       the scalar `each_key`/`each_value` yields -- needs cross-procedural param
       typing not yet modeled, so decline to the pre-existing path. */
    int wrap_pair = 0;
    if (ty_is_hash(rt)) {
      if (ct == TY_METHOD) {
        wrap_pair = (arity == 2);  /* each: pair as array; each_key/value: bare value */
      } else {
        /* a proc/lambda (value or inline): the call-site inference types its
           params from the forwarded call. `each` to a 1-param callable gets the
           [k,v] pair as one array; to a 2-param one, k and v positionally
           (auto-splat by arity); each_key/each_value pass the bare key/value. */
        int cpc = fwd_callable_arity(c, ex);
        if (arity == 2 && cpc == 1) wrap_pair = 1;
        else if (arity == 2 && cpc == 2) wrap_pair = 0;
        else if (arity == 1) wrap_pair = 0;
        else continue;  /* arity-2 with unresolved callable arity */
      }
    }

    int base = nt->count;
    int proc_clone = nt_clone_subtree(nt, ex);  /* re-read the proc per element */
    if (proc_clone < 0) continue;

    int reqs[4], reads[4];
    char pn[48];
    int alloc_ok = 1;
    for (int k = 0; k < arity; k++) {
      snprintf(pn, sizeof pn, "__fwd_%d_%d", id, k);
      reqs[k] = nt_new_node(nt, "RequiredParameterNode");
      nt_node_set_str(nt, reqs[k], "name", pn);
      reads[k] = nt_new_node(nt, "LocalVariableReadNode");
      nt_node_set_str(nt, reads[k], "name", pn);
      if (reqs[k] < 0 || reads[k] < 0) { alloc_ok = 0; break; }
    }
    if (!alloc_ok) continue;  /* node-table OOM: leave the call in its &-form */
    int params = nt_new_node(nt, "ParametersNode");
    nt_node_set_arr(nt, params, "requireds", reqs, arity);
    int bparams = nt_new_node(nt, "BlockParametersNode");
    nt_node_set_ref(nt, bparams, "parameters", params);

    int callargs = nt_new_node(nt, "ArgumentsNode");
    if (wrap_pair) {
      int pairarr = nt_new_node(nt, "ArrayNode");
      nt_node_set_arr(nt, pairarr, "elements", reads, 2);
      nt_node_set_arr(nt, callargs, "arguments", &pairarr, 1);
    }
    else nt_node_set_arr(nt, callargs, "arguments", reads, arity);
    int callnode = nt_new_node(nt, "CallNode");
    nt_node_set_ref(nt, callnode, "receiver", proc_clone);
    nt_node_set_str(nt, callnode, "name", "call");
    nt_node_set_ref(nt, callnode, "arguments", callargs);
    nt_node_set_ref(nt, callnode, "block", -1);

    int body = nt_new_node(nt, "StatementsNode");
    nt_node_set_arr(nt, body, "body", &callnode, 1);
    int blocknode = nt_new_node(nt, "BlockNode");
    if (params < 0 || bparams < 0 || callargs < 0 || callnode < 0 || body < 0 ||
        blocknode < 0)
      continue;  /* node-table OOM: a -1 id is an out-of-bounds node index below */
    nt_node_set_ref(nt, blocknode, "parameters", bparams);
    nt_node_set_ref(nt, blocknode, "body", body);

    nt_node_set_ref(nt, id, "block", blocknode);  /* call now takes a literal block */

    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;

    Scope *bs = comp_scope_of(c, blocknode);
    for (int k = 0; k < arity; k++) {
      snprintf(pn, sizeof pn, "__fwd_%d_%d", id, k);
      LocalVar *lv = scope_local_intern(bs, pn);
      lv->is_block_param = 1;
      lv->type = pty[k];
    }
    changed = 1;
  }
  return changed;
}

/* Desugar a blockless slicing enumerator materialized with `to_a` --
   `recv.each_slice(n).to_a` / `recv.each_cons(n).to_a` -- into the equivalent
   `recv.each_slice(n).map { |__s| __s }`, which the each_slice/each_cons map-fold
   already lowers to a direct slice/window loop. Avoids a full Enumerator object
   for the common materialize-the-slices idiom. */
int desugar_enum_chain_to_a(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  int changed = 0;
  int n0 = nt->count;
  for (int id = 0; id < n0; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || strcmp(nm, "to_a")) continue;
    if (nt_ref(nt, id, "block") >= 0) continue;
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; if (args >= 0) nt_arr(nt, args, "arguments", &ac);
    if (ac != 0) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_CallNode) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn || (strcmp(rn, "each_slice") && strcmp(rn, "each_cons"))) continue;
    if (nt_ref(nt, recv, "block") >= 0) continue;  /* the blockless enumerator form */

    int encl = c->nscope[id];
    int base = nt->count;
    char pn[32]; snprintf(pn, sizeof pn, "__es_%d", id);
    int req = nt_new_node(nt, "RequiredParameterNode"); nt_node_set_str(nt, req, "name", pn);
    int read = nt_new_node(nt, "LocalVariableReadNode"); nt_node_set_str(nt, read, "name", pn);
    int params = nt_new_node(nt, "ParametersNode"); nt_node_set_arr(nt, params, "requireds", &req, 1);
    int bparams = nt_new_node(nt, "BlockParametersNode"); nt_node_set_ref(nt, bparams, "parameters", params);
    int bodyst = nt_new_node(nt, "StatementsNode"); nt_node_set_arr(nt, bodyst, "body", &read, 1);
    int blocknode = nt_new_node(nt, "BlockNode");
    nt_node_set_ref(nt, blocknode, "parameters", bparams);
    nt_node_set_ref(nt, blocknode, "body", bodyst);

    nt_node_set_str(nt, id, "name", "map");        /* to_a -> map { |__s| __s } */
    nt_node_set_ref(nt, id, "block", blocknode);
    comp_grow_node_arrays(c);
    for (int j = base; j < nt->count; j++) c->nscope[j] = encl;
    Scope *bs = comp_scope_of(c, blocknode);
    LocalVar *lv = scope_local_intern(bs, pn); lv->is_block_param = 1;
    changed = 1;
  }
  return changed;
}

/* Propagate `proc.call(args)` argument types onto the proc literal `create`'s
   required params: a concrete arg overrides a param still at its bare-int
   default (the fallback guess, no real evidence), otherwise unify. Returns 1 if
   any param type changed. Shared by the local-proc and inline-lambda call sites. */
static int cs_type_params(Compiler *c, int create, const int *argv, int argc) {
  NodeTable *nt = (NodeTable *)c->nt;
  int pn = a_proc_params_node(c, create);
  if (pn < 0) return 0;
  int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
  Scope *bs = comp_scope_of(c, create);
  int changed = 0;
  for (int k = 0; k < rn && k < argc; k++) {
    const char *p = nt_str(nt, reqs[k], "name");
    if (!p) continue;
    LocalVar *lv = scope_local(bs, p);
    if (!lv) continue;
    TyKind at = infer_type(c, argv[k]);
    if (at == TY_UNKNOWN || at == lv->type) continue;
    TyKind merged = (lv->type == TY_INT) ? at : ty_unify(lv->type, at);
    if (merged != lv->type) { lv->type = merged; changed = 1; }
  }
  return changed;
}

int infer_block_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;

  /* `->(x, ...) {}` (LambdaNode): its params live in the enclosing scope (no
     separate scope), like block params. Register them here; the int default is
     applied later, AFTER the call-site arg-type seeding below, so a `->(t){...}`
     later called as `f.call("x")` types `t` from the call (string) instead of
     unifying a premature int default with it into poly (#1372). */
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
        char nm[16]; snprintf(nm, sizeof nm, "_%d", k);
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
    if (xrecv < 0) {
      /* receiverless instance_exec inside an instance method: params still
         take the call-site arg types; the receiver (self) is irrelevant here. */
      if (strcmp(cname, "instance_exec") || ie_implicit_self_class(c, id) < 0) continue;
    }
    else if (strcmp(cname, "instance_exec")) {
      TyKind xrt = infer_type(c, xrecv);
      if (!ty_is_object(xrt) ||
          comp_trampoline_kind(c, ty_object_class(xrt), cname, NULL) != 2) continue;
    }
    int blk = nt_ref(nt, id, "block");
    if (blk < 0) continue;
    int pn = nt_ref(nt, blk, "parameters");
    if (pn < 0) continue;
    int iargs = nt_ref(nt, id, "arguments");
    int iac = 0; const int *iav = iargs >= 0 ? nt_arr(nt, iargs, "arguments", &iac) : NULL;
    Scope *bs = comp_scope_of(c, blk);
    /* A trailing `k: v` call-site hash is not a positional arg; bind keyword
       block params to it by name. */
    int kwhash = ie_call_kwhash(c, id);
    if (kwhash >= 0) iac -= 1;
    const char *pnty = nt_type(nt, pn);
    if (pnty && !strcmp(pnty, "NumberedParametersNode")) {
      /* `{ _1 + _2 }` / `{ it ... }` (it normalizes to _1): bind _1.._N to the
         call-site arg types. */
      int maxn = (int)nt_int(nt, pn, "maximum", 0);
      for (int k = 0; k < maxn && k < iac; k++) {
        char npn[16]; snprintf(npn, sizeof npn, "_%d", k + 1);
        TyKind at = infer_type(c, iav[k]);
        LocalVar *lv = scope_local_intern(bs, npn); lv->is_block_param = 1;
        if (at != TY_UNKNOWN && lv->type != at) { lv->type = at; changed = 1; }
      }
      continue;
    }
    int inner = nt_ref(nt, pn, "parameters");
    int pnode = inner >= 0 ? inner : pn;
    int rnp = 0; const int *reqs = nt_arr(nt, pnode, "requireds", &rnp);
    /* mixed-args trampoline (`instance_exec(x, @base, 7, &b)`): bind each block
       param to the trampoline body's arg (caller arg substituted for a
       trampoline param read), not the caller's args. */
    int tramp_argc = strcmp(cname, "instance_exec") ? ie_tramp_effective_argc(c, id) : -1;
    /* auto-splat: a single array arg destructured across N>=2 params binds
       each to the element type. A sole splat (`instance_exec(*arr) { |a, b| }`)
       spreads the same way -- unwrap it to its array operand. A splat also
       spreads across a single param (`instance_exec(*arr) { |a| }` binds `a`
       to `arr[0]`), unlike a directly-passed array (which binds the whole array
       to a lone param), so allow `rnp >= 1` when explicitly splatted. */
    int arg0 = (iac == 1 && iav) ? iav[0] : -1;
    int is_splat = arg0 >= 0 && nt_type(nt, arg0) && !strcmp(nt_type(nt, arg0), "SplatNode");
    if (is_splat) arg0 = nt_ref(nt, arg0, "expression");
    if (tramp_argc < 0 && iac == 1 && (rnp >= 2 || (rnp >= 1 && is_splat)) && arg0 >= 0) {
      TyKind a0 = infer_type(c, arg0);
      if (ty_is_array(a0)) {
        TyKind et = ty_array_elem(a0);
        for (int k = 0; k < rnp; k++) {
          const char *p = nt_str(nt, reqs[k], "name");
          if (!p) continue;
          LocalVar *lv = scope_local_intern(bs, p); lv->is_block_param = 1;
          if (et != TY_UNKNOWN && lv->type != et) { lv->type = et; changed = 1; }
        }
        continue;
      }
    }
    for (int k = 0; k < rnp; k++) {
      const char *p = nt_str(nt, reqs[k], "name");
      if (!p) continue;
      int an = tramp_argc >= 0 ? ie_tramp_effective_arg(c, id, k) : (k < iac ? iav[k] : -1);
      if (an < 0) continue;
      TyKind at = infer_type(c, an);
      LocalVar *lv = scope_local_intern(bs, p); lv->is_block_param = 1;
      if (at != TY_UNKNOWN && lv->type != at) { lv->type = at; changed = 1; }
    }
    /* keyword block params (`|k:, j: 5|`): match the call-site `k: v` hash by
       name; an omitted optional keyword takes its default expr's type. */
    int nkw = 0; const int *kws = nt_arr(nt, pnode, "keywords", &nkw);
    for (int k = 0; k < nkw; k++) {
      const char *kpty = nt_type(nt, kws[k]);
      const char *kpn = nt_str(nt, kws[k], "name");
      if (!kpn) continue;
      int vn = ie_kwhash_value(c, kwhash, kpn);
      TyKind kt = TY_UNKNOWN;
      if (vn >= 0) kt = infer_type(c, vn);
      else if (kpty && !strcmp(kpty, "OptionalKeywordParameterNode")) {
        int dv = nt_ref(nt, kws[k], "value");
        if (dv >= 0) kt = infer_type(c, dv);
      }
      LocalVar *lv = scope_local_intern(bs, kpn); lv->is_block_param = 1;
      if (kt != TY_UNKNOWN && lv->type != kt) { lv->type = kt; changed = 1; }
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
    if (recv < 0 || !nt_type(nt, recv)) continue;
    const char *rrty = nt_type(nt, recv);
    int is_const = !strcmp(rrty, "ConstantReadNode") ||
                   (!strcmp(rrty, "ConstantPathNode") && nt_ref(nt, recv, "parent") < 0);
    if (!is_const) continue;
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
    if (!rty) continue;
    int call_args = nt_ref(nt, id, "arguments");
    int argc = 0; const int *argv = NULL;
    if (call_args >= 0) argv = nt_arr(nt, call_args, "arguments", &argc);
    if (argc == 0) continue;
    /* The receiver is itself a proc/lambda literal -- e.g. a desugared inline
       `&->(x){...}` clone, whose params no var write would let us find -- so type
       its own params directly from the call args. */
    if (!strcmp(rty, "LambdaNode") || is_proc_create(c, recv)) {
      if (cs_type_params(c, recv, argv, argc)) changed = 1;
      continue;
    }
    if (strcmp(rty, "LocalVariableReadNode")) continue;
    const char *varname = nt_str(nt, recv, "name");
    if (!varname) continue;
    Scope *call_scope = comp_scope_of(c, id);
    /* otherwise a local holding a proc: type the proc literal assigned to it */
    for (int w = 0; w < nt->count; w++) {
      const char *wty = nt_type(nt, w);
      if (!wty || strcmp(wty, "LocalVariableWriteNode")) continue;
      const char *wname = nt_str(nt, w, "name");
      if (!wname || strcmp(wname, varname)) continue;
      if (comp_scope_of(c, w) != call_scope) continue;
      int val = nt_ref(nt, w, "value");
      if (val < 0 || !is_proc_create(c, val)) continue;
      if (cs_type_params(c, val, argv, argc)) changed = 1;
    }
  }

  /* Lambda param int default, applied AFTER the call-site seeding above so it
     only fills params no call site typed -- the arithmetic-proc fallback,
     matching the proc-literal default loop below (#1372). */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "LambdaNode")) continue;
    int pn = nt_ref(nt, id, "parameters");
    if (pn < 0) continue;
    int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
    Scope *bs = comp_scope_of(c, id);
    for (int k = 0; k < rn; k++) {
      const char *p = nt_str(nt, reqs[k], "name");
      if (!p) continue;
      LocalVar *lv = scope_local(bs, p);
      if (lv && lv->type == TY_UNKNOWN) { lv->type = TY_INT; changed = 1; }
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
      /* A block passed to a pure `...` forwarder is really consumed by the
         method the forward eventually reaches; type its params from there. */
      int yld_mi = mi;
      if (mi >= 0 && !c->scopes[mi].yields &&
          !(c->scopes[mi].blk_param && c->scopes[mi].blk_param[0])) {
        int t = forwarding_yield_target(c, mi, 0);
        if (t >= 0) yld_mi = t;
      }
      if (yld_mi >= 0 && c->scopes[yld_mi].yields) {
        int yn = first_yield(c, yld_mi);
        int ya = yn >= 0 ? nt_ref(nt, yn, "arguments") : first_block_call_args(c, yld_mi);
        if (ya < 0) ya = first_ie_exec_args(c, yld_mi);  /* instance_exec(args, &b) */
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
          if (c->nscope[_yi] != yld_mi) continue;
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
          if (src && an_re_has_captures(src)) has_cap = 1;
        }
      }
      pt = has_cap ? TY_STR_ARRAY : TY_STRING;
    }
    else if ((!strcmp(name, "each") || ty_iter_shape(name) == TY_ITER_MAP ||
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
    else if ((!strcmp(name, "each") || ty_iter_shape(name) == TY_ITER_MAP ||
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
              !strcmp(name, "uniq") || !strcmp(name, "uniq!") ||
              !strcmp(name, "keep_if") || !strcmp(name, "delete_if") ||
              !strcmp(name, "flat_map") || !strcmp(name, "each_with_object") ||
              !strcmp(name, "chunk") || !strcmp(name, "group_by") ||
              !strcmp(name, "tally_by") || !strcmp(name, "min_by_all") ||
              !strcmp(name, "filter_map") || !strcmp(name, "count_by") ||
              !strcmp(name, "partition") || !strcmp(name, "each_slice") ||
              !strcmp(name, "each_cons") || !strcmp(name, "cycle")) &&
             ty_is_array(rt))
      pt = ty_array_elem(rt);
    /* each_index { |i| } binds the index, not the element: always int. */
    else if (!strcmp(name, "each_index") && ty_is_array(rt))
      pt = TY_INT;
    /* TY_POLY receiver with iteration methods: element type is TY_POLY */
    else if (rt == TY_POLY &&
             (!strcmp(name, "each") || ty_iter_shape(name) == TY_ITER_MAP ||
              !strcmp(name, "select") || !strcmp(name, "reject") || !strcmp(name, "find") ||
              !strcmp(name, "detect") || !strcmp(name, "any?") || !strcmp(name, "all?") ||
              !strcmp(name, "uniq") || !strcmp(name, "uniq!") || !strcmp(name, "sort_by") ||
              !strcmp(name, "min_by") || !strcmp(name, "max_by")))
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
    if ((ty_iter_shape(name) == TY_ITER_MAP) && rt == TY_UNKNOWN &&
        nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
        nt_str(nt, recv, "name") && (!strcmp(nt_str(nt, recv, "name"), "each_slice") ||
                                     !strcmp(nt_str(nt, recv, "name"), "each_cons")) &&
        nt_ref(nt, recv, "block") < 0) {
      int es_recv2 = nt_ref(nt, recv, "receiver");
      TyKind arr_t2 = es_recv2 >= 0 ? infer_type(c, es_recv2) : TY_UNKNOWN;
      int is_cons2 = !strcmp(nt_str(nt, recv, "name"), "each_cons");
      if (ty_is_array(arr_t2)) {
        Scope *es2 = comp_scope_of(c, block);
        int np2 = 0; while (block_param_name(c, block, np2)) np2++;
        /* each_cons binds the n-window (array); each_slice binds the slice
           (array) for a single param `|s|`, or destructures into elements when
           there are several params `|a, b|`. A single destructured param
           `|(a, b)|` over either splits the window/slice into its elements. */
        TyKind bp_t2 = is_cons2 ? arr_t2 : (np2 == 1 ? arr_t2 : ty_array_elem(arr_t2));
        if (bp_t2 != TY_UNKNOWN) {
          if (np2 == 0 && block_param_is_multi(c, block, 0)) {
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
    if ((ty_iter_shape(name) == TY_ITER_MAP) && rt == TY_UNKNOWN &&
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

    /* arr.each.with_index(off).inject(init) { |acc, (v,i)| } / { |acc, pair| }
       and arr.each_with_index.inject{...}: type the fold's params over the
       [elem, index] pair enumerator. (matz/spinel#1481) */
    if ((!strcmp(name, "inject") || !strcmp(name, "reduce")) &&
        nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
        nt_ref(nt, recv, "block") < 0) {
      const char *rn = nt_str(nt, recv, "name");
      int chain_arr = -1;
      if (rn && !strcmp(rn, "each_with_index")) chain_arr = nt_ref(nt, recv, "receiver");
      else if (rn && !strcmp(rn, "with_index")) {
        int wir = nt_ref(nt, recv, "receiver");
        if (wir >= 0 && nt_type(nt, wir) && !strcmp(nt_type(nt, wir), "CallNode") &&
            nt_str(nt, wir, "name") && !strcmp(nt_str(nt, wir, "name"), "each") &&
            nt_ref(nt, wir, "block") < 0)
          chain_arr = nt_ref(nt, wir, "receiver");
      }
      TyKind chain_at = chain_arr >= 0 ? infer_type(c, chain_arr) : TY_UNKNOWN;
      if (ty_is_array(chain_at) && block >= 0) {
        TyKind elem = ty_array_elem(chain_at);
        Scope *bs = comp_scope_of(c, block);
        int rargs = nt_ref(nt, id, "arguments"); int rargc = 0;
        const int *rargv = rargs >= 0 ? nt_arr(nt, rargs, "arguments", &rargc) : NULL;
        TyKind acc_t = (rargc > 0 && rargv) ? infer_type(c, rargv[0]) : elem;
        if (acc_t == TY_UNKNOWN) acc_t = elem;
        if (p0) {
          LocalVar *ap = scope_local_intern(bs, p0); ap->is_block_param = 1;
          TyKind m = ty_unify(ap->type, acc_t); if (m != ap->type) { ap->type = m; changed = 1; }
        }
        if (block_param_is_multi(c, block, 1)) {
          int lc = block_param_multi_count(c, block, 1);
          for (int li = 0; li < lc; li++) {
            const char *ln = block_param_multi_leaf(c, block, 1, li);
            if (!ln) continue;
            LocalVar *lp = scope_local_intern(bs, ln); lp->is_block_param = 1;
            TyKind want = (li == 0) ? elem : TY_INT;
            TyKind m = ty_unify(lp->type, want); if (m != lp->type) { lp->type = m; changed = 1; }
          }
        }
        else {
          const char *pp = block_param_name(c, block, 1);
          if (pp) {
            LocalVar *lp = scope_local_intern(bs, pp); lp->is_block_param = 1;
            TyKind pairt = (elem == TY_INT) ? TY_INT_ARRAY : TY_POLY_ARRAY;
            TyKind m = ty_unify(lp->type, pairt); if (m != lp->type) { lp->type = m; changed = 1; }
          }
        }
        continue;
      }
    }

    /* arr.each.with_index(off).<terminal> { |v, i| } / { |(v,i)| } / { |pair| }
       (map/collect/select/filter/reject/count/any?/all?/none?/each over the
       [elem, index] pair enumerator). (matz/spinel#1483) */
    if (block >= 0 &&
        (ty_iter_shape(name) == TY_ITER_MAP || ty_iter_shape(name) == TY_ITER_SELECT ||
         ty_iter_shape(name) == TY_ITER_REJECT || !strcmp(name, "each") ||
         !strcmp(name, "count") || !strcmp(name, "any?") || !strcmp(name, "all?") ||
         !strcmp(name, "none?")) &&
        nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
        nt_ref(nt, recv, "block") < 0) {
      const char *rn = nt_str(nt, recv, "name");
      int chain_arr = -1;
      if (rn && !strcmp(rn, "each_with_index")) chain_arr = nt_ref(nt, recv, "receiver");
      else if (rn && !strcmp(rn, "with_index")) {
        int wir = nt_ref(nt, recv, "receiver");
        if (wir >= 0 && nt_type(nt, wir) && !strcmp(nt_type(nt, wir), "CallNode") &&
            nt_str(nt, wir, "name") && !strcmp(nt_str(nt, wir, "name"), "each") &&
            nt_ref(nt, wir, "block") < 0)
          chain_arr = nt_ref(nt, wir, "receiver");
      }
      TyKind chain_at = chain_arr >= 0 ? infer_type(c, chain_arr) : TY_UNKNOWN;
      /* Only the |v, i| two-param form (v = element, i = index); single-param
         and destructure forms have method-dependent semantics and are left to
         other rules (the codegen path bails on them too). */
      const char *vp = block_param_name(c, block, 0);
      const char *ip = block_param_name(c, block, 1);
      if (ty_is_array(chain_at) && !block_param_is_multi(c, block, 0) && vp && ip) {
        TyKind elem = ty_array_elem(chain_at);
        Scope *bs = comp_scope_of(c, block);
        LocalVar *lp = scope_local_intern(bs, vp); lp->is_block_param = 1;
        TyKind m = ty_unify(lp->type, elem); if (m != lp->type) { lp->type = m; changed = 1; }
        LocalVar *lp2 = scope_local_intern(bs, ip); lp2->is_block_param = 1;
        TyKind m2 = ty_unify(lp2->type, TY_INT); if (m2 != lp2->type) { lp2->type = m2; changed = 1; }
        continue;
      }
    }

    /* array.{map,collect,each,select,filter,reject}.with_index(off) { |x, i| }:
       a blockless enumerator over an array, indexed -- element + int index. */
    if (!strcmp(name, "with_index") &&
        nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
        nt_ref(nt, recv, "block") < 0) {
      const char *inner = nt_str(nt, recv, "name");
      if (inner && (!strcmp(inner, "map") || !strcmp(inner, "collect") ||
                    !strcmp(inner, "each") || !strcmp(inner, "select") ||
                    !strcmp(inner, "filter") || !strcmp(inner, "reject"))) {
        int arr_recv = nt_ref(nt, recv, "receiver");
        TyKind arr_t = arr_recv >= 0 ? infer_type(c, arr_recv) : TY_UNKNOWN;
        if (ty_is_array(arr_t)) {
          Scope *wis = comp_scope_of(c, block);
          if (p0) {
            LocalVar *ep = scope_local_intern(wis, p0); ep->is_block_param = 1;
            TyKind em = ty_unify(ep->type, ty_array_elem(arr_t));
            if (em != ep->type) { ep->type = em; changed = 1; }
          }
          const char *idx_p = block_param_name(c, block, 1);
          if (idx_p) {
            LocalVar *ip = scope_local_intern(wis, idx_p); ip->is_block_param = 1;
            TyKind im = ty_unify(ip->type, TY_INT);
            if (im != ip->type) { ip->type = im; changed = 1; }
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
         !strcmp(name, "minmax") || !strcmp(name, "slice_when") || !strcmp(name, "chunk_while")) && ty_is_array(rt)) {
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
      /* `[[ints],...].inject { |a, b| a & b }`: the inner arrays are int arrays,
         so type both fold params as int arrays rather than poly. */
      if (rt == TY_POLY_ARRAY && comp_is_nested_int_array_literal(c, nt_ref(nt, id, "receiver")))
        et2 = TY_INT_ARRAY;
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
          int from_usage = 0;
          if (at == TY_UNKNOWN) {
            const char *a0ty = nt_type(nt, ewobj_argv[0]);
            int an0 = 0;
            if (a0ty && !strcmp(a0ty, "ArrayNode")) nt_arr(nt, ewobj_argv[0], "elements", &an0);
            if (a0ty && !strcmp(a0ty, "ArrayNode") && an0 == 0) {
              /* empty `[]`: the accumulator element type comes from how the memo
                 is filled (`memo << e`), following a forwarded callable's body. */
              TyKind me = ewo_memo_elem_type(c, id);
              if (me != TY_UNKNOWN) { at = ty_array_of(me); from_usage = 1; }
              else at = TY_INT_ARRAY;
            }
          }
          if (at != TY_UNKNOWN) {
            LocalVar *ap = scope_local_intern(es, p1_name); ap->is_block_param = 1;
            /* A usage-derived type overrides the bare int_array guess (the
               no-evidence default) so an early guess can't widen a later
               str/poly memo to poly_array; otherwise unify. */
            TyKind am = (from_usage && (ap->type == TY_UNKNOWN || ap->type == TY_INT_ARRAY))
                        ? at : ty_unify(ap->type, at);
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
TyKind return_node_type(Compiler *c, int id) {
  int args = nt_ref(c->nt, id, "arguments");
  if (args < 0) return TY_NIL;
  int n = 0;
  const int *a = nt_arr(c->nt, args, "arguments", &n);
  if (n > 1) return TY_POLY_ARRAY;
  return n > 0 ? infer_type(c, a[0]) : TY_NIL;
}

/* Defined in codegen_fold.c (linked in). */
int is_descendant(Compiler *c, int k, int anc);

/* True when the method body's tail statement unconditionally raises, so the C
   function never reaches a return (used to widen its void type to the override
   return type -- the unreachable "return value" can safely take that type). */
static int scope_tail_raises(Compiler *c, int s) {
  const NodeTable *nt = c->nt;
  int body = c->scopes[s].body;
  if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) return 0;
  int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
  if (bn <= 0) return 0;
  int last = bb[bn - 1];
  const char *ty = nt_type(nt, last);
  return ty && !strcmp(ty, "CallNode") && nt_ref(nt, last, "receiver") < 0 &&
         nt_str(nt, last, "name") && !strcmp(nt_str(nt, last, "name"), "raise");
}

int infer_return_types(Compiler *c) {
  const NodeTable *nt = c->nt;
  int changed = 0;
  int ns = c->nscopes;
  /* Accumulate each scope's explicit-return type in a single node pass.
     The naive form rescanned every node for every scope (O(scopes*nodes));
     on a large input that dominates. Group ReturnNodes by their owning scope
     once instead. */
  TyKind *ret_acc = (TyKind *)malloc(sizeof(TyKind) * (size_t)ns);
  char *has_ret = (char *)calloc((size_t)ns, 1);
  if (ret_acc && has_ret) {
    for (int id = 0; id < nt->count; id++) {
      if (nt_kind(nt, id) != NK_ReturnNode) continue;
      Scope *rs = comp_scope_of(c, id);
      if (!rs) continue;
      int si = (int)(rs - c->scopes);
      if (si < 0 || si >= ns) continue;
      TyKind rt = return_node_type(c, id);
      ret_acc[si] = has_ret[si] ? ty_unify(ret_acc[si], rt) : rt;
      has_ret[si] = 1;
    }
  }
  /* implicit return: the body's value */
  for (int s = 1; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    /* Specialized inherited-cls-new copies keep their fixed subclass return
       type (the shared body's bare `new` would otherwise infer the base).
       An --rbs-seeded return is likewise pinned. */
    if (sc->ret_specialized || sc->ret_rbs_seeded) continue;
    /* synthesized compiler_state methods carry a fixed return type (no AST). */
    if (sc->cs_synth) continue;
    /* An empty method body returns nil; if its value is used at all it must
       be poly (a void C function yields nothing to read). */
    int empty_body = sc->body < 0;
    if (sc->body >= 0 && nt_kind(nt, sc->body) == NK_StatementsNode) {
      int bn = 0; nt_arr(nt, sc->body, "body", &bn); if (bn == 0) empty_body = 1;
    }
    TyKind r = empty_body ? TY_POLY : infer_type(c, sc->body);
    /* explicit returns within this scope (collected above) */
    if (has_ret && has_ret[s]) r = ty_unify(r, ret_acc[s]);
    if (r != sc->ret) { sc->ret = r; changed = 1; }
    /* For a method with a &block param, record the value type its block yields
       (unified across all call sites). Blocks passed to it are emitted returning
       this common type so the sp_proc_call ABI is consistent. */
    if (sc->blk_param && sc->blk_param[0] && !sc->yields && !sc->is_lowered_yield) {
      TyKind bvt = yield_value_type(c, (int)(sc - c->scopes));
      if (bvt != TY_UNKNOWN && sc->blk_ret != (int)bvt) { sc->blk_ret = (int)bvt; changed = 1; }
    }
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

  /* An abstract base method (`def self.table_name; raise; end`) infers a void
     return, but a subclass overrides it with a value-returning version. A call
     bound to the base in value position would then assign void into a temp and
     fail to compile (#1416). Since the base body always raises, its return is
     unreachable -- widen its type to the override return(s), so the call is
     usable. Only raising bases qualify (a genuinely nil-returning void method
     must stay void). */
  for (int s = 1; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (sc->ret != TY_VOID || sc->class_id < 0 || !sc->name) continue;
    if (sc->ret_specialized || sc->ret_rbs_seeded || sc->cs_synth) continue;
    if (!scope_tail_raises(c, s)) continue;
    TyKind unified = TY_VOID;
    for (int t = 1; t < c->nscopes; t++) {
      Scope *ot = &c->scopes[t];
      if (t == s || !ot->name || strcmp(ot->name, sc->name)) continue;
      if (ot->is_cmethod != sc->is_cmethod || ot->class_id < 0) continue;
      if (!is_descendant(c, ot->class_id, sc->class_id)) continue;
      if (ot->ret == TY_VOID || ot->ret == TY_UNKNOWN) continue;
      unified = (unified == TY_VOID) ? ot->ret : ty_unify(unified, ot->ret);
    }
    if (unified != TY_VOID) { sc->ret = unified; changed = 1; }
  }

  free(ret_acc); free(has_ret);
  return changed;
}

/* Collect CallNode names in the subtree rooted at `id`, stopping at nested
   DefNodes (which are separate method scopes). `out` / `n` / `cap` are
   the dynamic string array to append to. */
void cr_collect_calls(const NodeTable *nt, int id,
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

/* ---- Loop-growth bigint promotion ----
   The legacy compiler's pre_detect_bigint, ported: inside a while loop a
   local rebuilt by self-referential multiplication (x = a * b or x *= y
   where an operand flows back from x through local-to-local assignments)
   or by fibonacci-shaped addition (x = a + b where BOTH operands flow
   back from x) grows without bound; promote it from int to bigint. The
   main inference fixpoint then spreads bigint through arithmetic results
   and assignment chains (ty_unify keeps int+bigint at bigint). */

#define BI_MAX_PAIRS 256

typedef struct { const char *dst, *src; } BiPair;

static const char *bi_local_name(const NodeTable *nt, int id) {
  if (id < 0) return NULL;
  const char *ty = nt_type(nt, id);
  if (!ty || strcmp(ty, "LocalVariableReadNode")) return NULL;
  return nt_str(nt, id, "name");
}

/* Collect `dst = src` local-to-local assignments in the loop subtree. */
static void bi_collect_assigns(const NodeTable *nt, int id, BiPair *pairs, int *np) {
  if (id < 0) return;
  const char *ty = nt_type(nt, id);
  if (!ty || !strcmp(ty, "DefNode") || !strcmp(ty, "ClassNode") || !strcmp(ty, "ModuleNode")) return;
  if (!strcmp(ty, "LocalVariableWriteNode")) {
    const char *src = bi_local_name(nt, nt_ref(nt, id, "value"));
    const char *dst = nt_str(nt, id, "name");
    if (src && dst && *np < BI_MAX_PAIRS) { pairs[*np].dst = dst; pairs[*np].src = src; (*np)++; }
  }
  /* `a, b = c, d` style multi-writes also carry values between locals. */
  if (!strcmp(ty, "MultiWriteNode")) {
    int ln = 0, rn = 0;
    const int *lhs = nt_arr(nt, id, "lefts", &ln);
    int v = nt_ref(nt, id, "value");
    const int *rhs = NULL;
    if (v >= 0 && nt_type(nt, v) && !strcmp(nt_type(nt, v), "ArrayNode"))
      rhs = nt_arr(nt, v, "elements", &rn);
    for (int k = 0; lhs && rhs && k < ln && k < rn; k++) {
      const char *lty = nt_type(nt, lhs[k]);
      if (!lty || strcmp(lty, "LocalVariableTargetNode")) continue;
      const char *src = bi_local_name(nt, rhs[k]);
      const char *dst = nt_str(nt, lhs[k], "name");
      if (src && dst && *np < BI_MAX_PAIRS) { pairs[*np].dst = dst; pairs[*np].src = src; (*np)++; }
    }
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) bi_collect_assigns(nt, nt_ref_at(nt, id, i), pairs, np);
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++) bi_collect_assigns(nt, ids[j], pairs, np);
  }
}

/* Does `var`'s value flow into `target` through the assignment pairs? */
static int bi_reaches(const BiPair *pairs, int np, const char *var, const char *target, int depth) {
  if (!strcmp(var, target)) return 1;
  if (depth > 10) return 0;
  for (int i = 0; i < np; i++)
    if (!strcmp(pairs[i].src, var) &&
        bi_reaches(pairs, np, pairs[i].dst, target, depth + 1)) return 1;
  return 0;
}

static void bi_promote(Compiler *c, int write_id, const char *lname) {
  Scope *s = comp_scope_of(c, write_id);
  LocalVar *lv = s ? scope_local(s, lname) : NULL;
  if (lv && !lv->rbs_seeded && (lv->type == TY_UNKNOWN || lv->type == TY_INT)) lv->type = TY_BIGINT;
}

static void bi_scan_loop_node(Compiler *c, int id, const BiPair *pairs, int np) {
  const NodeTable *nt = c->nt;
  if (id < 0) return;
  const char *ty = nt_type(nt, id);
  if (!ty || !strcmp(ty, "DefNode") || !strcmp(ty, "ClassNode") || !strcmp(ty, "ModuleNode")) return;
  if (!strcmp(ty, "LocalVariableWriteNode")) {
    const char *lname = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    const char *vty = v >= 0 ? nt_type(nt, v) : NULL;
    if (lname && vty && !strcmp(vty, "CallNode")) {
      const char *op = nt_str(nt, v, "name");
      const char *rname = bi_local_name(nt, nt_ref(nt, v, "receiver"));
      const char *aname = NULL;
      int args = nt_ref(nt, v, "arguments");
      int an = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an >= 1) aname = bi_local_name(nt, argv[0]);
      if (op && (!strcmp(op, "*") || !strcmp(op, "**"))) {
        if ((rname && bi_reaches(pairs, np, lname, rname, 0)) ||
            (aname && bi_reaches(pairs, np, lname, aname, 0)))
          bi_promote(c, id, lname);
      }
      else if (op && !strcmp(op, "+")) {
        /* fibonacci shape: BOTH operands flow back from lname; this
           rejects the linear `i = i + 1`. */
        if (rname && aname &&
            bi_reaches(pairs, np, lname, rname, 0) &&
            bi_reaches(pairs, np, lname, aname, 0))
          bi_promote(c, id, lname);
      }
    }
  }
  if (!strcmp(ty, "LocalVariableOperatorWriteNode")) {
    const char *op = nt_str(nt, id, "binary_operator");
    const char *lname = nt_str(nt, id, "name");
    if (op && lname && (!strcmp(op, "*") || !strcmp(op, "**")))
      bi_promote(c, id, lname);
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) bi_scan_loop_node(c, nt_ref_at(nt, id, i), pairs, np);
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++) bi_scan_loop_node(c, ids[j], pairs, np);
  }
}

/* Run the self-referential-multiply scan over one loop body subtree. */
static void bi_scan_loop_body(Compiler *c, int body) {
  if (body < 0) return;
  BiPair pairs[BI_MAX_PAIRS];
  int np = 0;
  bi_collect_assigns(c->nt, body, pairs, &np);
  bi_scan_loop_node(c, body, pairs, np);
}

/* An iteration method whose block runs an unbounded number of times, so an
   accumulator multiplied inside it can grow without bound. Only consulted in
   promote mode (the wrap-pinned optcarrot must not pay a block-loop bigint
   widening, which is why the default path stays `while`-only). */
static int bi_is_block_loop_method(const char *name) {
  return !strcmp(name, "times") || !strcmp(name, "each") ||
         !strcmp(name, "upto") || !strcmp(name, "downto") ||
         !strcmp(name, "step") || !strcmp(name, "loop") ||
         !strcmp(name, "each_with_index");
}

void infer_bigint_loop_locals(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (!strcmp(ty, "WhileNode")) {
      bi_scan_loop_body(c, nt_ref(nt, id, "statements"));
      continue;
    }
    /* Promote mode additionally treats block-iteration loops as growth sites:
       `n.times { f = f * x }`, `(a..b).each { ... }`, etc. The block body is a
       BlockNode -> statements; reuse the same self-referential-multiply scan. */
    if (g_promote_mode && !strcmp(ty, "CallNode")) {
      const char *mname = nt_str(nt, id, "name");
      int block = nt_ref(nt, id, "block");
      if (mname && bi_is_block_loop_method(mname) && block >= 0 &&
          nt_type(nt, block) && !strcmp(nt_type(nt, block), "BlockNode"))
        bi_scan_loop_body(c, nt_ref(nt, block, "body"));
    }
  }
}
