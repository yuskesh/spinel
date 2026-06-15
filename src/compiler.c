#include "compiler.h"

#include <stdlib.h>
#include <string.h>

Compiler *comp_new(const NodeTable *nt) {
  Compiler *c = calloc(1, sizeof(Compiler));
  if (!c) return NULL;
  c->nt = nt;
  int n = nt->count > 0 ? nt->count : 1;
  c->ntype = calloc((size_t)n, sizeof(TyKind));
  c->nscope = calloc((size_t)n, sizeof(int));   /* default scope 0 */
  c->node_cbody = malloc((size_t)n * sizeof(int));   /* enclosing class-body, -1 = none */
  for (int i = 0; i < n; i++) c->node_cbody[i] = -1;
  c->node_cap = n;
  return c;
}

/* Resize the per-node arrays after the node table grew (e.g. an AST subtree
   was cloned). New entries default to TY_UNKNOWN / scope 0. */
void comp_grow_node_arrays(Compiler *c) {
  int n = c->nt->count;
  if (n <= c->node_cap) return;
  c->ntype = realloc(c->ntype, sizeof(TyKind) * (size_t)n);
  c->nscope = realloc(c->nscope, sizeof(int) * (size_t)n);
  c->node_cbody = realloc(c->node_cbody, sizeof(int) * (size_t)n);
  for (int i = c->node_cap; i < n; i++) { c->ntype[i] = TY_UNKNOWN; c->nscope[i] = 0; c->node_cbody[i] = -1; }
  c->node_cap = n;
}

void comp_free(Compiler *c) {
  if (!c) return;
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
  free(c);
}

static LocalVar *lv_find(LocalVar *arr, int n, const char *name) {
  for (int i = 0; i < n; i++) if (strcmp(arr[i].name, name) == 0) return &arr[i];
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
    if (strcmp(c->gvar_alias_from[i], name) == 0) return c->gvar_alias_to[i];
  return name;
}
void comp_add_gvar_alias(Compiler *c, const char *from, const char *to) {
  for (int i = 0; i < c->ngvar_aliases; i++)
    if (strcmp(c->gvar_alias_from[i], from) == 0) return; /* already recorded */
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
    if (strcmp(c->symbols[i], name) == 0) return i;
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

ClassInfo *comp_class_new(Compiler *c, const char *name, int def_node) {
  if (c->nclasses >= c->cclasses) {
    c->cclasses = c->cclasses ? c->cclasses * 2 : 8;
    c->classes = realloc(c->classes, sizeof(ClassInfo) * (size_t)c->cclasses);
  }
  ClassInfo *ci = &c->classes[c->nclasses++];
  memset(ci, 0, sizeof(*ci));
  ci->name = name ? strdup(name) : NULL;
  ci->def_node = def_node;
  ci->parent = -1;
  ci->enclosing_class = -1;
  return ci;
}

int comp_class_index(Compiler *c, const char *name) {
  if (!name) return -1;
  for (int i = 0; i < c->nclasses; i++)
    if (c->classes[i].name && strcmp(c->classes[i].name, name) == 0) return i;
  return -1;
}

int comp_ivar_index(ClassInfo *ci, const char *name) {
  for (int i = 0; i < ci->nivars; i++)
    if (strcmp(ci->ivars[i], name) == 0) return i;
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
    if (strcmp(ci->cvars[i], name) == 0) return i;
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

int comp_method_in_class(Compiler *c, int class_id, const char *name) {
  if (!name) return -1;
  /* iterate in reverse so a reopened class's later definition wins */
  for (int s = c->nscopes - 1; s >= 0; s--)
    if (c->scopes[s].class_id == class_id && !c->scopes[s].is_cmethod &&
        c->scopes[s].name && strcmp(c->scopes[s].name, name) == 0) return s;
  return -1;
}

int comp_cmethod_in_class(Compiler *c, int class_id, const char *name) {
  if (!name) return -1;
  for (int s = c->nscopes - 1; s >= 0; s--)
    if (c->scopes[s].class_id == class_id && c->scopes[s].is_cmethod &&
        c->scopes[s].name && strcmp(c->scopes[s].name, name) == 0) return s;
  return -1;
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
  if (!ct || strcmp(ct, "CallNode")) return 0;
  if (nt_ref(nt, call, "receiver") >= 0) return 0;  /* must be receiverless */
  const char *cn = nt_str(nt, call, "name");
  if (!cn) return 0;
  int kind = !strcmp(cn, "instance_eval") ? 1 : !strcmp(cn, "instance_exec") ? 2 : 0;
  if (!kind) return 0;
  /* The block arg must forward the method's own &block parameter. */
  int barg = nt_ref(nt, call, "block");
  if (barg < 0) return 0;
  int bexpr = nt_ref(nt, barg, "expression");
  if (bexpr < 0) return 0;
  const char *bvn = nt_str(nt, bexpr, "name");
  if (!bvn || strcmp(bvn, s->blk_param)) return 0;
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
int comp_sg_const_candidates(Compiler *c, int class_id, const char *base, int *out, int max) {
  const NodeTable *nt = c->nt;
  const char *cls_name = c->classes[class_id].name;
  size_t blen = strlen(base);
  int count = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || strcmp(ty, "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    size_t nl = strlen(nm);
    if (nl != blen + 1 || nm[nl - 1] != '=' || strncmp(nm, base, blen)) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) continue;
    const char *rty = nt_type(nt, recv);
    if (!rty || (strcmp(rty, "ConstantReadNode") && strcmp(rty, "ConstantPathNode"))) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn || strcmp(rn, cls_name)) continue;
    int args = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (an != 1) return -1;
    const char *vty = nt_type(nt, av[0]);
    if (!vty || (strcmp(vty, "ConstantReadNode") && strcmp(vty, "ConstantPathNode"))) return -1;
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
  if (!ty || strcmp(ty, "CallNode")) return -1;
  if (nt_ref(nt, call_id, "block") >= 0) return -1;
  if (nt_ref(nt, call_id, "arguments") >= 0) return -1;
  const char *nm = nt_str(nt, call_id, "name");
  if (!nm) return -1;
  size_t nl = strlen(nm);
  if (nl > 0 && nm[nl - 1] == '=') return -1;
  int recv = nt_ref(nt, call_id, "receiver");
  if (recv < 0) return -1;
  const char *rty = nt_type(nt, recv);
  if (!rty || (strcmp(rty, "ConstantReadNode") && strcmp(rty, "ConstantPathNode"))) return -1;
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
  if (!ty || strcmp(ty, "CallNode")) return 0;
  if (nt_ref(nt, call_id, "block") >= 0) return 0;
  if (nt_ref(nt, call_id, "arguments") >= 0) return 0;
  const char *nm = nt_str(nt, call_id, "name");
  if (!nm) return 0;
  size_t nl = strlen(nm);
  if (nl > 0 && nm[nl - 1] == '=') return 0;
  int recv = nt_ref(nt, call_id, "receiver");
  if (recv < 0) return 0;
  const char *rty = nt_type(nt, recv);
  if (!rty || (strcmp(rty, "ConstantReadNode") && strcmp(rty, "ConstantPathNode"))) return 0;
  const char *rn = nt_str(nt, recv, "name");
  int ci = rn ? comp_class_index(c, rn) : -1;
  if (ci < 0 || !comp_is_sg_reader(&c->classes[ci], nm)) return 0;
  return comp_sg_const_candidates(c, ci, nm, out, max);
}

/* A literal ArrayNode whose elements are all integer literals (or empty). */
static int is_int_array_literal(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  if (!ty || strcmp(ty, "ArrayNode")) return 0;
  int en = 0; const int *els = nt_arr(nt, node, "elements", &en);
  for (int i = 0; i < en; i++) {
    const char *et = nt_type(nt, els[i]);
    if (!et || strcmp(et, "IntegerNode")) return 0;
  }
  return 1;
}

/* A literal nested array `[[..ints..], ...]` -- every element is itself an
   int-array literal. Used to type the inner arrays of a poly-array for fold
   operations (e.g. inject(&:&) set intersection over arrays of int arrays). */
int comp_is_nested_int_array_literal(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  if (!ty || strcmp(ty, "ArrayNode")) return 0;
  int en = 0; const int *els = nt_arr(nt, node, "elements", &en);
  if (en == 0) return 0;
  for (int i = 0; i < en; i++)
    if (!is_int_array_literal(c, els[i])) return 0;
  return 1;
}

static int name_in(char **list, int n, const char *name) {
  for (int i = 0; i < n; i++) if (strcmp(list[i], name) == 0) return 1;
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
    if (strcmp(ci->alias_new[i], new_name) == 0) return;
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
        if (strcmp(ci->alias_new[i], name) == 0) { next = ci->alias_old[i]; break; }
    }
    if (!next) return name;
    name = next;
  }
  return name;
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
  for (int s = 0; s < c->nscopes; s++)
    if (c->scopes[s].class_id < 0 && c->scopes[s].name &&
        strcmp(c->scopes[s].name, name) == 0) return s;
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
    if (strcmp(s->locals[i].name, name) == 0) return &s->locals[i];
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
    if (!strcmp(ci->prep_from[k], name)) return ci->prep_to[k];
  return NULL;
}

const char *comp_prep_user_name(const char *name) {
  if (!name || strncmp(name, "__prep_", 7) != 0) return name;
  const char *p = name + 7;
  while (*p >= '0' && *p <= '9') p++;
  return (*p == '_') ? p + 1 : name;
}
