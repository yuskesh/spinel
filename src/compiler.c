#include "compiler.h"

#include <stdlib.h>
#include <string.h>

Compiler *comp_new(const NodeTable *nt) {
  Compiler *c = calloc(1, sizeof(Compiler));
  if (!c) return NULL;
  c->nt = nt;
  c->ntype = calloc((size_t)(nt->count > 0 ? nt->count : 1), sizeof(TyKind));
  return c;
}

void comp_free(Compiler *c) {
  if (!c) return;
  for (int i = 0; i < c->nlocals; i++) free(c->locals[i].name);
  free(c->locals);
  free(c->ntype);
  free(c);
}

LocalVar *comp_local(Compiler *c, const char *name) {
  for (int i = 0; i < c->nlocals; i++)
    if (strcmp(c->locals[i].name, name) == 0) return &c->locals[i];
  return NULL;
}

LocalVar *comp_local_intern(Compiler *c, const char *name) {
  LocalVar *lv = comp_local(c, name);
  if (lv) return lv;
  if (c->nlocals >= c->clocals) {
    c->clocals = c->clocals ? c->clocals * 2 : 8;
    c->locals = realloc(c->locals, sizeof(LocalVar) * (size_t)c->clocals);
  }
  lv = &c->locals[c->nlocals++];
  lv->name = strdup(name);
  lv->type = TY_UNKNOWN;
  lv->gc_root = 0;
  return lv;
}
