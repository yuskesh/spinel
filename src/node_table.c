#include "node_table.h"

#include <stdlib.h>
#include <string.h>

/* ---- string unescape (mirrors node_table_loader.rb#unescape_str) ---- */

static char *unescape_dup(const char *s, size_t len) {
  char *out = malloc(len + 1);
  if (!out) return NULL;
  size_t o = 0;
  size_t i = 0;
  while (i < len) {
    char ch = s[i];
    if (ch == '%' && i + 2 < len) {
      char a = s[i + 1], b = s[i + 2];
      char dec = 0;
      int known = 1;
      if (a == '0' && b == 'A') dec = '\n';
      else if (a == '0' && b == 'D') dec = '\r';
      else if (a == '0' && b == '9') dec = '\t';
      else if (a == '2' && b == '0') dec = ' ';
      else if (a == '2' && b == '5') dec = '%';
      else if (a == '0' && b == '0') dec = '\0';
      else known = 0;
      if (known) {
        out[o++] = dec;
        i += 3;
        continue;
      }
      /* unknown escape: keep '%' literally and advance one */
      out[o++] = ch;
      i++;
    }
    else {
      out[o++] = ch;
      i++;
    }
  }
  out[o] = '\0';
  return out;
}

/* ---- field append helpers ---- */

static void ensure_cap(void **buf, int *cap, int need, size_t elem) {
  if (need <= *cap) return;
  int nc = *cap ? *cap * 2 : 4;
  if (nc < need) nc = need;
  *buf = realloc(*buf, (size_t)nc * elem);
  *cap = nc;
}

static char *dup_n(const char *s, size_t n) {
  char *p = malloc(n + 1);
  if (!p) return NULL;
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

static void node_set_type(SpNode *nd, const char *type, size_t len) {
  free(nd->type);
  nd->type = dup_n(type, len);
}

static void node_add_str(SpNode *nd, const char *key, size_t klen, char *val /*owned*/) {
  ensure_cap((void **)&nd->s, &nd->cs, nd->ns + 1, sizeof(SpStrField));
  nd->s[nd->ns].key = dup_n(key, klen);
  nd->s[nd->ns].val = val;
  nd->ns++;
}

static void node_add_int(SpNode *nd, const char *key, size_t klen, long long v) {
  ensure_cap((void **)&nd->i, &nd->ci, nd->ni + 1, sizeof(SpIntField));
  nd->i[nd->ni].key = dup_n(key, klen);
  nd->i[nd->ni].val = v;
  nd->ni++;
}

static void node_add_ref(SpNode *nd, const char *key, size_t klen, int ref) {
  ensure_cap((void **)&nd->r, &nd->cr, nd->nr + 1, sizeof(SpRefField));
  nd->r[nd->nr].key = dup_n(key, klen);
  nd->r[nd->nr].ref = ref;
  nd->nr++;
}

static void node_add_arr(SpNode *nd, const char *key, size_t klen, int *ids, int n /*owned*/) {
  ensure_cap((void **)&nd->a, &nd->ca, nd->na + 1, sizeof(SpArrField));
  nd->a[nd->na].key = dup_n(key, klen);
  nd->a[nd->na].ids = ids;
  nd->a[nd->na].n = n;
  nd->na++;
}

/* ---- line splitting ----
 * Splits a line (sans newline) into tag/id/field/value where value is
 * everything after the third space (possibly empty). Returns the number
 * of leading single-token parts found (1..3) and sets *val_off to the
 * byte offset of the value remainder (or len if none). Token boundaries
 * are reported via the parts[] offset/len pairs. */
typedef struct { const char *p; size_t len; } Tok;

static int split_line(const char *line, size_t len, Tok parts[3], const char **val, size_t *val_len) {
  int np = 0;
  size_t i = 0;
  *val = NULL;
  *val_len = 0;
  while (np < 3 && i < len) {
    size_t start = i;
    while (i < len && line[i] != ' ') i++;
    parts[np].p = line + start;
    parts[np].len = i - start;
    np++;
    if (i < len) i++; /* skip the space */
    if (np == 3) {
      /* remainder (after the 3rd space) is the value */
      *val = line + i;
      *val_len = len - i;
      break;
    }
  }
  return np;
}

static long long parse_ll(const char *s, size_t len) {
  long long sign = 1, v = 0;
  size_t i = 0;
  if (i < len && (s[i] == '-' || s[i] == '+')) { if (s[i] == '-') sign = -1; i++; }
  for (; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') break;
    v = v * 10 + (s[i] - '0');
  }
  return v * sign;
}

/* Parse a comma-separated id list into a malloc'd int array. */
static int *parse_id_list(const char *s, size_t len, int *out_n) {
  if (len == 0) { *out_n = 0; return NULL; }
  int cap = 8, n = 0;
  int *ids = malloc(sizeof(int) * cap);
  size_t i = 0;
  while (i < len) {
    size_t start = i;
    while (i < len && s[i] != ',') i++;
    if (n >= cap) { cap *= 2; ids = realloc(ids, sizeof(int) * cap); }
    ids[n++] = (int)parse_ll(s + start, i - start);
    if (i < len) i++; /* skip comma */
  }
  *out_n = n;
  return ids;
}

static int tok_eq(Tok t, const char *lit) {
  size_t l = strlen(lit);
  return t.len == l && memcmp(t.p, lit, l) == 0;
}

NodeTable *nt_load_text(const char *text) {
  NodeTable *nt = calloc(1, sizeof(NodeTable));
  if (!nt) return NULL;
  nt->root_id = 0;

  size_t tlen = strlen(text);

  /* Pass 1: find max node id, ROOT, SOURCE_FILE. */
  int max_id = 0;
  {
    size_t i = 0;
    while (i < tlen) {
      size_t ls = i;
      while (i < tlen && text[i] != '\n') i++;
      size_t llen = i - ls;
      if (i < tlen) i++;
      if (llen == 0) continue;
      const char *line = text + ls;
      Tok parts[3];
      const char *val; size_t vlen;
      int np = split_line(line, llen, parts, &val, &vlen);
      if (np >= 2) {
        if (tok_eq(parts[0], "ROOT")) {
          nt->root_id = (int)parse_ll(parts[1].p, parts[1].len);
        }
        else if (tok_eq(parts[0], "SOURCE_FILE")) {
          /* path is parts[1] plus possibly value; emitted as one token */
          nt->source_file = unescape_dup(parts[1].p, parts[1].len);
        }
        else {
          /* N/S/I/F/R/A all carry a node id in parts[1] */
          char c = parts[0].p[0];
          if (parts[0].len == 1 && (c == 'N' || c == 'S' || c == 'I' ||
                                    c == 'F' || c == 'R' || c == 'A')) {
            int nid = (int)parse_ll(parts[1].p, parts[1].len);
            if (nid > max_id) max_id = nid;
          }
        }
      }
    }
  }

  nt->count = max_id + 1;
  nt->nodes = calloc((size_t)nt->count, sizeof(SpNode));
  if (!nt->nodes) { free(nt); return NULL; }

  /* Pass 2: populate fields. */
  {
    size_t i = 0;
    while (i < tlen) {
      size_t ls = i;
      while (i < tlen && text[i] != '\n') i++;
      size_t llen = i - ls;
      if (i < tlen) i++;
      if (llen == 0) continue;
      const char *line = text + ls;
      Tok parts[3];
      const char *val; size_t vlen;
      int np = split_line(line, llen, parts, &val, &vlen);
      if (np < 2 || parts[0].len != 1) continue;
      char tag = parts[0].p[0];
      int nid = (int)parse_ll(parts[1].p, parts[1].len);
      if (nid < 0 || nid >= nt->count) continue;
      SpNode *nd = &nt->nodes[nid];

      if (tag == 'N') {
        /* type is parts[2] */
        if (np >= 3) node_set_type(nd, parts[2].p, parts[2].len);
      }
      else if (np >= 3) {
        Tok field = parts[2];
        if (tag == 'S') {
          char *uv = unescape_dup(val ? val : "", val ? vlen : 0);
          node_add_str(nd, field.p, field.len, uv);
        }
        else if (tag == 'I') {
          node_add_int(nd, field.p, field.len, parse_ll(val ? val : "", val ? vlen : 0));
        }
        else if (tag == 'R') {
          int ref = val ? (int)parse_ll(val, vlen) : -1;
          node_add_ref(nd, field.p, field.len, ref);
        }
        else if (tag == 'A') {
          int n = 0;
          int *ids = parse_id_list(val ? val : "", val ? vlen : 0, &n);
          node_add_arr(nd, field.p, field.len, ids, n);
        }
        else if (tag == 'F') {
          /* content = the value token (float string); field name ignored */
          free(nd->content);
          nd->content = dup_n(val ? val : "", val ? vlen : 0);
        }
      }
    }
  }

  return nt;
}

void nt_free(NodeTable *nt) {
  if (!nt) return;
  for (int k = 0; k < nt->count; k++) {
    SpNode *nd = &nt->nodes[k];
    free(nd->type);
    free(nd->content);
    for (int j = 0; j < nd->ns; j++) { free(nd->s[j].key); free(nd->s[j].val); }
    for (int j = 0; j < nd->ni; j++) free(nd->i[j].key);
    for (int j = 0; j < nd->nr; j++) free(nd->r[j].key);
    for (int j = 0; j < nd->na; j++) { free(nd->a[j].key); free(nd->a[j].ids); }
    free(nd->s); free(nd->i); free(nd->r); free(nd->a);
  }
  free(nt->nodes);
  free(nt->source_file);
  free(nt);
}

/* ---- accessors ---- */

static const SpNode *node_at(const NodeTable *nt, int id) {
  if (id < 0 || id >= nt->count) return NULL;
  return &nt->nodes[id];
}

const char *nt_type(const NodeTable *nt, int id) {
  const SpNode *nd = node_at(nt, id);
  return nd ? nd->type : NULL;
}

const char *nt_str(const NodeTable *nt, int id, const char *key) {
  const SpNode *nd = node_at(nt, id);
  if (!nd) return NULL;
  for (int j = 0; j < nd->ns; j++)
    if (strcmp(nd->s[j].key, key) == 0) return nd->s[j].val;
  return NULL;
}

long long nt_int(const NodeTable *nt, int id, const char *key, long long dflt) {
  const SpNode *nd = node_at(nt, id);
  if (!nd) return dflt;
  for (int j = 0; j < nd->ni; j++)
    if (strcmp(nd->i[j].key, key) == 0) return nd->i[j].val;
  return dflt;
}

int nt_ref(const NodeTable *nt, int id, const char *key) {
  const SpNode *nd = node_at(nt, id);
  if (!nd) return -1;
  for (int j = 0; j < nd->nr; j++)
    if (strcmp(nd->r[j].key, key) == 0) return nd->r[j].ref;
  return -1;
}

const int *nt_arr(const NodeTable *nt, int id, const char *key, int *out_n) {
  *out_n = 0;
  const SpNode *nd = node_at(nt, id);
  if (!nd) return NULL;
  for (int j = 0; j < nd->na; j++)
    if (strcmp(nd->a[j].key, key) == 0) { *out_n = nd->a[j].n; return nd->a[j].ids; }
  return NULL;
}

const char *nt_content(const NodeTable *nt, int id) {
  const SpNode *nd = node_at(nt, id);
  return nd ? nd->content : NULL;
}

int nt_num_refs(const NodeTable *nt, int id) {
  const SpNode *nd = node_at(nt, id);
  return nd ? nd->nr : 0;
}
int nt_ref_at(const NodeTable *nt, int id, int i) {
  const SpNode *nd = node_at(nt, id);
  if (!nd || i < 0 || i >= nd->nr) return -1;
  return nd->r[i].ref;
}
int nt_num_arrs(const NodeTable *nt, int id) {
  const SpNode *nd = node_at(nt, id);
  return nd ? nd->na : 0;
}
const int *nt_arr_at(const NodeTable *nt, int id, int i, int *out_n) {
  *out_n = 0;
  const SpNode *nd = node_at(nt, id);
  if (!nd || i < 0 || i >= nd->na) return NULL;
  *out_n = nd->a[i].n;
  return nd->a[i].ids;
}
