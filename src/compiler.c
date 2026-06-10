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
  return c;
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
    for (int j = 0; j < c->classes[i].nreaders; j++) free(c->classes[i].readers[j]);
    free(c->classes[i].readers);
    for (int j = 0; j < c->classes[i].nwriters; j++) free(c->classes[i].writers[j]);
    free(c->classes[i].writers);
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
  s->ret = TY_UNKNOWN;
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

static int comp_cmethod_in_class(Compiler *c, int class_id, const char *name) {
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
  return lv;
}
