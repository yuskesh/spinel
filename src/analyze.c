#include "analyze_internal.h"

void compute_reachable(Compiler *c) {
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

  /* The synthesized compiler_state dump method calls ir_emit_int/str/sa/ia,
     but it has no AST so the BFS above can't see those calls. Mark them
     reachable when any class declares compiler_state fields. */
  {
    int any_cs = 0;
    for (int ci = 0; ci < c->nclasses; ci++) if (c->classes[ci].ncs > 0) { any_cs = 1; break; }
    if (any_cs) {
      MARK_NAME("ir_emit_int"); MARK_NAME("ir_emit_str");
      MARK_NAME("ir_emit_sa");  MARK_NAME("ir_emit_ia");
      while (qhead < qtail) { int s = queue[qhead++]; for (int ni = 0; ni < sc_n[s]; ni++) MARK_NAME(scope_calls[s][ni]); }
    }
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
/* ANameSet: moved to analyze_internal.h */
int aname_has(ANameSet *s, const char *nm) {
  if (!nm) return 1;
  for (int i = 0; i < s->n; i++) if (!strcmp(s->v[i], nm)) return 1;
  return 0;
}
void aname_add(ANameSet *s, const char *nm) {
  if (aname_has(s, nm)) return;
  if (s->n >= s->cap) { s->cap = s->cap ? s->cap * 2 : 8; s->v = realloc(s->v, sizeof(char *) * (size_t)s->cap); }
  s->v[s->n++] = nm;
}
int a_nested_block(const char *ty) { return ty && (!strcmp(ty, "BlockNode") || !strcmp(ty, "LambdaNode")); }
int a_is_local_node(const char *ty) {
  return ty && (!strcmp(ty, "LocalVariableReadNode") || !strcmp(ty, "LocalVariableWriteNode") ||
                !strcmp(ty, "LocalVariableTargetNode") || !strcmp(ty, "LocalVariableOperatorWriteNode") ||
                !strcmp(ty, "LocalVariableOrWriteNode") || !strcmp(ty, "LocalVariableAndWriteNode"));
}
int a_is_write_node(const char *ty) {
  return ty && (!strcmp(ty, "LocalVariableWriteNode") || !strcmp(ty, "LocalVariableTargetNode") ||
                !strcmp(ty, "LocalVariableOperatorWriteNode") || !strcmp(ty, "LocalVariableOrWriteNode") ||
                !strcmp(ty, "LocalVariableAndWriteNode"));
}
/* Mark every node id in the subtree (crossing nested blocks: a node inside an
   inner block is still "inside a proc"). */
void a_mark_subtree(Compiler *c, int id, char *inproc) {
  if (id < 0) return;
  inproc[id] = 1;
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(c->nt, id, i); if (ch >= 0) a_mark_subtree(c, ch, inproc); }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n); for (int k = 0; k < n; k++) if (ids[k] >= 0) a_mark_subtree(c, ids[k], inproc); }
}
/* Names used (read or written) directly in the proc body, not crossing nested
   blocks. */
void a_collect_used(Compiler *c, int id, ANameSet *out) {
  if (id < 0) return;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return;
  if (a_is_local_node(ty)) aname_add(out, nt_str(c->nt, id, "name"));
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(c->nt, id, i); if (ch >= 0 && !a_nested_block(nt_type(c->nt, ch))) a_collect_used(c, ch, out); }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n); for (int k = 0; k < n; k++) if (ids[k] >= 0 && !a_nested_block(nt_type(c->nt, ids[k]))) a_collect_used(c, ids[k], out); }
}
int a_proc_params_node(Compiler *c, int create) {
  const char *ty = nt_type(c->nt, create);
  if (ty && !strcmp(ty, "LambdaNode")) return nt_ref(c->nt, create, "parameters");
  int block = nt_ref(c->nt, create, "block");
  if (block < 0) return -1;
  int bp = nt_ref(c->nt, block, "parameters");
  return bp < 0 ? -1 : nt_ref(c->nt, bp, "parameters");
}
int a_proc_body(Compiler *c, int create) {
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
/* A plain block `m(args) { ... }` passed to a method that keeps a real &block
   parameter (not yield-inlined) is lifted to a standalone proc function, so it
   captures enclosing variables exactly like a proc literal. */
int a_block_is_lifted(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty || strcmp(ty, "CallNode")) return 0;
  int blk = nt_ref(nt, id, "block");
  if (blk < 0 || !nt_type(nt, blk) || strcmp(nt_type(nt, blk), "BlockNode")) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int recv = nt_ref(nt, id, "receiver");
  int mi = -1;
  if (recv < 0) {
    mi = comp_method_index(c, name);
    if (mi < 0) { Scope *self = comp_scope_of(c, id); if (self && self->class_id >= 0) mi = comp_method_in_chain(c, self->class_id, name, NULL); }
  }
else {
    TyKind rt = infer_type(c, recv);
    if (ty_is_object(rt)) mi = comp_method_in_chain(c, ty_object_class(rt), name, NULL);
  }
  if (mi < 0) return 0;
  Scope *m = &c->scopes[mi];
  if (!m->blk_param || !m->blk_param[0] || m->yields || m->is_lowered_yield) return 0;
  /* instance_eval/exec trampolines splice their block at the call site rather
     than lifting it to a proc, so they are not lifted-block captures. */
  if (m->class_id >= 0 && !m->is_cmethod && m->name &&
      comp_trampoline_kind(c, m->class_id, m->name, NULL)) return 0;
  return 1;
}

int a_proc_create_or_lifted(Compiler *c, int id) {
  return is_proc_create(c, id) || a_block_is_lifted(c, id);
}

void mark_proc_captures(Compiler *c) {
  const NodeTable *nt = c->nt;
  char *inproc = (char *)calloc((size_t)nt->count, 1);
  if (!inproc) return;
  for (int id = 0; id < nt->count; id++)
    if (a_proc_create_or_lifted(c, id)) { int body = a_proc_body(c, id); if (body >= 0) a_mark_subtree(c, body, inproc); }

  for (int id = 0; id < nt->count; id++) {
    if (!a_proc_create_or_lifted(c, id)) continue;
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
void bigint_scan_body(const NodeTable *nt, int id, char ***names, int *n, int *cap) {
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

void detect_bigint_loop_vars(Compiler *c) {
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
void propagate_bigint_cascade(Compiler *c) {
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
   receiver's class during inference (codegen mirrors this via an_ie_class_id). */
int *g_ie_node_class = NULL;

void mark_ie_subtree(Compiler *c, int node, int cls) {
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
void build_ie_map(Compiler *c) {
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
int ie_class_of(Compiler *c, int node) {
  (void)c;
  return (g_ie_node_class && node >= 0) ? g_ie_node_class[node] : -1;
}

/* ---- Block/lambda parameter alpha-renaming ----------------------------
 * Block and lambda parameters are interned into the *enclosing* scope, so a
 * parameter sharing a name with an enclosing local collapses onto a single
 * LocalVar (hence one type), corrupting both. Ruby semantics say the two are
 * distinct (the parameter shadows). When the name is also assigned outside the
 * block body -- the case that pollutes the shared type -- rename the parameter
 * and its in-body references to a fresh, collision-free name so they become
 * separate variables. Runs before walk_scope so all downstream interning and
 * codegen see the disambiguated names. */

/* The ParametersNode for a block (BlockParametersNode -> ParametersNode) or a
   lambda (ParametersNode directly). -1 if none / not a plain ParametersNode. */
int blkp_params_node(Compiler *c, int create) {
  const NodeTable *nt = c->nt;
  int pn = nt_ref(nt, create, "parameters");
  if (pn < 0) return -1;
  const char *pty = nt_type(nt, pn);
  if (pty && !strcmp(pty, "BlockParametersNode")) pn = nt_ref(nt, pn, "parameters");
  return pn;
}

int blkp_binds_param(Compiler *c, int create, const char *name) {
  int pn = blkp_params_node(c, create);
  if (pn < 0) return 0;
  const char *pty = nt_type(c->nt, pn);
  if (!pty || strcmp(pty, "ParametersNode")) return 0;
  int rn = 0; const int *reqs = nt_arr(c->nt, pn, "requireds", &rn);
  for (int i = 0; i < rn; i++) {
    const char *p = nt_str(c->nt, reqs[i], "name");
    if (p && !strcmp(p, name)) return 1;
  }
  return 0;
}

int lv_node_is_named_ref(const char *ty) {
  return ty && (!strcmp(ty, "LocalVariableReadNode") || !strcmp(ty, "LocalVariableWriteNode") ||
                !strcmp(ty, "LocalVariableTargetNode") || !strcmp(ty, "LocalVariableOperatorWriteNode") ||
                !strcmp(ty, "LocalVariableOrWriteNode") || !strcmp(ty, "LocalVariableAndWriteNode"));
}
int lv_node_is_write(const char *ty) {
  return ty && (!strcmp(ty, "LocalVariableWriteNode") || !strcmp(ty, "LocalVariableTargetNode") ||
                !strcmp(ty, "LocalVariableOperatorWriteNode") || !strcmp(ty, "LocalVariableOrWriteNode") ||
                !strcmp(ty, "LocalVariableAndWriteNode"));
}

/* Rewrite references to `oldn` -> `newn`, stopping at nested defs/classes and
   at nested blocks/lambdas that re-bind `oldn`. */
void blkp_rewrite_refs(Compiler *c, int node, const char *oldn, const char *newn) {
  if (node < 0) return;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  if (!ty) return;
  if (!strcmp(ty, "DefNode") || !strcmp(ty, "ClassNode") || !strcmp(ty, "ModuleNode")) return;
  if ((!strcmp(ty, "BlockNode") || !strcmp(ty, "LambdaNode")) && blkp_binds_param(c, node, oldn)) return;
  if (lv_node_is_named_ref(ty)) {
    const char *nm = nt_str(nt, node, "name");
    if (nm && !strcmp(nm, oldn)) nt_set_str((NodeTable *)nt, node, "name", newn);
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) blkp_rewrite_refs(c, nt_ref_at(nt, node, i), oldn, newn);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(nt, node, i, &n); for (int k = 0; k < n; k++) blkp_rewrite_refs(c, ids[k], oldn, newn); }
}

void blkp_mark_subtree(const NodeTable *nt, int node, char *marks) {
  if (node < 0) return;
  marks[node] = 1;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) blkp_mark_subtree(nt, nt_ref_at(nt, node, i), marks);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(nt, node, i, &n); for (int k = 0; k < n; k++) blkp_mark_subtree(nt, ids[k], marks); }
}

/* Iteration block parameters (each/map/select/...) are already shadow-safe via
   a codegen save/restore around the inlined body. Renaming is only needed for
   forms the inliner does not cover: lambdas lowered to standalone proc
   functions, and instance_eval/exec (and trampoline) block bodies spliced at
   the call site. Returns 1 if `L` is such a node. */
int blkp_needs_rename(Compiler *c, int L) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, L);
  if (ty && !strcmp(ty, "LambdaNode")) return 1;
  if (!ty || strcmp(ty, "BlockNode")) return 0;
  /* Find the CallNode that owns this block. Rename for forms whose block
     params are typed by a dedicated param-inference pass (so a renamed slot
     still gets a type): instance_eval/exec splice bodies and inject/reduce
     folds. Ordinary iteration blocks (each/map/...) keep the inliner's
     save/restore over the shared slot and must not be renamed. */
  for (int id = 0; id < nt->count; id++) {
    if (nt_ref(nt, id, "block") != L) continue;
    const char *cn = nt_str(nt, id, "name");
    if (!cn) return 0;
    if (!strcmp(cn, "instance_eval") || !strcmp(cn, "instance_exec") ||
        !strcmp(cn, "inject") || !strcmp(cn, "reduce")) return 1;
    /* blocks lifted to standalone proc/fiber functions: proc {} / lambda {} /
       Proc.new {} / Fiber.new {} / Thread.new {}. Their params are typed by a
       dedicated pass, so renaming a shadowing param is safe. */
    int rcv = nt_ref(nt, id, "receiver");
    if (rcv < 0 && (!strcmp(cn, "proc") || !strcmp(cn, "lambda"))) return 1;
    if (rcv >= 0 && !strcmp(cn, "new")) {
      const char *rty = nt_type(nt, rcv);
      int is_const = rty && (!strcmp(rty, "ConstantReadNode") ||
                             (!strcmp(rty, "ConstantPathNode") && nt_ref(nt, rcv, "parent") < 0));
      const char *rn = is_const ? nt_str(nt, rcv, "name") : NULL;
      if (rn && (!strcmp(rn, "Proc") || !strcmp(rn, "Fiber") || !strcmp(rn, "Thread"))) return 1;
    }
    return 0;
  }
  return 0;
}

/* ---- Colliding nested-constant qualification --------------------------
 * Constants live in a flat cst_<NAME> namespace, so `RootNS::Mid::LEAF` and
 * `Lex::RootNS::Mid::LEAF` collide. When the same constant name is written
 * under 2+ distinct module paths, rename each nested write to a qualified
 * `<Mod>__..__<NAME>` and rewrite every path read to whichever qualified
 * constant it denotes (relative reads prefer the lexically enclosing module
 * chain; `::`-anchored reads resolve from the root). Collision-gated: programs
 * with unique constant names are untouched. */


/* QCWrite: moved to analyze_internal.h */

void qc_collect_writes(Compiler *c, int node, char (*path)[64], int depth,
                              QCWrite **ws, int *n, int *cap) {
  const NodeTable *nt = c->nt;
  if (node < 0) return;
  const char *ty = nt_type(nt, node);
  if (!ty) return;
  if ((!strcmp(ty, "ModuleNode") || !strcmp(ty, "ClassNode")) && depth < QC_MAXDEPTH) {
    int cp = nt_ref(nt, node, "constant_path");
    const char *mn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (mn) {
      snprintf(path[depth], 64, "%s", mn);
      depth++;
    }
  }
  else if (!strcmp(ty, "ConstantWriteNode")) {
    const char *nm = nt_str(nt, node, "name");
    if (nm) {
      if (*n >= *cap) { *cap = *cap ? *cap * 2 : 16; *ws = realloc(*ws, sizeof(QCWrite) * (size_t)*cap); }
      QCWrite *w = &(*ws)[(*n)++];
      w->node = node; w->depth = depth;
      for (int i = 0; i < depth; i++) snprintf(w->path[i], 64, "%s", path[i]);
      snprintf(w->name, sizeof w->name, "%s", nm);
    }
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) qc_collect_writes(c, nt_ref_at(nt, node, i), path, depth, ws, n, cap);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) { int m = 0; const int *ids = nt_arr_at(nt, node, i, &m); for (int k = 0; k < m; k++) qc_collect_writes(c, ids[k], path, depth, ws, n, cap); }
}

/* Reconstruct a path read's chain (["RootNS","Mid","LEAF"]) and whether it is
   root-anchored. Returns the chain length, or 0 if unsupported. */
int qc_read_chain(const NodeTable *nt, int node, char (*chain)[64], int *abs_anchor) {
  char rev[QC_MAXDEPTH + 1][64];
  int n = 0;
  int cur = node;
  *abs_anchor = 0;
  while (cur >= 0 && n <= QC_MAXDEPTH) {
    const char *ty = nt_type(nt, cur);
    const char *nm = nt_str(nt, cur, "name");
    if (!ty || !nm) return 0;
    snprintf(rev[n++], 64, "%s", nm);
    if (!strcmp(ty, "ConstantReadNode")) break;
    if (strcmp(ty, "ConstantPathNode")) return 0;
    int par = nt_ref(nt, cur, "parent");
    if (par < 0) { *abs_anchor = 1; break; }
    cur = par;
  }
  for (int i = 0; i < n; i++) snprintf(chain[i], 64, "%s", rev[n - 1 - i]);
  return n;
}

void qc_qualified_name(char *out, size_t cap, const QCWrite *w) {
  out[0] = 0;
  for (int i = 0; i < w->depth; i++) { strncat(out, w->path[i], cap - strlen(out) - 1); strncat(out, "__", cap - strlen(out) - 1); }
  strncat(out, w->name, cap - strlen(out) - 1);
}

void qc_rewrite_reads(Compiler *c, int node, char (*mods)[64], int mdepth,
                             QCWrite *ws, int wn) {
  const NodeTable *nt = c->nt;
  if (node < 0) return;
  const char *ty = nt_type(nt, node);
  if (!ty) return;
  int depth = mdepth;
  char (*path)[64] = mods;
  if ((!strcmp(ty, "ModuleNode") || !strcmp(ty, "ClassNode")) && depth < QC_MAXDEPTH) {
    int cp = nt_ref(nt, node, "constant_path");
    const char *mn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (mn) { snprintf(path[depth], 64, "%s", mn); depth++; }
  }
  else if (!strcmp(ty, "ConstantReadNode")) {
    /* bare constant read inside a module body: resolve lexically innermost-
       first against the colliding writes. Skip reads that are a path's parent
       (handled via the chain) or a class/module definition name. */
    const char *nm = nt_str(nt, node, "name");
    int part_of_other = 0;
    for (int q = 0; q < nt->count && !part_of_other; q++) {
      const char *qt = nt_type(nt, q);
      if (!qt) continue;
      if (!strcmp(qt, "ConstantPathNode") && nt_ref(nt, q, "parent") == node) part_of_other = 1;
      if ((!strcmp(qt, "ClassNode") || !strcmp(qt, "ModuleNode")) &&
          nt_ref(nt, q, "constant_path") == node) part_of_other = 1;
    }
    if (nm && !part_of_other) {
      int involved = 0;
      for (int i = 0; i < wn; i++) if (!strcmp(ws[i].name, nm)) { involved = 1; break; }
      if (involved) {
        for (int pref = depth; pref >= 0; pref--) {
          int matched = -1;
          for (int i = 0; i < wn && matched < 0; i++) {
            if (strcmp(ws[i].name, nm) || ws[i].depth != pref) continue;
            int ok = 1;
            for (int j = 0; j < pref && ok; j++) if (strcmp(ws[i].path[j], path[j])) ok = 0;
            if (ok) matched = i;
          }
          if (matched >= 0) {
            if (ws[matched].depth > 0) {
              char qn[512]; qc_qualified_name(qn, sizeof qn, &ws[matched]);
              nt_set_str((NodeTable *)nt, node, "name", qn);
            }
            break;
          }
        }
      }
    }
  }
  else if (!strcmp(ty, "ConstantPathNode")) {
    /* only process path heads: skip if this node is some other path's parent */
    int is_parent = 0;
    for (int q = 0; q < nt->count && !is_parent; q++) {
      const char *qt = nt_type(nt, q);
      if (qt && !strcmp(qt, "ConstantPathNode") && nt_ref(nt, q, "parent") == node) is_parent = 1;
    }
    if (!is_parent) {
      char chain[QC_MAXDEPTH + 1][64];
      int abs_anchor = 0;
      int cl = qc_read_chain(nt, node, chain, &abs_anchor);
      if (cl >= 2) {
        const char *cname = chain[cl - 1];
        /* does this name participate in a collision? */
        int involved = 0;
        for (int i = 0; i < wn; i++) if (!strcmp(ws[i].name, cname)) { involved = 1; break; }
        if (involved) {
          /* try lexical prefixes innermost-first (relative), or only the root (::) */
          int max_pref = abs_anchor ? 0 : depth;
          for (int pref = max_pref; pref >= 0; pref--) {
            int matched = -1;
            for (int i = 0; i < wn && matched < 0; i++) {
              if (strcmp(ws[i].name, cname)) continue;
              if (ws[i].depth != pref + (cl - 1)) continue;
              int ok = 1;
              for (int j = 0; j < pref && ok; j++) if (strcmp(ws[i].path[j], path[j])) ok = 0;
              for (int j = 0; j < cl - 1 && ok; j++) if (strcmp(ws[i].path[pref + j], chain[j])) ok = 0;
              if (ok) matched = i;
            }
            if (matched >= 0) {
              if (ws[matched].depth > 0) {
                char qn[512]; qc_qualified_name(qn, sizeof qn, &ws[matched]);
                nt_set_str((NodeTable *)nt, node, "name", qn);
              }
              break;
            }
          }
        }
      }
    }
  }
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) qc_rewrite_reads(c, nt_ref_at(nt, node, i), path, depth, ws, wn);
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) { int m = 0; const int *ids = nt_arr_at(nt, node, i, &m); for (int k = 0; k < m; k++) qc_rewrite_reads(c, ids[k], path, depth, ws, wn); }
}

void qualify_colliding_consts(Compiler *c) {
  const NodeTable *nt = c->nt;
  QCWrite *ws = NULL; int wn = 0, wcap = 0;
  char path[QC_MAXDEPTH][64];
  qc_collect_writes(c, nt->root_id, path, 0, &ws, &wn, &wcap);
  /* keep only names written under 2+ distinct module paths */
  int any = 0;
  for (int i = 0; i < wn; i++) {
    int collide = 0;
    for (int j = 0; j < wn && !collide; j++) {
      if (i == j || strcmp(ws[i].name, ws[j].name)) continue;
      if (ws[i].depth != ws[j].depth) { collide = 1; break; }
      for (int k = 0; k < ws[i].depth; k++) if (strcmp(ws[i].path[k], ws[j].path[k])) { collide = 1; break; }
    }
    if (!collide) { ws[i] = ws[--wn]; i--; continue; }
    any = 1;
  }
  if (any) {
    /* rewrite reads first (they match against the original write names) */
    char mods[QC_MAXDEPTH][64];
    qc_rewrite_reads(c, nt->root_id, mods, 0, ws, wn);
    /* then qualify the nested writes themselves */
    for (int i = 0; i < wn; i++) {
      if (ws[i].depth == 0) continue;
      char qn[512]; qc_qualified_name(qn, sizeof qn, &ws[i]);
      nt_set_str((NodeTable *)nt, ws[i].node, "name", qn);
    }
  }
  free(ws);
}

void rename_shadowing_block_params(Compiler *c) {
  const NodeTable *nt = c->nt;
  int n = nt->count;
  char *inbody = malloc((size_t)n);
  if (!inbody) return;
  for (int L = 0; L < n; L++) {
    const char *ty = nt_type(nt, L);
    if (!ty || (strcmp(ty, "BlockNode") && strcmp(ty, "LambdaNode"))) continue;
    if (!blkp_needs_rename(c, L)) continue;
    int pn = blkp_params_node(c, L);
    if (pn < 0) continue;
    const char *pty = nt_type(nt, pn);
    if (!pty || strcmp(pty, "ParametersNode")) continue;  /* numbered params handled elsewhere */
    int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
    if (rn == 0) continue;
    int body = nt_ref(nt, L, "body");
    if (body < 0) continue;
    memset(inbody, 0, (size_t)n);
    blkp_mark_subtree(nt, body, inbody);
    for (int i = 0; i < rn; i++) {
      const char *p = nt_str(nt, reqs[i], "name");
      if (!p) continue;
      /* collision: the name is used outside this block's body -- as a local
         write/read or as another block's parameter (param-vs-param, e.g. two
         inject folds sharing `|a, b|` with different element types). Both pollute
         the shared LocalVar's type. */
      int collide = 0;
      for (int w = 0; w < n && !collide; w++) {
        if (inbody[w]) continue;
        const char *wty = nt_type(nt, w);
        int is_param_node = wty && !strcmp(wty, "RequiredParameterNode");
        if (!lv_node_is_write(wty) && !is_param_node) continue;
        /* don't let this block's own parameter nodes count as a collision */
        if (is_param_node) {
          int own = 0;
          for (int q = 0; q < rn; q++) if (reqs[q] == w) { own = 1; break; }
          if (own) continue;
        }
        const char *wn = nt_str(nt, w, "name");
        if (wn && !strcmp(wn, p)) collide = 1;
      }
      if (!collide) continue;
      char oldn[160], newn[176];
      snprintf(oldn, sizeof oldn, "%s", p);   /* copy: nt_set_str frees p's storage */
      snprintf(newn, sizeof newn, "%s__bp%d", oldn, L);
      nt_set_str((NodeTable *)nt, reqs[i], "name", newn);
      blkp_rewrite_refs(c, body, oldn, newn);
    }
  }
  free(inbody);
}

void analyze_program(Compiler *c) {
  /* scope 0 = top level */
  Scope *top = comp_scope_new(c, NULL, -1);
  top->body = nt_ref(c->nt, c->nt->root_id, "statements");

  rename_shadowing_block_params(c);
  qualify_colliding_consts(c);
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
  int ivar_backstop_changed = 0;
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cl = &c->classes[ci];
    for (int iv = 0; iv < cl->nivars; iv++)
      if (cl->ivar_types[iv] == TY_UNKNOWN) { cl->ivar_types[iv] = TY_POLY; ivar_backstop_changed = 1; }
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
      ivar_backstop_changed = 1;
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
     types always win; the mini-loop below propagates the new types.
     Also re-runs after the ivar backstops above widened an UNKNOWN slot
     to poly: reads of such an ivar inferred UNKNOWN during the fixpoint,
     leaving params bound from them untyped (and the method dropped,
     turning its call sites into undefined references). */
  if (infer_hash_params(c) | infer_array_params(c) | infer_params_from_ivar_hash_ops(c) |
      ivar_backstop_changed) {
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

  /* Backstop step 1: a method reached only via method(:sym) is invoked through
     the bound Method ABI, which passes mrb_int args -- default its untyped
     params/ret to int rather than dropping it (which would leave it undeclared).
     Done before the drop decision below so the freshly-typed params can
     propagate through poly-dispatch param binding (e.g. a poke dispatch table
     entry typed here flows into `@pads[0].poke(data)` -> Pad#poke). */
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (!sc->reachable || !sc->name) continue;
    int taken = 0;
    for (int id = 0; id < c->nt->count && !taken; id++) {
      const char *nty = nt_type(c->nt, id);
      if (!nty || strcmp(nty, "CallNode")) continue;
      const char *nm = nt_str(c->nt, id, "name");
      if (!nm || strcmp(nm, "method")) continue;
      const char *msym = method_sym_arg(c, id);
      if (msym && !strcmp(msym, sc->name)) taken = 1;
    }
    if (taken) {
      /* The bound-Method ABI is `mrb_int (*)(void *, mrb_int...)`: the dispatch
         site (sp_poly_arr_get_hash / sp_poly_slice) reads the return as mrb_int
         and passes int args. So a method(:sym) target MUST return int and take
         int params -- a poly return (e.g. PPU#peek_2002 returning the poly
         @io_latch) would be misread as a struct through the int cast and yield
         garbage. Pin unknown params to int and a poly/unknown return to int
         (codegen coerces the body's poly return via sp_poly_to_i). */
      for (int i = 0; i < sc->nparams; i++) {
        LocalVar *p = sc->pnames[i] ? scope_local(sc, sc->pnames[i]) : NULL;
        if (p && p->type == TY_UNKNOWN) p->type = TY_INT;
      }
      if (sc->ret == TY_UNKNOWN || sc->ret == TY_POLY) sc->ret = TY_INT;
    }
  }


  /* Propagate ivar types up the inheritance chain: a base-class method runs on
     subclass instances, so an ivar it reads must carry the union of every
     subclass's assignments. Without this, an abstract base whose @x is only set
     to nil/placeholder there sees the wrong type when it calls `@x.foo`, even
     though every concrete subclass assigns @x a real object. Monotonic (unify
     only widens), so iterate to a fixpoint. */
  for (int iter = 0; iter < 16; iter++) {
    int prop_changed = 0;
    for (int k = 0; k < c->nclasses; k++) {
      ClassInfo *kc = &c->classes[k];
      for (int iv = 0; iv < kc->nivars; iv++) {
        TyKind kt = kc->ivar_types[iv];
        if (kt == TY_UNKNOWN) continue;
        const char *ivn = kc->ivars[iv];
        for (int a = kc->parent; a >= 0; a = c->classes[a].parent) {
          int ai = comp_ivar_index(&c->classes[a], ivn);
          if (ai < 0) continue;
          TyKind merged = ty_unify(c->classes[a].ivar_types[ai], kt);
          if (merged != c->classes[a].ivar_types[ai]) {
            c->classes[a].ivar_types[ai] = merged; prop_changed = 1;
          }
        }
      }
    }
    if (!prop_changed) break;
  }

  /* Re-run param binding now that method(:sym) targets are int-typed (step 1
     above) and ivars carry their inheritance-unioned types: a base method
     calling `@x.foo(arg)` on a poly @x (only widened to poly by the up-propagation
     just above) can finally bind foo's params. Without this the bound method
     would stay TY_UNKNOWN and be dropped below, leaving an undefined function at
     the poly-dispatch call site. */
  build_ie_map(c);
  for (int it = 0; it < 8; it++) { if (!infer_param_types(c)) break; }

  /* Backstop step 2: a reachable method that STILL has TY_UNKNOWN params was
     never bound by any typed call site -- mark it unreachable so codegen skips
     it rather than exit(1)ing on the unknown type. */
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    if (!sc->reachable || sc->nparams == 0) continue;
    int has_unknown = 0;
    for (int i = 0; i < sc->nparams; i++) {
      LocalVar *p = sc->pnames[i] ? scope_local(sc, sc->pnames[i]) : NULL;
      if (p && p->type == TY_UNKNOWN) { has_unknown = 1; break; }
    }
    if (has_unknown) sc->reachable = 0;
  }

  /* The method-reference backstop and ivar up-propagation above changed param
     and ivar types after the main fixpoint, so re-run local write-type inference:
     a local like `xfine = 8 - (data & 0x7)` only becomes int once its method's
     `data` param is pinned to int by the backstop. */
  for (int iter = 0; iter < 8; iter++) if (!infer_write_types(c)) break;
  /* The write-type re-run can re-derive a hash/array container type for an
     iteration-bound block param from its element-index usage (e.g. `a[1]=v`),
     clobbering the TY_POLY the block-param pass pinned for a poly-collection
     `.each`. Re-pin block-param types so poly elements stay poly. */
  for (int iter = 0; iter < 8; iter++) if (!infer_block_params(c)) break;
  /* infer_write_types resets non-param locals, undoing the earlier bigint
     loop-variable promotion, so re-apply it. */
  detect_bigint_loop_vars(c);
  propagate_bigint_cascade(c);

  /* A non-parameter local that inference never resolved holds a value of unknown
     static type -- a block param bound to an element of a poly receiver, or a
     local fed by a dynamically-dispatched call (e.g. optcarrot's memory-map
     procs). Declare it boxed (poly) rather than failing codegen; reads then go
     through the tag-dispatching poly paths. Method params are excluded: the
     backstop above already pins or drops those. Set before the node-type cache
     is rebuilt so reads of the local see poly. */
  for (int s = 0; s < c->nscopes; s++)
    for (int i = 0; i < c->scopes[s].nlocals; i++) {
      LocalVar *blv = &c->scopes[s].locals[i];
      if (!blv->is_param && blv->type == TY_UNKNOWN) blv->type = TY_POLY;
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
    int saved2 = an_ie_class_id;
    an_ie_class_id = ty_object_class(rt2);
    for (int k2 = 0; k2 < bn2; k2++) infer_type(c, bb2[k2]);
    an_ie_class_id = saved2;
  }
}

