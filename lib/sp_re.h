#ifndef SP_RE_H
#define SP_RE_H
/* sp_re.h -- regexp wrappers + MatchData, compiled once in lib/sp_re.c.
 *
 * The regexp ENGINE (re_compile / re_exec / re_free) is the separate
 * build/regexp/*.o library; this layer is spinel's Ruby-facing surface
 * (Regexp#match, gsub/sub/scan/split, the $~/$1.. match registers, and
 * MatchData). The match-register globals are per-process singletons: the
 * generated TU reads them directly for $&/$1.. (codegen_expr.c) and marks
 * them in its whole-program GC root hook, so they are extern here and
 * defined once in lib/sp_re.c.
 *
 * Kept in sp_runtime.h (TU-coupled): the whole-program GC root hook
 * sp_re_mark_globals, the startup-error handler (longjmps through a
 * TU-local jmp_buf), and the hash-replacement gsub/sub variants (await
 * the typed-hash batch). */
#include "sp_array.h"   /* sp_StrArray / sp_PolyArray / sp_RbVal + heap */
#include "sp_str.h"     /* string helpers used by the wrappers */

/* ---- regexp engine ABI (implemented in build/regexp/*.o) ---- */
typedef struct mrb_regexp_pattern mrb_regexp_pattern;
mrb_regexp_pattern* re_compile(const char *pattern, int64_t len, uint32_t flags);
void re_free(mrb_regexp_pattern *pat);
int re_exec(const mrb_regexp_pattern *pat, const char *str, int64_t len, int64_t start, int *captures, int captures_size);
/* named-capture introspection (lib/regexp/re_compile.c) */
int re_num_named(const mrb_regexp_pattern *pat);
const char *re_named_name(const mrb_regexp_pattern *pat, int i, int *group_out);
int re_named_group(const mrb_regexp_pattern *pat, const char *name);

typedef struct { const char *source; int caps[64]; int ncap; const mrb_regexp_pattern *pat; } sp_MatchData;

/* ---- match-register state (per-process; read by the generated TU and
   marked by its GC root hook) ---- */
extern const char *sp_re_captures[10];
extern int sp_re_caps[64];
extern const char *sp_re_last_str;
extern const char *sp_re_match_str;
extern const char *sp_re_match_pre;
extern const char *sp_re_match_post;
extern const char *sp_re_startup_err;

/* ---- wrappers (lib/sp_re.c) ---- */
const char *sp_re_last_paren_match(void);
void sp_re_set_captures(const char *str, int *caps, int ncaps);
mrb_int sp_re_match(mrb_regexp_pattern *pat, const char *str);
mrb_int sp_re_rindex(mrb_regexp_pattern *pat, const char *str);
sp_StrArray *sp_re_rpartition(mrb_regexp_pattern *pat, const char *str);
mrb_bool sp_re_match_p(mrb_regexp_pattern *pat, const char *str);
mrb_bool sp_re_match_p_at(mrb_regexp_pattern *pat, const char *str, mrb_int pos);
void sp_re_expand_rep(char **out_io, size_t *olen_io, size_t *cap_io, const char *rep, size_t rlen, const char *src, int *caps, int ncaps);
const char *sp_re_gsub(mrb_regexp_pattern *pat, const char *str, const char *rep);
const char *sp_re_sub(mrb_regexp_pattern *pat, const char *str, const char *rep);
sp_StrArray *sp_re_scan(mrb_regexp_pattern *pat, const char *str);
sp_StrArray *sp_re_split(mrb_regexp_pattern *pat, const char *str);
sp_StrArray *sp_re_split_limit(mrb_regexp_pattern *pat, const char *str, mrb_int limit);
mrb_int sp_re_rindex_opt(mrb_regexp_pattern *pat, const char *str);
sp_RbVal sp_re_rindex_poly(mrb_regexp_pattern *pat, const char *str);
sp_RbVal sp_re_index_poly(mrb_regexp_pattern *pat, const char *str);
sp_RbVal sp_re_match_poly(mrb_regexp_pattern *pat, const char *str);
const char *sp_re_escape(const char *src);
sp_PolyArray *sp_re_scan_poly(mrb_regexp_pattern *pat, const char *str);
sp_PolyArray *sp_re_match_data(mrb_regexp_pattern *pat, const char *str);
void sp_MatchData_scan(void *p);
sp_MatchData *sp_re_matchdata(mrb_regexp_pattern *pat, const char *str);
sp_MatchData *sp_re_matchdata_at(mrb_regexp_pattern *pat, const char *str, mrb_int cpos);
const char *sp_MatchData_aref(sp_MatchData *m, mrb_int i);
const char *sp_MatchData_aref_name(sp_MatchData *m, const char *name);
sp_StrArray *sp_MatchData_names(sp_MatchData *m);
mrb_int sp_MatchData_length(sp_MatchData *m);
mrb_int sp_md_char_off(sp_MatchData *m, int byteoff);
mrb_int sp_MatchData_begin(sp_MatchData *m, mrb_int i);
mrb_int sp_MatchData_end(sp_MatchData *m, mrb_int i);
sp_IntArray *sp_MatchData_offset(sp_MatchData *m, mrb_int i);
const char *sp_MatchData_to_s(sp_MatchData *m);
sp_PolyArray *sp_MatchData_captures(sp_MatchData *m);
sp_PolyArray *sp_MatchData_to_a(sp_MatchData *m);
const char *sp_MatchData_pre_match(sp_MatchData *m);
const char *sp_MatchData_post_match(sp_MatchData *m);
void sp_re_default_error_handler(const char *msg);

#endif /* SP_RE_H */
