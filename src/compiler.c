#include "compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int comp_ternary_arms(const NodeTable *nt, int id, int *then_node, int *else_node) {
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "IfNode")) return 0;
  int then_b = nt_ref(nt, id, "statements");
  int sub = nt_ref(nt, id, "subsequent");
  if (then_b < 0 || sub < 0) return 0;
  const char *subty = nt_type(nt, sub);
  if (!subty || !sp_streq(subty, "ElseNode")) return 0;
  int else_stmts = nt_ref(nt, sub, "statements");
  if (else_stmts < 0) return 0;
  int tn = 0, en = 0;
  const int *tb = nt_arr(nt, then_b, "body", &tn);
  const int *eb = nt_arr(nt, else_stmts, "body", &en);
  if (tn != 1 || en != 1 || !tb || !eb) return 0;
  *then_node = tb[0];
  *else_node = eb[0];
  return 1;
}

int comp_nil_chain_bottom(const NodeTable *nt, int v) {
  /* `a = b = ... = nil` writes nil to EVERY target, but a nested write node
     evaluates to its slot's unified type, so the inner slot's other writes
     would leak into the outer target's type. Detect the chain so analyze can
     treat the outer write as a nil write and codegen can give each target its
     own typed nil. Only plain local/ivar writes chain; or-/and-/op-writes
     have their own value semantics. Returns the terminal NilNode, or -1. */
  int depth = 0;
  while (v >= 0 && depth < 64) {
    const char *t = nt_type(nt, v);
    if (!t) return -1;
    if (sp_streq(t, "NilNode")) return depth > 0 ? v : -1;
    if (!sp_streq(t, "LocalVariableWriteNode") &&
        !sp_streq(t, "InstanceVariableWriteNode")) return -1;
    v = nt_ref(nt, v, "value");
    depth++;
  }
  return -1;
}

Compiler *comp_new(const NodeTable *nt) {
  Compiler *c = calloc(1, sizeof(Compiler));
  if (!c) return NULL;
  c->nt = nt;
  int n = nt->count > 0 ? nt->count : 1;
  c->ntype = calloc((size_t)n, sizeof(TyKind));
  c->nilnarrow = calloc((size_t)n, sizeof(TyKind));
  c->nscope = calloc((size_t)n, sizeof(int));   /* default scope 0 */
  c->node_cbody = malloc((size_t)n * sizeof(int));   /* enclosing class-body, -1 = none */
  for (int i = 0; i < n; i++) c->node_cbody[i] = -1;
  c->empty_arr_recv = calloc((size_t)n, 1);
  c->node_cap = n;
  return c;
}

/* Resize the per-node arrays after the node table grew (e.g. an AST subtree
   was cloned). New entries default to TY_UNKNOWN / scope 0. */
void comp_grow_node_arrays(Compiler *c) {
  int n = c->nt->count;
  if (n <= c->node_cap) return;
  c->ntype = realloc(c->ntype, sizeof(TyKind) * (size_t)n);
  c->nilnarrow = realloc(c->nilnarrow, sizeof(TyKind) * (size_t)n);
  c->nscope = realloc(c->nscope, sizeof(int) * (size_t)n);
  c->node_cbody = realloc(c->node_cbody, sizeof(int) * (size_t)n);
  c->empty_arr_recv = realloc(c->empty_arr_recv, (size_t)n);
  for (int i = c->node_cap; i < n; i++) { c->ntype[i] = TY_UNKNOWN; c->nilnarrow[i] = TY_UNKNOWN; c->nscope[i] = 0; c->node_cbody[i] = -1; c->empty_arr_recv[i] = 0; }
  c->node_cap = n;
}

void comp_free(Compiler *c) {
  if (!c) return;
  free(c->blk_body_map);
  c->blk_body_map = NULL;
  for (int s = 0; s < c->nscopes; s++) {
    Scope *sc = &c->scopes[s];
    free(sc->name);
    for (int i = 0; i < sc->nlocals; i++) free(sc->locals[i].name);
    free(sc->locals);
    for (int i = 0; i < sc->nparams; i++) free(sc->pnames[i]);
    free(sc->pnames);
    free(sc->pdefault);
  }
  free(c->scopes);
  for (int i = 0; i < c->nsymbols; i++) free(c->symbols[i]);
  free(c->symbols);
  for (int i = 0; i < c->nclasses; i++) {
    free(c->classes[i].name);
    for (int j = 0; j < c->classes[i].nivars; j++) free(c->classes[i].ivars[j]);
    free(c->classes[i].ivars);
    free(c->classes[i].ivar_types);
    for (int j = 0; j < c->classes[i].n_rbs_pin_ivars; j++) free(c->classes[i].rbs_pin_ivars[j]);
    free(c->classes[i].rbs_pin_ivars);
    for (int j = 0; j < c->classes[i].nreaders; j++) free(c->classes[i].readers[j]);
    free(c->classes[i].readers);
    for (int j = 0; j < c->classes[i].nwriters; j++) free(c->classes[i].writers[j]);
    free(c->classes[i].writers);
    for (int j = 0; j < c->classes[i].nundefs; j++) free(c->classes[i].undefs[j]);
    free(c->classes[i].undefs);
    for (int j = 0; j < c->classes[i].nsg_readers; j++) free(c->classes[i].sg_readers[j]);
    free(c->classes[i].sg_readers);
    for (int j = 0; j < c->classes[i].nsg_writers; j++) free(c->classes[i].sg_writers[j]);
    free(c->classes[i].sg_writers);
    for (int j = 0; j < c->classes[i].nprep_chain; j++) {
      free(c->classes[i].prep_from[j]);
      free(c->classes[i].prep_to[j]);
    }
    free(c->classes[i].prep_from);
    free(c->classes[i].prep_to);
  }
  free(c->classes);
  for (int i = 0; i < c->ngvars; i++) free(c->gvars[i].name);
  free(c->gvars);
  for (int i = 0; i < c->nconsts; i++) free(c->consts[i].name);
  free(c->consts);
  free(c->toplevel_includes);
  free(c->nscope);
  free(c->ntype);
  free(c->node_cbody);
  free(c->empty_arr_recv);
  free(c);
}

static LocalVar *lv_find(LocalVar *arr, int n, const char *name) {
  for (int i = 0; i < n; i++) if (sp_streq(arr[i].name, name)) return &arr[i];
  return NULL;
}
static LocalVar *lv_intern(LocalVar **arr, int *n, int *cap, const char *name) {
  LocalVar *lv = lv_find(*arr, *n, name);
  if (lv) return lv;
  if (*n >= *cap) { *cap = *cap ? *cap * 2 : 8; *arr = realloc(*arr, sizeof(LocalVar) * (size_t)*cap); }
  lv = &(*arr)[(*n)++];
  memset(lv, 0, sizeof(*lv));
  lv->name = strdup(name);
  lv->type = TY_UNKNOWN;
  return lv;
}
LocalVar *comp_gvar(Compiler *c, const char *name) { return lv_find(c->gvars, c->ngvars, name); }
LocalVar *comp_gvar_intern(Compiler *c, const char *name) { return lv_intern(&c->gvars, &c->ngvars, &c->cgvars, name); }
const char *comp_resolve_gvar(Compiler *c, const char *name) {
  for (int i = 0; i < c->ngvar_aliases; i++)
    if (sp_streq(c->gvar_alias_from[i], name)) return c->gvar_alias_to[i];
  return name;
}
void comp_add_gvar_alias(Compiler *c, const char *from, const char *to) {
  for (int i = 0; i < c->ngvar_aliases; i++)
    if (sp_streq(c->gvar_alias_from[i], from)) return; /* already recorded */
  c->gvar_alias_from = realloc(c->gvar_alias_from, sizeof(char*) * (size_t)(c->ngvar_aliases + 1));
  c->gvar_alias_to   = realloc(c->gvar_alias_to,   sizeof(char*) * (size_t)(c->ngvar_aliases + 1));
  c->gvar_alias_from[c->ngvar_aliases] = strdup(from);
  c->gvar_alias_to[c->ngvar_aliases]   = strdup(to);
  c->ngvar_aliases++;
}
LocalVar *comp_const(Compiler *c, const char *name) { return lv_find(c->consts, c->nconsts, name); }
LocalVar *comp_const_intern(Compiler *c, const char *name) { return lv_intern(&c->consts, &c->nconsts, &c->cconsts, name); }

int comp_sym_intern(Compiler *c, const char *name) {
  for (int i = 0; i < c->nsymbols; i++)
    if (sp_streq(c->symbols[i], name)) return i;
  if (c->nsymbols >= c->csymbols) {
    c->csymbols = c->csymbols ? c->csymbols * 2 : 8;
    c->symbols = realloc(c->symbols, sizeof(char *) * (size_t)c->csymbols);
  }
  c->symbols[c->nsymbols] = strdup(name);
  return c->nsymbols++;
}

Scope *comp_scope_new(Compiler *c, const char *name, int def_node) {
  if (c->nscopes >= c->cscopes) {
    c->cscopes = c->cscopes ? c->cscopes * 2 : 8;
    c->scopes = realloc(c->scopes, sizeof(Scope) * (size_t)c->cscopes);
  }
  Scope *s = &c->scopes[c->nscopes++];
  memset(s, 0, sizeof(*s));
  s->name = name ? strdup(name) : NULL;
  s->def_node = def_node;
  s->body = -1;
  s->class_id = -1;
  s->rest_idx = -1;
  s->kwrest_idx = -1;
  s->ret = TY_UNKNOWN;
  s->dm_subst_node = -1;
  return s;
}

/* Runtime typedefs `sp_<X>` a user class name could redefine. A user class whose
   name matches one gets a `u_`-prefixed C stem (`sp_u_<name>`) so its emitted
   struct/typedef/methods never collide with the runtime's own type. (Builtin
   class names like Range/Complex are reopened via the __oc_ path and never emit
   a fresh sp_<name> struct, so listing them here is merely harmless.) */
static int sp_name_collides_runtime(const char *n) {
  /* ONLY runtime-internal type names that are NOT Ruby classes. A builtin class
     (String, Range, Complex, Proc, ...) is reopened via the primitive/__oc_ path
     and never emits a fresh `sp_<name>` struct, so it does not collide -- and it
     must NOT be mangled, or the builtin-name checks (`sp_streq(dcn,"String")`)
     that share the C stem would stop matching. */
  static const char *const reserved[] = {
    "RbVal", "Val", "Bigint",
    "IntArray", "FloatArray", "StrArray", "PolyArray", "PtrArray",
    "IntIntHash", "IntStrHash", "StrIntHash", "StrStrHash", "SymPolyHash",
    "StrPolyHash", "PolyPolyHash", "BoundMethod", "Curry", "ProcCompose",
    "StrBuf", "Argf", "Argv", "condvar", "mutex", "queue", "thread",
    NULL };
  for (int i = 0; reserved[i]; i++) if (sp_streq(n, reserved[i])) return 1;
  return 0;
}
/* The C-identifier stem for a class: the name, disambiguated if it would clash
   with a runtime typedef. Caller owns the returned strdup'd string. */
static char *sp_class_c_name(const char *name) {
  if (!name) return NULL;
  if (!sp_name_collides_runtime(name)) return strdup(name);
  size_t n = strlen(name);
  char *m = malloc(n + 3);
  if (!m) return strdup(name);
  m[0] = 'u'; m[1] = '_'; memcpy(m + 2, name, n + 1);
  return m;
}
ClassInfo *comp_class_new(Compiler *c, const char *name, int def_node) {
  if (c->nclasses >= c->cclasses) {
    c->cclasses = c->cclasses ? c->cclasses * 2 : 8;
    c->classes = realloc(c->classes, sizeof(ClassInfo) * (size_t)c->cclasses);
  }
  ClassInfo *ci = &c->classes[c->nclasses++];
  memset(ci, 0, sizeof(*ci));
  ci->name = name ? strdup(name) : NULL;
  ci->c_name = sp_class_c_name(name);
  ci->def_node = def_node;
  ci->parent = -1;
  ci->enclosing_class = -1;
  return ci;
}

/* name -> class index, frozen index (shares the scope-index freeze signal; see
   comp_scope_index_set_frozen). comp_class_index is a linear scan over classes
   called per node during the fixpoint -- O(lookups * classes). Built descending
   so the chain head is the lowest class index (first definition wins, matching
   the forward scan). While unfrozen (walk_scope / register passes add and rename
   classes with the count unchanged), fall back to the linear scan. */
static int ci_frozen = 0, ci_nclasses = -1, ci_buckets = 0;
static int *ci_next = NULL, *ci_head = NULL;
static unsigned ci_hash(const char *s) {
  unsigned h = 2166136261u;
  for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
  return h;
}
static void ci_build(Compiler *c) {
  int nc = c->nclasses;
  free(ci_next); free(ci_head);
  ci_buckets = nc > 0 ? nc : 1;
  ci_next = malloc((size_t)(nc > 0 ? nc : 1) * sizeof(int));
  ci_head = malloc((size_t)ci_buckets * sizeof(int));
  ci_nclasses = nc;
  if (!ci_next || !ci_head) { ci_buckets = 0; return; }
  for (int i = 0; i < ci_buckets; i++) ci_head[i] = -1;
  for (int i = nc - 1; i >= 0; i--) {
    if (!c->classes[i].name) continue;
    unsigned b = ci_hash(c->classes[i].name) % (unsigned)ci_buckets;
    ci_next[i] = ci_head[b]; ci_head[b] = i;
  }
}
int comp_class_index(Compiler *c, const char *name) {
  if (!name) return -1;
  if (!ci_frozen) {
    for (int i = 0; i < c->nclasses; i++)
      if (c->classes[i].name && sp_streq(c->classes[i].name, name)) return i;
    return -1;
  }
  if (ci_nclasses != c->nclasses) ci_build(c);
  if (!ci_buckets) return -1;
  for (int i = ci_head[ci_hash(name) % (unsigned)ci_buckets]; i >= 0; i = ci_next[i])
    if (c->classes[i].name && sp_streq(c->classes[i].name, name)) return i;
  return -1;
}

int comp_ivar_index(ClassInfo *ci, const char *name) {
  for (int i = 0; i < ci->nivars; i++)
    if (sp_streq(ci->ivars[i], name)) return i;
  return -1;
}

int comp_ivar_intern(ClassInfo *ci, const char *name) {
  int idx = comp_ivar_index(ci, name);
  if (idx >= 0) return idx;
  if (ci->nivars >= ci->civars) {
    ci->civars = ci->civars ? ci->civars * 2 : 8;
    ci->ivars = realloc(ci->ivars, sizeof(char *) * (size_t)ci->civars);
    ci->ivar_types = realloc(ci->ivar_types, sizeof(TyKind) * (size_t)ci->civars);
  }
  ci->ivars[ci->nivars] = strdup(name);
  ci->ivar_types[ci->nivars] = TY_UNKNOWN;
  return ci->nivars++;
}

int comp_cvar_index(ClassInfo *ci, const char *name) {
  for (int i = 0; i < ci->ncvars; i++)
    if (sp_streq(ci->cvars[i], name)) return i;
  return -1;
}

int comp_cvar_intern(ClassInfo *ci, const char *name) {
  int idx = comp_cvar_index(ci, name);
  if (idx >= 0) return idx;
  if (ci->ncvars >= ci->ccvars) {
    ci->ccvars = ci->ccvars ? ci->ccvars * 2 : 8;
    ci->cvars = realloc(ci->cvars, sizeof(char *) * (size_t)ci->ccvars);
    ci->cvar_types = realloc(ci->cvar_types, sizeof(TyKind) * (size_t)ci->ccvars);
  }
  ci->cvars[ci->ncvars] = strdup(name);
  ci->cvar_types[ci->ncvars] = TY_UNKNOWN;
  return ci->ncvars++;
}

/* (class_id, name, is_cmethod) -> scope index, cached per scope count. Both
   lookups below otherwise scan all scopes in reverse, and they are called many
   times per node during the inference fixpoint (O(lookups * scopes)). The chain
   is head-inserted in ascending scope order, so it is in descending order and
   the first matching entry is the highest scope index -- the same "later
   definition wins" semantics as the reverse linear scan. Rebuilt when the scope
   count changes (no scopes are added during the fixpoint). */
static unsigned sm_hash(int class_id, const char *name, int is_cm) {
  unsigned h = 2166136261u;
  for (const char *p = name; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
  h ^= (unsigned)class_id * 2654435761u;
  h ^= (unsigned)(is_cm ? 0x9e3779b9u : 0);
  return h;
}
static int sm_nscopes = -1, sm_buckets = 0, sm_frozen = 0;
static int *sm_next = NULL, *sm_head = NULL;
/* Parallel index for top-level methods (class_id < 0), keyed by name. Built in
   descending scope order so the chain head is the lowest scope index -- matching
   comp_method_index's "first definition wins" forward scan. */
static int *tm_next = NULL, *tm_head = NULL;
/* The index is only safe once scope shape (count + each scope's class_id/name/
   is_cmethod) stops changing: walk_scope and the register_* / prepend passes
   create and *rename* scopes, and a rename leaves the count unchanged, so a
   count-keyed cache would go stale. analyze_program freezes the index just
   before the inference fixpoint (where lookups are hottest and scope shape is
   fixed) and unfreezes at entry. While unfrozen, fall back to the linear scan. */
void comp_scope_index_set_frozen(int f) { sm_frozen = f; sm_nscopes = -1; ci_frozen = f; ci_nclasses = -1; }
static void sm_build(Compiler *c) {
  int ns = c->nscopes;
  free(sm_next); free(sm_head); free(tm_next); free(tm_head);
  sm_buckets = ns > 0 ? ns : 1;
  sm_next = malloc((size_t)(ns > 0 ? ns : 1) * sizeof(int));
  sm_head = malloc((size_t)sm_buckets * sizeof(int));
  tm_next = malloc((size_t)(ns > 0 ? ns : 1) * sizeof(int));
  tm_head = malloc((size_t)sm_buckets * sizeof(int));
  sm_nscopes = ns;
  if (!sm_next || !sm_head || !tm_next || !tm_head) { sm_buckets = 0; return; }
  for (int i = 0; i < sm_buckets; i++) { sm_head[i] = -1; tm_head[i] = -1; }
  for (int s = 0; s < ns; s++) {
    if (!c->scopes[s].name) continue;
    if (c->scopes[s].class_id >= 0) {
      unsigned b = sm_hash(c->scopes[s].class_id, c->scopes[s].name, c->scopes[s].is_cmethod) % (unsigned)sm_buckets;
      sm_next[s] = sm_head[b]; sm_head[b] = s;
    }
  }
  /* top-level methods: descending so the lowest scope index ends at the head */
  for (int s = ns - 1; s >= 0; s--) {
    if (c->scopes[s].class_id >= 0 || !c->scopes[s].name) continue;
    unsigned b = sm_hash(-1, c->scopes[s].name, 0) % (unsigned)sm_buckets;
    tm_next[s] = tm_head[b]; tm_head[b] = s;
  }
}
static int sm_lookup(Compiler *c, int class_id, const char *name, int is_cm) {
  if (!name) return -1;
  if (!sm_frozen) {
    /* scope shape may still change: scan in reverse so a later (reopened /
       transplanted) definition wins, matching the frozen index's ordering. */
    for (int s = c->nscopes - 1; s >= 0; s--)
      if (c->scopes[s].class_id == class_id && (int)c->scopes[s].is_cmethod == is_cm &&
          c->scopes[s].name && sp_streq(c->scopes[s].name, name)) return s;
    return -1;
  }
  if (sm_nscopes != c->nscopes) sm_build(c);
  if (!sm_buckets) return -1;
  unsigned b = sm_hash(class_id, name, is_cm) % (unsigned)sm_buckets;
  for (int s = sm_head[b]; s >= 0; s = sm_next[s])
    if (c->scopes[s].class_id == class_id && (int)c->scopes[s].is_cmethod == is_cm &&
        c->scopes[s].name && sp_streq(c->scopes[s].name, name)) return s;
  return -1;
}

int comp_method_in_class(Compiler *c, int class_id, const char *name) {
  return sm_lookup(c, class_id, name, 0);
}

int comp_cmethod_in_class(Compiler *c, int class_id, const char *name) {
  return sm_lookup(c, class_id, name, 1);
}
int comp_cmethod_in_chain(Compiler *c, int class_id, const char *name, int *def_class) {
  name = comp_resolve_alias(c, class_id, name);
  for (int cid = class_id; cid >= 0; cid = c->classes[cid].parent) {
    int mi = comp_cmethod_in_class(c, cid, name);
    if (mi >= 0) { if (def_class) *def_class = cid; return mi; }
  }
  return -1;
}

int comp_method_in_chain(Compiler *c, int class_id, const char *name, int *def_class) {
  name = comp_resolve_alias(c, class_id, name);
  for (int cid = class_id; cid >= 0; cid = c->classes[cid].parent) {
    int mi = comp_method_in_class(c, cid, name);
    if (mi >= 0) { if (def_class) *def_class = cid; return mi; }
  }
  return -1;
}

void comp_method_vis_set(ClassInfo *ci, const char *name, int kind) {
  if (!name) return;
  for (int i = 0; i < ci->nvis; i++)
    if (sp_streq(ci->vis_names[i], name)) { ci->vis_kinds[i] = kind; return; }
  if (ci->nvis >= ci->cvis) {
    ci->cvis = ci->cvis ? ci->cvis * 2 : 8;
    char **nn = realloc(ci->vis_names, sizeof(char *) * (size_t)ci->cvis);
    int *nk = realloc(ci->vis_kinds, sizeof(int) * (size_t)ci->cvis);
    if (!nn || !nk) { fprintf(stderr, "out of memory\n"); exit(1); }
    ci->vis_names = nn; ci->vis_kinds = nk;
  }
  ci->vis_names[ci->nvis] = strdup(name);
  ci->vis_kinds[ci->nvis] = kind;
  ci->nvis++;
}

int comp_method_vis(ClassInfo *ci, const char *name) {
  if (!name) return SP_VIS_PUBLIC;
  for (int i = 0; i < ci->nvis; i++)
    if (sp_streq(ci->vis_names[i], name)) return ci->vis_kinds[i];
  return SP_VIS_PUBLIC;
}

int comp_method_vis_in_chain(Compiler *c, int class_id, const char *name) {
  name = comp_resolve_alias(c, class_id, name);
  for (int cid = class_id; cid >= 0; cid = c->classes[cid].parent) {
    ClassInfo *ci = &c->classes[cid];
    for (int i = 0; i < ci->nvis; i++)
      if (sp_streq(ci->vis_names[i], name)) return ci->vis_kinds[i];
  }
  return SP_VIS_PUBLIC;
}

/* Detect an instance_eval/exec trampoline method: a method whose body is a
   single `instance_eval(&block)` / `instance_exec(args, &block)` that forwards
   the method's own `&block` parameter. A call `recv.M(args) { ... }` to such a
   method is compiled exactly like `recv.instance_eval/exec(args) { ... }`.
   Returns 1 for an instance_eval trampoline, 2 for instance_exec, 0 otherwise.
   When non-zero and def_class is non-NULL, it receives the defining class. */
int comp_trampoline_kind(Compiler *c, int class_id, const char *name, int *def_class) {
  const NodeTable *nt = c->nt;
  int dc = -1;
  int mi = comp_method_in_chain(c, class_id, name, &dc);
  if (mi < 0) return 0;
  Scope *s = &c->scopes[mi];
  if (!s->blk_param || !s->blk_param[0]) return 0;  /* needs a named &block */
  if (s->body < 0) return 0;
  int bn = 0; const int *bb = nt_arr(nt, s->body, "body", &bn);
  if (bn != 1 || !bb) return 0;
  int call = bb[0];
  const char *ct = nt_type(nt, call);
  if (!ct || !sp_streq(ct, "CallNode")) return 0;
  if (nt_ref(nt, call, "receiver") >= 0) return 0;  /* must be receiverless */
  const char *cn = nt_str(nt, call, "name");
  if (!cn) return 0;
  int kind = sp_streq(cn, "instance_eval") ? 1 : sp_streq(cn, "instance_exec") ? 2 : 0;
  if (!kind) return 0;
  /* The block arg must forward the method's own &block parameter. */
  int barg = nt_ref(nt, call, "block");
  if (barg < 0) return 0;
  int bexpr = nt_ref(nt, barg, "expression");
  if (bexpr < 0) return 0;
  const char *bvn = nt_str(nt, bexpr, "name");
  if (!bvn || !sp_streq(bvn, s->blk_param)) return 0;
  if (def_class) *def_class = dc;
  return kind;
}

/* Stage-1 static fold for a module-level singleton accessor that holds a
   constant: if `Class.base = SomeConst` appears exactly once program-wide and
   every write is the same constant-resolvable class/module, return that
   class's index; otherwise -1 (polymorphic / non-constant -> runtime path). */
/* Collect the distinct constant class indices assigned to `Class.base = Const`
   across the whole program (deduped, in first-seen order). Returns the count, or
   -1 if any write's RHS is not a constant-resolvable class. */
/* Index of constant-setter CallNodes (`name` ends in '=', constant receiver),
   cached per node table. comp_sg_const_candidates is called once per singleton
   accessor read during the inference fixpoint; scanning every node each time
   made it O(reads * nodes * iterations). The structural shape indexed here is
   stable across the pass; per-call filters (base, receiver class, arg) still
   run fresh. */
static const NodeTable *sgc_nt = NULL;
static int *sgc_ids = NULL;
static int sgc_n = 0, sgc_ntc = -1;
int comp_sg_const_candidates(Compiler *c, int class_id, const char *base, int *out, int max) {
  const NodeTable *nt = c->nt;
  const char *cls_name = c->classes[class_id].name;
  size_t blen = strlen(base);
  int count = 0;
  if (sgc_nt != nt || sgc_ntc != nt->count) {
    free(sgc_ids);
    sgc_ids = malloc((size_t)nt->count * sizeof(int));
    sgc_n = 0;
    if (sgc_ids) {
      for (int id = 0; id < nt->count; id++) {
        const char *ty = nt_type(nt, id);
        if (!ty || !sp_streq(ty, "CallNode")) continue;
        const char *nm = nt_str(nt, id, "name");
        if (!nm) continue;
        size_t nl = strlen(nm);
        if (nl == 0 || nm[nl - 1] != '=') continue;
        int recv = nt_ref(nt, id, "receiver");
        if (recv < 0) continue;
        const char *rty = nt_type(nt, recv);
        if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode")))
          sgc_ids[sgc_n++] = id;
      }
    }
    sgc_nt = nt;
    sgc_ntc = nt->count;
  }
  for (int ii = 0; ii < sgc_n; ii++) {
    int id = sgc_ids[ii];
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    size_t nl = strlen(nm);
    if (nl != blen + 1 || nm[nl - 1] != '=' || strncmp(nm, base, blen)) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn || !sp_streq(rn, cls_name)) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an != 1) return -1;
    const char *vty = nt_type(nt, av[0]);
    if (!vty || (!sp_streq(vty, "ConstantReadNode") && !sp_streq(vty, "ConstantPathNode"))) return -1;
    int rci = comp_class_index(c, nt_str(nt, av[0], "name"));
    if (rci < 0) return -1;
    int seen = 0;
    for (int j = 0; j < count; j++) if (out[j] == rci) { seen = 1; break; }
    if (!seen && count < max) out[count++] = rci;
  }
  return count;
}

/* Stage-1 fold: the accessor holds a single distinct constant program-wide. */
int comp_sg_const_binding(Compiler *c, int class_id, const char *base) {
  int cand[32];
  int n = comp_sg_const_candidates(c, class_id, base, cand, 32);
  return n == 1 ? cand[0] : -1;
}

/* If `call_id` reads a module singleton accessor (`Class.reader`) whose value
   folds to a single constant via comp_sg_const_binding, return that constant's
   class index; otherwise -1. */
int comp_sg_reader_const(Compiler *c, int call_id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, call_id);
  if (!ty || !sp_streq(ty, "CallNode")) return -1;
  if (nt_ref(nt, call_id, "block") >= 0) return -1;
  if (nt_ref(nt, call_id, "arguments") >= 0) return -1;
  const char *nm = nt_str(nt, call_id, "name");
  if (!nm) return -1;
  size_t nl = strlen(nm);
  if (nl > 0 && nm[nl - 1] == '=') return -1;
  int recv = nt_ref(nt, call_id, "receiver");
  if (recv < 0) return -1;
  const char *rty = nt_type(nt, recv);
  if (!rty || (!sp_streq(rty, "ConstantReadNode") && !sp_streq(rty, "ConstantPathNode"))) return -1;
  const char *rn = nt_str(nt, recv, "name");
  int ci = rn ? comp_class_index(c, rn) : -1;
  if (ci < 0 || !comp_is_sg_reader(&c->classes[ci], nm)) return -1;
  return comp_sg_const_binding(c, ci, nm);
}

/* If `call_id` reads a module singleton accessor written with 2+ distinct
   constants (Stage-2), fill out[] with those class indices and return the
   count; otherwise 0 (or -1 if a non-constant RHS makes it un-resolvable). */
int comp_sg_reader_candidates(Compiler *c, int call_id, int *out, int max) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, call_id);
  if (!ty || !sp_streq(ty, "CallNode")) return 0;
  if (nt_ref(nt, call_id, "block") >= 0) return 0;
  if (nt_ref(nt, call_id, "arguments") >= 0) return 0;
  const char *nm = nt_str(nt, call_id, "name");
  if (!nm) return 0;
  size_t nl = strlen(nm);
  if (nl > 0 && nm[nl - 1] == '=') return 0;
  int recv = nt_ref(nt, call_id, "receiver");
  if (recv < 0) return 0;
  const char *rty = nt_type(nt, recv);
  if (!rty || (!sp_streq(rty, "ConstantReadNode") && !sp_streq(rty, "ConstantPathNode"))) return 0;
  const char *rn = nt_str(nt, recv, "name");
  int ci = rn ? comp_class_index(c, rn) : -1;
  if (ci < 0 || !comp_is_sg_reader(&c->classes[ci], nm)) return 0;
  return comp_sg_const_candidates(c, ci, nm, out, max);
}

/* A constant name that resolves to something at compile time: a value
   constant, a registered class/module, or a well-known builtin the
   DefinedNode emit answers "constant" for (keep the list in sync with the
   ConstantReadNode arm in codegen_expr.c). */
static int const_name_resolves(Compiler *c, const char *cn) {
  if (!cn) return 0;
  if (comp_const(c, cn) || comp_class_index(c, cn) >= 0) return 1;
  static const char *const builtins[] = {
    "Object", "BasicObject", "Kernel", "Module", "Class", "Array", "Hash",
    "String", "Integer", "Float", "Symbol", "Regexp", "Range", "NilClass",
    "TrueClass", "FalseClass", "Numeric", "Comparable", "Enumerable",
    "IO", "File", "Dir", "Math", "GC", "Process", "ENV", "ARGV",
    "STDOUT", "STDERR", "STDIN", NULL };
  for (int bi = 0; builtins[bi]; bi++) if (sp_streq(cn, builtins[bi])) return 1;
  return 0;
}

/* Statically-false `defined?(Const)` guard: the predicate is a DefinedNode
   (or a `defined?(Const) && ...` AndNode, whose left arm short-circuits the
   whole conjunction to nil) over a constant / constant path that resolves to
   nothing at compile time. A constant path (A::B::C) needs every segment to
   resolve for the path to possibly exist; one unresolved segment makes the
   whole path -- and the guard -- statically false (`RubyVM::YJIT` folds on
   RubyVM, and `MissingParent::String` folds on MissingParent despite its
   builtin tail). Such an if-branch is compile-time dead -- doom's
   `if defined?(RubyVM::YJIT) && RubyVM::YJIT.enabled?` MRI hack -- so call
   reachability must not walk it and codegen must not emit it. Deliberately
   narrow: only constants/constant paths, no dynamic defined? forms. */
int comp_defined_guard_false(Compiler *c, int pred) {
  const NodeTable *nt = c->nt;
  if (pred < 0) return 0;
  const char *pt = nt_type(nt, pred);
  if (!pt) return 0;
  /* `defined?(X) && rest`: falseness of the left arm decides the whole
     predicate (recurses to cover chained `&&`s, which nest leftward). */
  if (sp_streq(pt, "AndNode"))
    return comp_defined_guard_false(c, nt_ref(nt, pred, "left"));
  if (!sp_streq(pt, "DefinedNode")) return 0;
  int v = nt_ref(nt, pred, "value");
  if (v < 0) return 0;
  const char *vt = nt_type(nt, v);
  if (vt && sp_streq(vt, "ConstantReadNode"))
    return !const_name_resolves(c, nt_str(nt, v, "name"));
  if (vt && sp_streq(vt, "ConstantPathNode")) {
    /* Walk tail-to-root; any unresolved segment kills the whole path. */
    for (int seg = v; ; ) {
      if (!const_name_resolves(c, nt_str(nt, seg, "name"))) return 1;
      int par = nt_ref(nt, seg, "parent");
      if (par < 0) return 0;                     /* ::Root form, all resolved */
      const char *pty = nt_type(nt, par);
      if (pty && sp_streq(pty, "ConstantPathNode")) { seg = par; continue; }
      if (pty && sp_streq(pty, "ConstantReadNode"))
        return !const_name_resolves(c, nt_str(nt, par, "name"));
      return 0;                                  /* dynamic parent: no fold */
    }
  }
  return 0;
}

/* A literal ArrayNode whose elements are all integer literals (or empty). */
static int is_int_array_literal(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  if (!ty || !sp_streq(ty, "ArrayNode")) return 0;
  int en = 0; const int *els = nt_arr(nt, node, "elements", &en);
  for (int i = 0; i < en; i++) {
    const char *et = nt_type(nt, els[i]);
    if (!et || !sp_streq(et, "IntegerNode")) return 0;
  }
  return 1;
}

/* A literal nested array `[[..ints..], ...]` -- every element is itself an
   int-array literal. Used to type the inner arrays of a poly-array for fold
   operations (e.g. inject(&:&) set intersection over arrays of int arrays). */
int comp_is_nested_int_array_literal(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  if (!ty || !sp_streq(ty, "ArrayNode")) return 0;
  int en = 0; const int *els = nt_arr(nt, node, "elements", &en);
  if (en == 0) return 0;
  for (int i = 0; i < en; i++)
    if (!is_int_array_literal(c, els[i])) return 0;
  return 1;
}

static int name_in(char **list, int n, const char *name) {
  for (int i = 0; i < n; i++) if (sp_streq(list[i], name)) return 1;
  return 0;
}
void comp_add_reader(ClassInfo *ci, const char *name) {
  if (name_in(ci->readers, ci->nreaders, name)) return;
  if (ci->nreaders >= ci->creaders) {
    ci->creaders = ci->creaders ? ci->creaders * 2 : 4;
    ci->readers = realloc(ci->readers, sizeof(char *) * (size_t)ci->creaders);
  }
  ci->readers[ci->nreaders++] = strdup(name);
}
void comp_add_writer(ClassInfo *ci, const char *name) {
  if (name_in(ci->writers, ci->nwriters, name)) return;
  if (ci->nwriters >= ci->cwriters) {
    ci->cwriters = ci->cwriters ? ci->cwriters * 2 : 4;
    ci->writers = realloc(ci->writers, sizeof(char *) * (size_t)ci->cwriters);
  }
  ci->writers[ci->nwriters++] = strdup(name);
}
int comp_is_reader(ClassInfo *ci, const char *name) { return name_in(ci->readers, ci->nreaders, name); }
int comp_is_writer(ClassInfo *ci, const char *name) { return name_in(ci->writers, ci->nwriters, name); }
void comp_add_undef(ClassInfo *ci, const char *name) {
  if (name_in(ci->undefs, ci->nundefs, name)) return;
  if (ci->nundefs >= ci->cundefs) {
    ci->cundefs = ci->cundefs ? ci->cundefs * 2 : 4;
    ci->undefs = realloc(ci->undefs, sizeof(char *) * (size_t)ci->cundefs);
  }
  ci->undefs[ci->nundefs++] = strdup(name);
}
int comp_is_undeffed_in_chain(Compiler *c, int class_id, const char *name) {
  for (int cid = class_id; cid >= 0; cid = c->classes[cid].parent) {
    if (name_in(c->classes[cid].undefs, c->classes[cid].nundefs, name)) return 1;
    if (comp_method_in_class(c, cid, name) >= 0) return 0;
  }
  return 0;
}
void comp_add_sg_reader(ClassInfo *ci, const char *name) {
  if (name_in(ci->sg_readers, ci->nsg_readers, name)) return;
  if (ci->nsg_readers >= ci->csg_readers) {
    ci->csg_readers = ci->csg_readers ? ci->csg_readers * 2 : 4;
    ci->sg_readers = realloc(ci->sg_readers, sizeof(char *) * (size_t)ci->csg_readers);
  }
  ci->sg_readers[ci->nsg_readers++] = strdup(name);
}
void comp_add_sg_writer(ClassInfo *ci, const char *name) {
  if (name_in(ci->sg_writers, ci->nsg_writers, name)) return;
  if (ci->nsg_writers >= ci->csg_writers) {
    ci->csg_writers = ci->csg_writers ? ci->csg_writers * 2 : 4;
    ci->sg_writers = realloc(ci->sg_writers, sizeof(char *) * (size_t)ci->csg_writers);
  }
  ci->sg_writers[ci->nsg_writers++] = strdup(name);
}
int comp_is_sg_reader(ClassInfo *ci, const char *name) { return name_in(ci->sg_readers, ci->nsg_readers, name); }
int comp_is_sg_writer(ClassInfo *ci, const char *name) { return name_in(ci->sg_writers, ci->nsg_writers, name); }

void comp_add_alias(ClassInfo *ci, const char *new_name, const char *old_name) {
  if (!new_name || !old_name) return;
  for (int i = 0; i < ci->naliases; i++)
    if (sp_streq(ci->alias_new[i], new_name)) return;
  if (ci->naliases >= ci->caliases) {
    ci->caliases = ci->caliases ? ci->caliases * 2 : 4;
    ci->alias_new = realloc(ci->alias_new, sizeof(char *) * (size_t)ci->caliases);
    ci->alias_old = realloc(ci->alias_old, sizeof(char *) * (size_t)ci->caliases);
  }
  ci->alias_new[ci->naliases] = strdup(new_name);
  ci->alias_old[ci->naliases] = strdup(old_name);
  ci->naliases++;
}

const char *comp_resolve_alias(Compiler *c, int class_id, const char *name) {
  if (!name) return name;
  /* Follow alias links (chain-aware), guarding against cycles. */
  for (int hops = 0; hops < 32; hops++) {
    const char *next = NULL;
    for (int cid = class_id; cid >= 0 && !next; cid = c->classes[cid].parent) {
      ClassInfo *ci = &c->classes[cid];
      for (int i = 0; i < ci->naliases; i++)
        if (sp_streq(ci->alias_new[i], name)) { next = ci->alias_old[i]; break; }
    }
    if (!next) return name;
    name = next;
  }
  return name;
}

/* native-binding registry (Path B): a native_func maps a Ruby Module.method to
   a C symbol, with spinel-typed args/return. Lookup and the small type-spec
   vocabulary live here so both analyze and codegen can reach them. */
int comp_native_find(Compiler *c, const char *mod, const char *name) {
  if (!mod || !name) return -1;
  for (int i = 0; i < c->n_native_funcs; i++)
    if (sp_streq(c->native_funcs[i].mod, mod) && sp_streq(c->native_funcs[i].name, name))
      return i;
  return -1;
}

TyKind native_spec_to_ty(const char *spec) {
  if (!spec) return TY_UNKNOWN;
  if (sp_streq(spec, "any"))    return TY_POLY;
  if (sp_streq(spec, "string")) return TY_STRING;
  if (sp_streq(spec, "string?")) return TY_POLY;  /* nullable string -> boxed */
  if (sp_streq(spec, "nstring")) return TY_STRING; /* NULL-able string, unboxed */
  if (sp_streq(spec, "cstring")) return TY_STRING; /* borrowed C string (static buffer): call site dups */
  if (sp_streq(spec, "regexp")) return TY_REGEX;   /* regex-literal arg -> sp_re_pat_<n> */
  if (sp_streq(spec, "int"))    return TY_INT;
  if (sp_streq(spec, "float"))  return TY_FLOAT;
  if (sp_streq(spec, "bool"))   return TY_BOOL;
  if (sp_streq(spec, "nil") || sp_streq(spec, "void")) return TY_NIL;
  return TY_UNKNOWN;
}

/* Find a native method binding on a class: kind 0 = instance method, 1 =
   constructor. Arity-keyed -- prefer an exact nargs==argc match, else the first
   same-name binding. Returns the index in c->native_methods, or -1. */
int comp_native_method_find(Compiler *c, int class_id, const char *name, int argc, int kind) {
  return comp_native_method_find_typed(c, class_id, name, argc, kind, NULL);
}

/* Type-keyed variant: among same-name same-arity bindings, prefer one whose
   arg specs match the call's inferred arg types (putc(65) -> the [:int]
   binding, putc("A") -> [:string]). argtys may be NULL (arity-only). */
int comp_native_method_find_typed(Compiler *c, int class_id, const char *name, int argc, int kind,
                                  const TyKind *argtys) {
  if (class_id < 0 || !name) return -1;
  int arity_match = -1, loose = -1;
  for (int i = 0; i < c->n_native_methods; i++) {
    NativeMethod *m = &c->native_methods[i];
    if (m->class_id != class_id || m->kind != kind || !sp_streq(m->name, name)) continue;
    if (m->nargs == argc) {
      if (argtys) {
        int all = 1;
        for (int a = 0; a < argc; a++) {
          TyKind want = native_spec_to_ty(m->args[a]);
          if (want != TY_POLY && argtys[a] != TY_UNKNOWN && argtys[a] != TY_POLY && argtys[a] != want) { all = 0; break; }
        }
        if (all) return i;
      }
      if (arity_match < 0) arity_match = i;
    }
    if (loose < 0) loose = i;
  }
  return arity_match >= 0 ? arity_match : loose;
}

int comp_reader_in_chain(Compiler *c, int class_id, const char *name, int *def_class) {
  name = comp_resolve_alias(c, class_id, name);
  for (int cid = class_id; cid >= 0; cid = c->classes[cid].parent)
    if (comp_is_reader(&c->classes[cid], name)) { if (def_class) *def_class = cid; return 1; }
  return 0;
}
int comp_writer_in_chain(Compiler *c, int class_id, const char *name, int *def_class) {
  name = comp_resolve_alias(c, class_id, name);
  for (int cid = class_id; cid >= 0; cid = c->classes[cid].parent)
    if (comp_is_writer(&c->classes[cid], name)) { if (def_class) *def_class = cid; return 1; }
  return 0;
}

Scope *comp_scope_of(Compiler *c, int node_id) {
  if (node_id < 0 || node_id >= c->nt->count) return &c->scopes[0];
  int idx = c->nscope[node_id];
  if (idx < 0 || idx >= c->nscopes) idx = 0;
  return &c->scopes[idx];
}

int comp_method_index(Compiler *c, const char *name) {
  if (!name) return -1;
  if (!sm_frozen) {
    for (int s = 0; s < c->nscopes; s++)
      if (c->scopes[s].class_id < 0 && c->scopes[s].name &&
          sp_streq(c->scopes[s].name, name)) return s;
    return -1;
  }
  if (sm_nscopes != c->nscopes) sm_build(c);
  if (!sm_buckets) return -1;
  unsigned b = sm_hash(-1, name, 0) % (unsigned)sm_buckets;
  for (int s = tm_head[b]; s >= 0; s = tm_next[s])
    if (c->scopes[s].class_id < 0 && c->scopes[s].name && sp_streq(c->scopes[s].name, name)) return s;
  return -1;
}

/* Find a method named `name` in any top-level included module.
   module_function methods are class-level (is_cmethod=1), so check both.
   Iterate in reverse so the last include wins (Ruby semantics). */
int comp_included_method_index(Compiler *c, const char *name) {
  if (!name) return -1;
  for (int k = c->ntoplevel_includes - 1; k >= 0; k--) {
    int ci = c->toplevel_includes[k];
    int mi = comp_cmethod_in_chain(c, ci, name, NULL);
    if (mi >= 0) return mi;
    mi = comp_method_in_chain(c, ci, name, NULL);
    if (mi >= 0) return mi;
  }
  return -1;
}

LocalVar *scope_local(Scope *s, const char *name) {
  for (int i = 0; i < s->nlocals; i++)
    if (sp_streq(s->locals[i].name, name)) return &s->locals[i];
  return NULL;
}

LocalVar *scope_local_intern(Scope *s, const char *name) {
  LocalVar *lv = scope_local(s, name);
  if (lv) return lv;
  if (s->nlocals >= s->clocals) {
    s->clocals = s->clocals ? s->clocals * 2 : 8;
    s->locals = realloc(s->locals, sizeof(LocalVar) * (size_t)s->clocals);
  }
  lv = &s->locals[s->nlocals++];
  lv->name = strdup(name);
  lv->type = TY_UNKNOWN;
  lv->gc_root = 0;
  lv->is_param = 0;
  lv->is_block_param = 0;
  lv->proc_ret = TY_UNKNOWN;
  lv->is_cell = 0;
  lv->byref_out = 0;
  lv->init_guarded = 0;
  lv->rbs_seeded = 0;
  return lv;
}

void comp_prep_chain_add(ClassInfo *ci, const char *from, const char *to) {
  if (ci->nprep_chain >= ci->cprep_chain) {
    ci->cprep_chain = ci->cprep_chain ? ci->cprep_chain * 2 : 4;
    ci->prep_from = realloc(ci->prep_from, sizeof(char *) * (size_t)ci->cprep_chain);
    ci->prep_to   = realloc(ci->prep_to,   sizeof(char *) * (size_t)ci->cprep_chain);
  }
  ci->prep_from[ci->nprep_chain] = strdup(from);
  ci->prep_to[ci->nprep_chain]   = strdup(to);
  ci->nprep_chain++;
}

const char *comp_prep_chain_target(Compiler *c, int class_id, const char *name) {
  if (class_id < 0 || class_id >= c->nclasses || !name) return NULL;
  ClassInfo *ci = &c->classes[class_id];
  for (int k = 0; k < ci->nprep_chain; k++)
    if (sp_streq(ci->prep_from[k], name)) return ci->prep_to[k];
  return NULL;
}

const char *comp_prep_user_name(const char *name) {
  if (!name || strncmp(name, "__prep_", 7) != 0) return name;
  const char *p = name + 7;
  while (*p >= '0' && *p <= '9') p++;
  return (*p == '_') ? p + 1 : name;
}
