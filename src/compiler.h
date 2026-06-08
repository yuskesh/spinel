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
  int gc_root;      /* scratch during analysis; rooting is type-derived in codegen */
  int is_param;     /* declared as a method parameter (C function param) */
  int is_block_param; /* bound by a block; typed by block-param inference */
  int proc_ret;     /* when type==TY_PROC: the proc's body return type (TyKind),
                       TY_UNKNOWN if not statically known */
  int is_cell;      /* captured by an escaping proc: lives in a heap cell
                       (mrb_int *_cell_<name>) so the closure and the enclosing
                       scope share mutable storage */
} LocalVar;

typedef struct {
  char *name;       /* method name; NULL for the top-level scope */
  int def_node;     /* DefNode id; -1 for top-level */
  int body;         /* StatementsNode id (-1 if empty) */
  int class_id;     /* owning class index, or -1 for free functions */
  int yields;       /* body contains a YieldNode (inlined at call sites) */
  int reachable;    /* method name is referenced somewhere (else dead code) */
  int is_cmethod;   /* `def self.foo`: a class (singleton) method, no instance self */
  char *blk_param;  /* name of the `&block` parameter, or NULL (anon -> "") */

  char **pnames;    /* parameter names, in order (requireds then optionals) */
  int *pdefault;    /* per-param default-value node id, or -1 if required */
  int nparams;
  int nrequired;    /* count of leading required params */

  TyKind ret;       /* inferred return type */
  int ret_proc_ret; /* when ret==TY_PROC: the returned proc's body return type
                       (TyKind), so a caller's `m.call` knows the result type */

  LocalVar *locals; /* params + body locals */
  int nlocals, clocals;
} Scope;

typedef struct {
  char *name;          /* class name ("Point") */
  int def_node;        /* ClassNode id */
  int parent;          /* superclass index, or -1 */
  char **ivars;        /* instance variable names, incl. leading '@' */
  TyKind *ivar_types;
  int nivars, civars;
  char **readers;      /* attr reader method names (no '@') */
  int nreaders, creaders;
  char **writers;      /* attr writer base names (no '@', no '=') */
  int nwriters, cwriters;
  char **alias_new;    /* `alias new old`: alias_new[i] redirects to alias_old[i] */
  char **alias_old;
  int naliases, caliases;
  int is_struct;       /* defined via Struct.new(:a, :b): readers[] are the
                          positional members; the constructor takes them in
                          order and there is no user `initialize`. */
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

  LocalVar *gvars;    /* global variables ($g), name without '$' */
  int ngvars, cgvars;
  LocalVar *consts;   /* top-level constants (FOO) */
  int nconsts, cconsts;
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

/* Global variables and top-level constants. *_intern finds or creates. */
LocalVar *comp_gvar(Compiler *c, const char *name);
LocalVar *comp_gvar_intern(Compiler *c, const char *name);
LocalVar *comp_const(Compiler *c, const char *name);
LocalVar *comp_const_intern(Compiler *c, const char *name);

/* Classes. */
ClassInfo *comp_class_new(Compiler *c, const char *name, int def_node);
int        comp_class_index(Compiler *c, const char *name);   /* -1 if none */
int        comp_ivar_index(ClassInfo *ci, const char *name);  /* -1 if none */
int        comp_ivar_intern(ClassInfo *ci, const char *name); /* find or add; returns index */
/* Find the instance-method scope index for class_id + method name, or -1. */
int        comp_method_in_class(Compiler *c, int class_id, const char *name);
/* Find the class (singleton) method scope, walking the superclass chain. */
int        comp_cmethod_in_chain(Compiler *c, int class_id, const char *name, int *def_class);
/* Like comp_method_in_class but walks the superclass chain. On success,
   *def_class (if non-NULL) is set to the class that defines the method. */
int        comp_method_in_chain(Compiler *c, int class_id, const char *name, int *def_class);
/* Walk the chain for an attr reader/writer; returns 1 and the owning class. */
int        comp_reader_in_chain(Compiler *c, int class_id, const char *name, int *def_class);
int        comp_writer_in_chain(Compiler *c, int class_id, const char *name, int *def_class);
void       comp_add_reader(ClassInfo *ci, const char *name);
void       comp_add_writer(ClassInfo *ci, const char *name);
int        comp_is_reader(ClassInfo *ci, const char *name);
int        comp_is_writer(ClassInfo *ci, const char *name);
void       comp_add_alias(ClassInfo *ci, const char *new_name, const char *old_name);
/* Resolve `name` through the class's (chain-aware) alias table to the
   underlying method/attr name. Returns `name` unchanged if not aliased. */
const char *comp_resolve_alias(Compiler *c, int class_id, const char *name);

/* Node type cache. */
static inline TyKind comp_ntype(const Compiler *c, int id) {
  if (id < 0 || id >= c->nt->count) return TY_UNKNOWN;
  return c->ntype[id];
}

#endif
