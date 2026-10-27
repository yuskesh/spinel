/* M3' (derived from spinel-mlir 3accdcd patches/m3/source_map.inc for this
   pin), built only under SPINEL_MLIR_POSITIONS.

   spinel-mlir's M3 kept a second line map because Spinel's let every PUSH,
   its own inserted requires included, consume one parent line. On this pin
   Spinel marks an inserted require with SPINEL_INSERT, which consumes none,
   so sp_build_line_map already gives each buffer line its original file and
   line. M3' does not correct positions. It keeps M3's other two duties:

   - every buffer line a node may start or end on is compared byte for byte
     with the file and line the facts would name for it (the #<SPINEL_SOURCE>
     display position where one applies); a node on a line that fails is
     refused, never shifted;
   - a node whose end lies in another file than its start (the root
     ProgramNode / StatementsNode when a required file's statements come
     first) gets no position, as M3 did after its addendum B.

   Spinel's own sp_line_orig, __LINE__ and the flag-off path are untouched. */
int sp_m3_fail_count = 0, sp_m3_fail_node = -1, sp_m3_fail_file = -1, sp_m3_fail_line = 0;
const char *sp_m3_fail_reason = NULL;
int sp_m3_top_file = 0;
static int sp_m3_built = 0, sp_m3_n = 0;
static int *sp_m3_file = NULL, *sp_m3_orig = NULL;
static unsigned char *sp_m3_reason = NULL;   /* 0 verified, 1 marker, 2 content */
static const char *const sp_m3_reason_name[] = { "verified", "marker", "content" };

static void sp_m3_fail(int id, int fid, int bl, const char *why) {
  if (sp_m3_fail_count++ == 0) {
    sp_m3_fail_node = id; sp_m3_fail_file = fid; sp_m3_fail_line = bl; sp_m3_fail_reason = why;
  }
}

/* Line `line` (1-based) of the original file `fid`, read once per file. */
static int sp_m3_source_line(int fid, int line, const char **s, size_t *n) {
  static char **texts = NULL; static int ntexts = 0;
  if (fid < 0 || fid >= sp_file_count || line < 1) return 0;
  if (fid >= ntexts) {
    char **nt = (char **)realloc(texts, sizeof *nt * (size_t)sp_file_count);
    if (!nt) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    for (int i = ntexts; i < sp_file_count; i++) nt[i] = NULL;
    texts = nt; ntexts = sp_file_count;
  }
  if (!texts[fid]) texts[fid] = read_file(sp_file_table[fid]);
  if (!texts[fid]) return 0;
  const char *p = texts[fid];
  for (int l = 1; l < line; l++) { p = strchr(p, '\n'); if (!p) return 0; p++; }
  if (!*p && line > 1) return 0;
  const char *e = strchr(p, '\n');
  *s = p; *n = e ? (size_t)(e - p) : strlen(p);
  return 1;
}

/* `buffer` is the text sp_build_line_map was just given, the one Prism parses. */
static void sp_m3_build(const char *buffer) {
  sp_m3_n = sp_line_map_n;
  sp_m3_file = (int *)calloc((size_t)sp_m3_n + 2, sizeof(int));
  sp_m3_orig = (int *)calloc((size_t)sp_m3_n + 2, sizeof(int));
  sp_m3_reason = (unsigned char *)calloc((size_t)sp_m3_n + 2, 1);
  if (!sp_m3_file || !sp_m3_orig || !sp_m3_reason) {
    fprintf(stderr, "spinel_parse: out of memory\n"); exit(1);
  }
  const char *line = buffer;
  for (int bl = 1; bl <= sp_m3_n && line; bl++) {
    const char *eol = strchr(line, '\n');
    size_t len = eol ? (size_t)(eol - line) : strlen(line);
    if (sp_line_orig[bl] <= 0) sp_m3_reason[bl] = 1;
    else {
      int fid = sp_line_file[bl], orig = sp_line_orig[bl];
      if (sp_disp_line[bl] > 0) { fid = sp_disp_file[bl]; orig = sp_disp_line[bl]; }
      sp_m3_file[bl] = fid; sp_m3_orig[bl] = orig;
      const char *s; size_t n;
      if (!sp_m3_source_line(fid, orig, &s, &n) || n != len || memcmp(s, line, len) != 0)
        sp_m3_reason[bl] = 2;
    }
    line = eol ? eol + 1 : NULL;
  }
  sp_m3_built = 1;
}

static void sp_m3_fix_start(int id, int bl, int *orig, int *fid) {
  if (!sp_m3_built || bl < 1 || bl > sp_m3_n) { sp_m3_fail(id, -1, bl, "map_unavailable"); return; }
  if (sp_m3_reason[bl] == 1) { *orig = 0; *fid = 0; return; }   /* as M3: a marker line maps to nothing */
  if (sp_m3_reason[bl]) { sp_m3_fail(id, sp_m3_file[bl], bl, sp_m3_reason_name[sp_m3_reason[bl]]); return; }
  *orig = sp_m3_orig[bl];
  *fid = sp_m3_file[bl];
}

/* fid is the node's start file. A node whose end lies in another file has no
   span in one original file: it gets no position, as a marker line does,
   rather than one file's name with the other's line. */
static void sp_m3_fix_end(int id, int el, int fid, int *eorig) {
  if (!sp_m3_built) return;   /* the start already failed */
  if (el < 1 || el > sp_m3_n) { sp_m3_fail(id, -1, el, "map_unavailable"); return; }
  if (sp_m3_reason[el] == 1) { *eorig = 0; return; }
  if (sp_m3_reason[el]) { sp_m3_fail(id, sp_m3_file[el], el, sp_m3_reason_name[sp_m3_reason[el]]); return; }
  if (sp_m3_file[el] != fid) { *eorig = 0; return; }
  *eorig = sp_m3_orig[el];
}
