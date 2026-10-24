/* M1 on this pin: the pristine snapshot of the parsed table.
 *
 * spinel-mlir's M1 loaded the parser's text twice, one table for codegen and
 * one kept unanalyzed. This pin parses straight into a NodeTable, so the
 * snapshot is a deep copy taken right after parsing, before analysis edits
 * the table. It owns exactly what nt_free frees. Every array is zeroed before
 * it is filled, so a copy that fails part way is released by nt_free too.
 * Used only under --emit-mlir; without the flag nothing here runs. */
static char *mlir_snap_bytes(const char *p, size_t n) {
  char *q = malloc(n + 1);
  if (q) { memcpy(q, p, n); q[n] = 0; }
  return q;
}
#define MLIR_SNAP(dst, src) ((src) == NULL || ((dst) = mlir_snap_bytes((src), strlen(src))) != NULL)
#define MLIR_SNAP_FIELDS(d, s, f, n, c) \
  ((s)->n == 0 || ((d)->f = calloc((size_t)(s)->n, sizeof *(s)->f)) != NULL ? ((d)->n = (d)->c = (s)->n, 1) : 0)

static NodeTable *sp_emit_mlir_snapshot(const NodeTable *nt) {
  if (!nt) return NULL;
  NodeTable *t = calloc(1, sizeof *t);
  if (!t) return NULL;
  t->root_id = nt->root_id;
  t->has_cext_decls = nt->has_cext_decls;
  t->version = nt->version;
  t->edits = nt->edits;
  if (nt->count > 0 && !(t->nodes = calloc((size_t)nt->count, sizeof *t->nodes))) goto fail;
  t->count = t->node_cap = nt->count;
  for (int k = 0; k < nt->count; k++) {
    const SpNode *s = &nt->nodes[k];
    SpNode *d = &t->nodes[k];
    d->kind = s->kind;
    if (!MLIR_SNAP(d->type, s->type) || !MLIR_SNAP(d->content, s->content)) goto fail;
    if (!MLIR_SNAP_FIELDS(d, s, s, ns, cs) || !MLIR_SNAP_FIELDS(d, s, i, ni, ci) ||
        !MLIR_SNAP_FIELDS(d, s, r, nr, cr) || !MLIR_SNAP_FIELDS(d, s, a, na, ca)) goto fail;
    for (int j = 0; j < s->ns; j++) {
      d->s[j].disc = s->s[j].disc;
      d->s[j].val_len = s->s[j].val_len;
      if (!MLIR_SNAP(d->s[j].key, s->s[j].key)) goto fail;
      if (s->s[j].val && !(d->s[j].val = mlir_snap_bytes(s->s[j].val, s->s[j].val_len))) goto fail;
    }
    for (int j = 0; j < s->ni; j++) {
      d->i[j].disc = s->i[j].disc;
      d->i[j].val = s->i[j].val;
      if (!MLIR_SNAP(d->i[j].key, s->i[j].key)) goto fail;
    }
    for (int j = 0; j < s->nr; j++) {
      d->r[j].disc = s->r[j].disc;
      d->r[j].ref = s->r[j].ref;
      if (!MLIR_SNAP(d->r[j].key, s->r[j].key)) goto fail;
    }
    for (int j = 0; j < s->na; j++) {
      d->a[j].disc = s->a[j].disc;
      if (!MLIR_SNAP(d->a[j].key, s->a[j].key)) goto fail;
      if (s->a[j].n > 0) {
        if (!(d->a[j].ids = malloc(sizeof(int) * (size_t)s->a[j].n))) goto fail;
        memcpy(d->a[j].ids, s->a[j].ids, sizeof(int) * (size_t)s->a[j].n);
      }
      d->a[j].n = s->a[j].n;
    }
  }
  if (!MLIR_SNAP(t->source_file, nt->source_file)) goto fail;
  if (nt->nfiles > 0 && !(t->files = calloc((size_t)nt->nfiles, sizeof *t->files))) goto fail;
  t->nfiles = nt->nfiles;
  for (int k = 0; k < nt->nfiles; k++)
    if (!MLIR_SNAP(t->files[k], nt->files[k])) goto fail;
  return t;
fail:
  nt_free(t);
  return NULL;
}
#undef MLIR_SNAP
#undef MLIR_SNAP_FIELDS
