/* Shared compiler state for the C Spinel compiler.
 *
 * In the single-binary design the analyzer and code generator share one
 * in-memory state object instead of serializing an IR file. This struct
 * is that object; its field set corresponds to the legacy .ir dump. It
 * grows milestone by milestone -- M2 adds method scopes (one Scope per
 * `def`, plus the top-level scope) on top of M1's node type cache.
 */
#ifndef SPINEL_COMPILER_H
#define SPINEL_COMPILER_H

#include "node_table.h"
#include "types.h"

typedef struct {
  char *name;       /* Ruby local name (without sigil) */
  TyKind type;      /* inferred type */
  int gc_root;      /* needs SP_GC_ROOT (heap-managed: strings, ...) */
  int is_param;     /* declared as a method parameter */
} LocalVar;

typedef struct {
  char *name;       /* method name; NULL for the top-level scope */
  int def_node;     /* DefNode id; -1 for top-level */
  int body;         /* StatementsNode id (-1 if empty) */
  int class_id;     /* owning class index, or -1 for free functions */

  char **pnames;    /* parameter names, in order */
  int nparams;

  TyKind ret;       /* inferred return type */

  LocalVar *locals; /* params + body locals */
  int nlocals, clocals;
} Scope;

typedef struct {
  char *name;          /* class name ("Point") */
  int def_node;        /* ClassNode id */
  char **ivars;        /* instance variable names, incl. leading '@' */
  TyKind *ivar_types;
  int nivars, civars;
} ClassInfo;

typedef struct {
  const NodeTable *nt;
  TyKind *ntype;    /* [nt->count] node id -> inferred type */
  int *nscope;      /* [nt->count] node id -> owning scope index */

  Scope *scopes;    /* scope[0] = top level */
  int nscopes, cscopes;

  char **symbols;   /* interned symbol names; index = sp_sym id */
  int nsymbols, csymbols;

  ClassInfo *classes;
  int nclasses, cclasses;
} Compiler;

Compiler *comp_new(const NodeTable *nt);
void comp_free(Compiler *c);

/* Scopes. */
Scope *comp_scope_new(Compiler *c, const char *name, int def_node);
Scope *comp_scope_of(Compiler *c, int node_id);        /* owning scope */
int    comp_method_index(Compiler *c, const char *name); /* -1 if none */

/* Locals within a scope. */
LocalVar *scope_local(Scope *s, const char *name);
LocalVar *scope_local_intern(Scope *s, const char *name);

/* Symbol intern table. comp_sym_intern returns the symbol's id. */
int comp_sym_intern(Compiler *c, const char *name);

/* Classes. */
ClassInfo *comp_class_new(Compiler *c, const char *name, int def_node);
int        comp_class_index(Compiler *c, const char *name);   /* -1 if none */
int        comp_ivar_index(ClassInfo *ci, const char *name);  /* -1 if none */
int        comp_ivar_intern(ClassInfo *ci, const char *name); /* find or add; returns index */
/* Find the method scope index for class_id + method name, or -1. */
int        comp_method_in_class(Compiler *c, int class_id, const char *name);

/* Node type cache. */
static inline TyKind comp_ntype(const Compiler *c, int id) {
  if (id < 0 || id >= c->nt->count) return TY_UNKNOWN;
  return c->ntype[id];
}

#endif
