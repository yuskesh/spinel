/* Shared compiler state for the C Spinel compiler.
 *
 * In the single-binary design the analyzer and code generator share one
 * in-memory state object instead of serializing an IR file. This struct
 * is that object; its field set corresponds to the legacy .ir dump
 * (node-type cache, local/method/class tables, capability flags). It
 * grows milestone by milestone -- for M1 it holds the per-node inferred
 * type cache and the top-level local variable table.
 */
#ifndef SPINEL_COMPILER_H
#define SPINEL_COMPILER_H

#include "node_table.h"
#include "types.h"

typedef struct {
  char *name;       /* Ruby local name (without sigil) */
  TyKind type;      /* inferred type */
  int gc_root;      /* needs SP_GC_ROOT (heap-managed: strings, ...) */
} LocalVar;

typedef struct {
  const NodeTable *nt;
  TyKind *ntype;         /* [nt->count] node id -> inferred type */
  LocalVar *locals;      /* top-level locals */
  int nlocals, clocals;
} Compiler;

Compiler *comp_new(const NodeTable *nt);
void comp_free(Compiler *c);

/* Local variable table. comp_local returns NULL if absent. */
LocalVar *comp_local(Compiler *c, const char *name);
LocalVar *comp_local_intern(Compiler *c, const char *name); /* find or create */

/* Node type cache. */
static inline TyKind comp_ntype(const Compiler *c, int id) {
  if (id < 0 || id >= c->nt->count) return TY_UNKNOWN;
  return c->ntype[id];
}

#endif
