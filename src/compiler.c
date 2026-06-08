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
  free(c->nscope);
  free(c->ntype);
  free(c);
}

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

int comp_method_in_class(Compiler *c, int class_id, const char *name) {
  if (!name) return -1;
  for (int s = 0; s < c->nscopes; s++)
    if (c->scopes[s].class_id == class_id && c->scopes[s].name &&
        strcmp(c->scopes[s].name, name) == 0) return s;
  return -1;
}

int comp_method_in_chain(Compiler *c, int class_id, const char *name, int *def_class) {
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

int comp_reader_in_chain(Compiler *c, int class_id, const char *name, int *def_class) {
  for (int cid = class_id; cid >= 0; cid = c->classes[cid].parent)
    if (comp_is_reader(&c->classes[cid], name)) { if (def_class) *def_class = cid; return 1; }
  return 0;
}
int comp_writer_in_chain(Compiler *c, int class_id, const char *name, int *def_class) {
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
  return lv;
}
