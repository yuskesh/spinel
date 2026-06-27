/* sp_re.c -- regexp wrappers + MatchData (see sp_re.h).
 *
 * Moved out of sp_runtime.h so this layer compiles once into
 * libspinel_rt.a. It calls the regexp engine (re_*) and the shared string
 * heap / arrays; sp_sprintf / sp_raise_cls resolve at the final link. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "sp_re.h"
#include "sp_string.h"   /* sp_String builder */
#include "sp_inspect.h"  /* sp_inspect_container (poly element render) */

#ifndef SPL
#define SPL(s) (&("\xff" s)[1])
#endif
const char *sp_sprintf(const char *fmt, ...);  /* defined in the generated TU */

/* match-register state (declared extern in sp_re.h). */
const char *sp_re_captures[10] = {0};
int sp_re_caps[64];
const char *sp_re_last_str = NULL;
const char *sp_re_match_str = NULL;
const char *sp_re_match_pre = NULL;
const char *sp_re_match_post = NULL;
const char *sp_re_startup_err = NULL;

const char *sp_re_last_paren_match(void) {
  for (int i = 9; i >= 1; i--) {
    if (sp_re_captures[i]) return sp_re_captures[i];
  }
  return NULL;
}
void sp_re_set_captures(const char *str, int *caps, int ncaps) {
  sp_re_last_str = str;
  for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
  for (int i = 1; i < ncaps && i < 10; i++) {
    if (caps[i*2] >= 0 && caps[(i*2)+1] >= 0) {
      int len = caps[(i*2)+1] - caps[i*2];
      char *buf = sp_str_alloc_raw(len+1);
      memcpy(buf, str+caps[i*2], len); buf[len] = 0;
      sp_re_captures[i] = buf;
    }
  }
  /* Populate the symbolic back-references from caps[0]/[1] (the whole
     match span). NULL when the match failed; the codegen ternary
     falls back to "". */
  sp_re_match_str = NULL;
  sp_re_match_pre = NULL;
  sp_re_match_post = NULL;
  if (ncaps >= 1 && caps[0] >= 0 && caps[1] >= 0) {
    int slen = (int)strlen(str);
    int mlen = caps[1] - caps[0];
    char *m = sp_str_alloc_raw(mlen + 1);
    memcpy(m, str + caps[0], mlen); m[mlen] = 0;
    sp_re_match_str = m;
    char *pre = sp_str_alloc_raw(caps[0] + 1);
    memcpy(pre, str, caps[0]); pre[caps[0]] = 0;
    sp_re_match_pre = pre;
    int post_len = slen - caps[1];
    char *post = sp_str_alloc_raw(post_len + 1);
    memcpy(post, str + caps[1], post_len); post[post_len] = 0;
    sp_re_match_post = post;
  }
}
mrb_int sp_re_match(mrb_regexp_pattern *pat, const char *str) {
  int64_t slen = (int64_t)strlen(str);
  int ncaps = 32;
  int n = re_exec(pat, str, slen, 0, sp_re_caps, ncaps);
  if (n > 0) { sp_re_set_captures(str, sp_re_caps, n/2); return sp_re_caps[0]; }
  /* Issue #848: clear backrefs on no-match so a subsequent `$1`
     reads as nil rather than the previous match's group. */
  for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
  sp_re_last_str = NULL;
  sp_re_match_str = NULL;
  sp_re_match_pre = NULL;
  sp_re_match_post = NULL;
  return -1;
}
mrb_int sp_re_rindex(mrb_regexp_pattern *pat, const char *str) {
  int64_t slen = (int64_t)strlen(str);
  int caps[2];
  int64_t pos = 0;
  mrb_int last = -1;
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 2);
    if (n <= 0) break;
    last = caps[0];
    /* Advance past the match; for zero-width matches step by 1
       to avoid an infinite loop. */
    int64_t next = caps[1];
    if (next <= pos) next = pos + 1;
    pos = next;
  }
  return last;
}
sp_StrArray *sp_re_rpartition(mrb_regexp_pattern *pat, const char *str) {
  int64_t slen = (int64_t)strlen(str);
  int caps[2];
  int64_t pos = 0;
  mrb_int ms = -1, me = -1;
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 2);
    if (n <= 0) break;
    ms = caps[0]; me = caps[1];
    /* rpartition keys on the rightmost match START (MRI reverse search),
       so step one past this start to look for a later-starting match. */
    pos = caps[0] + 1;
  }
  sp_StrArray *r = sp_StrArray_new();
  if (ms < 0) {
    sp_StrArray_push(r, SPL(""));
    sp_StrArray_push(r, SPL(""));
    sp_StrArray_push(r, str);
    return r;
  }
  char *before = sp_str_alloc_raw(ms + 1);
  memcpy(before, str, ms); before[ms] = 0;
  int mlen = (int)(me - ms);
  char *mid = sp_str_alloc_raw(mlen + 1);
  memcpy(mid, str + ms, mlen); mid[mlen] = 0;
  int alen = (int)(slen - me);
  char *after = sp_str_alloc_raw(alen + 1);
  memcpy(after, str + me, alen); after[alen] = 0;
  sp_StrArray_push(r, before);
  sp_StrArray_push(r, mid);
  sp_StrArray_push(r, after);
  return r;
}
mrb_bool sp_re_match_p(mrb_regexp_pattern *pat, const char *str) {
  int64_t slen = (int64_t)strlen(str);
  int caps[2];
  return re_exec(pat, str, slen, 0, caps, 2) > 0;
}
mrb_bool sp_re_match_p_at(mrb_regexp_pattern *pat, const char *str, mrb_int pos) {
  int64_t slen = (int64_t)strlen(str);
  if (pos < 0) pos += slen;
  if (pos < 0 || pos > slen) return FALSE;
  int caps[2];
  return re_exec(pat, str, slen, (mrb_int)pos, caps, 2) > 0;
}
void sp_re_expand_rep(char **out_io, size_t *olen_io, size_t *cap_io,
                             const char *rep, size_t rlen,
                             const char *src, int *caps, int ncaps) {
  size_t olen = *olen_io;
  char *out = *out_io;
  size_t cap = *cap_io;
  size_t i = 0;
  while (i < rlen) {
    char c = rep[i];
    if (c == '\\' && i + 1 < rlen) {
      char d = rep[i+1];
      if ((d >= '0' && d <= '9') || d == '&') {
        int gi = (d == '&') ? 0 : (d - '0');
        if ((gi*2) + 1 < ncaps && caps[gi*2] >= 0 && caps[(gi*2)+1] >= 0) {
          int g_len = caps[(gi*2)+1] - caps[gi*2];
          if (olen + g_len + 1 >= cap) { cap = ((olen + g_len) * 2) + 64; out = (char*)realloc(out, cap); }
          memcpy(out+olen, src + caps[gi*2], g_len);
          olen += g_len;
        }
        i += 2;
        continue;
      }
else if (d == '\\') {
        if (olen + 1 >= cap) { cap = (cap * 2) + 64; out = (char*)realloc(out, cap); }
        out[olen++] = '\\';
        i += 2;
        continue;
      }
    }
    if (olen + 1 >= cap) { cap = (cap * 2) + 64; out = (char*)realloc(out, cap); }
    out[olen++] = c;
    i++;
  }
  *out_io = out; *olen_io = olen; *cap_io = cap;
}
const char *sp_re_gsub(mrb_regexp_pattern *pat, const char *str, const char *rep) {
  int64_t slen = (int64_t)strlen(str); size_t rlen = strlen(rep);
  size_t cap = (slen * 2) + (rlen * 4) + 64;
 /* Build into a plain malloc scratch: the buffer is grown with realloc
    here and inside sp_re_expand_rep, which is only valid on a real
    malloc base (a sp_str body pointer is offset past its header). The
    final string is allocated at the exact length below. */
  char *out = (char *)malloc(cap); size_t olen = 0;
  int64_t pos = 0; int caps[64];
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 64);
    if (n <= 0 || caps[0] < 0) break;
    size_t before = caps[0] - pos;
    if (olen+before+rlen >= cap) { cap = ((olen+before+rlen)*2)+64; out = (char*)realloc(out, cap); }
    memcpy(out+olen, str+pos, before); olen += before;
    sp_re_expand_rep(&out, &olen, &cap, rep, rlen, str, caps, n);
    if (caps[0] == caps[1]) {
 /* Zero-width match (/^/, /$/, /\b/, an empty pattern): Ruby inserts
    the replacement before the char at this position, keeps that char,
    and advances past it. Copy the char and step by one so the scan
    makes progress without dropping it or spinning on the same spot. */
      if (caps[1] < slen) {
        if (olen+1 >= cap) { cap = (olen*2)+64; out = (char*)realloc(out, cap); }
        out[olen++] = str[caps[1]];
      }
      pos = caps[1] + 1;
    }
else {
      pos = caps[1];
    }
  }
 /* pos can land at slen+1 after a zero-width match at the end; guard the
    tail copy so `slen - pos` doesn't underflow size_t. */
  if (pos < slen) {
    size_t rest = slen - pos;
    if (olen+rest+1 >= cap) { cap = olen+rest+64; out = (char*)realloc(out, cap); }
    memcpy(out+olen, str+pos, rest); olen += rest;
  }
 /* Emit a string sized to exactly the bytes written (sp_str_alloc sets
    the length and null-terminates); release the scratch. */
  char *res = sp_str_alloc(olen);
  memcpy(res, out, olen);
  free(out);
  return res;
}
const char *sp_re_sub(mrb_regexp_pattern *pat, const char *str, const char *rep) {
  int64_t slen = (int64_t)strlen(str); size_t rlen = strlen(rep);
  int caps[64];
  int n = re_exec(pat, str, slen, 0, caps, 64);
  if (n <= 0 || caps[0] < 0) return str;
  /* Issue #855: expand `\1`..`\9` / `\&` from rep against caps. */
  size_t cap = caps[0] + (rlen * 4) + (slen - caps[1]) + 64;
 /* malloc scratch: sp_re_expand_rep and the tail grow it with realloc,
    which needs a real malloc base. Exact-sized string emitted below. */
  char *out = (char *)malloc(cap);
  memcpy(out, str, caps[0]);
  size_t olen = caps[0];
  sp_re_expand_rep(&out, &olen, &cap, rep, rlen, str, caps, n);
  size_t rest = slen - caps[1];
  if (olen + rest + 1 >= cap) { cap = olen + rest + 64; out = (char*)realloc(out, cap); }
  memcpy(out+olen, str+caps[1], rest); olen += rest;
  char *res = sp_str_alloc(olen);
  memcpy(res, out, olen);
  free(out);
  return res;
}
sp_StrArray *sp_re_scan(mrb_regexp_pattern *pat, const char *str) {
  sp_StrArray *arr = sp_StrArray_new();
  int64_t slen = (int64_t)strlen(str); int64_t pos = 0; int caps[64];
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 64);
    if (n <= 0 || caps[0] < 0) break;
    int len = caps[1] - caps[0];
    char *m = sp_str_alloc_raw(len+1); memcpy(m, str+caps[0], len); m[len] = 0;
    sp_StrArray_push(arr, m);
    pos = caps[1]; if (caps[0] == caps[1]) pos++;
  }
  return arr;
}
/* Forward decl from the regexp engine (lib/regexp/re_utf8.c). */
int re_utf8_charlen(const char *s, const char *end);

/* String#split(regexp[, limit]). Mirrors CRuby / upstream mruby-regexp
   string_regexp.rb#split: a zero-width match steps past one whole (multibyte-
   aware) character and never emits an empty leading field; capture groups are
   spliced between the surrounding fields (unmatched optional groups are
   omitted, like md[i].nil?); and with the default limit (0) trailing empty
   fields are stripped. limit > 0 caps the field count (the last field is the
   unsplit remainder); limit < 0 keeps trailing empties. An empty subject is
   always []. */
static void split_push_slice(sp_StrArray *arr, const char *str, int64_t from, int64_t to) {
  int len = (int)(to - from);
  char *m = sp_str_alloc_raw(len + 1);
  memcpy(m, str + from, len); m[len] = 0;
  sp_StrArray_push(arr, m);
}

sp_StrArray *sp_re_split_limit(mrb_regexp_pattern *pat, const char *str, mrb_int limit) {
  sp_StrArray *arr = sp_StrArray_new();
  int64_t slen = (int64_t)strlen(str);

  /* limit == 1: the whole string is the single field; "" splits to []. */
  if (limit == 1) {
    if (slen > 0) split_push_slice(arr, str, 0, slen);
    return arr;
  }

  int64_t field_start = 0, search_pos = 0, count = 0;
  int caps[64];
  while (search_pos <= slen) {
    if (limit > 0 && count >= limit - 1) {
      split_push_slice(arr, str, field_start, slen);
      return arr;
    }
    int n = re_exec(pat, str, slen, search_pos, caps, 64);
    if (n <= 0 || caps[0] < 0) break;
    int64_t match_start = caps[0], match_end = caps[1];

    if (match_start == match_end) {
      /* zero-width: advance past one whole character so multibyte text is
         not split between bytes. */
      if (match_end < slen) {
        search_pos = match_end + re_utf8_charlen(str + match_end, str + slen);
      }
      else {
        search_pos = match_end + 1;
      }
      if (match_start == field_start) continue;  /* nothing to emit yet */
    }

    split_push_slice(arr, str, field_start, match_start);
    count++;
    field_start = match_end;
    if (match_start != match_end) search_pos = match_end;

    /* Splice captured groups (caps[0] is the whole match; groups 1.. follow).
       Unmatched optional groups are skipped, matching CRuby. */
    for (int gi = 1; (gi * 2) + 1 < n; gi++) {
      if (caps[gi*2] >= 0 && caps[(gi*2)+1] >= 0) {
        split_push_slice(arr, str, caps[gi*2], caps[(gi*2)+1]);
      }
    }
  }

  /* Trailing field (omitted for an empty subject, or when stripped below). */
  if (slen > 0 && field_start <= slen && (field_start < slen || limit != 0)) {
    split_push_slice(arr, str, field_start, slen);
  }

  /* Default limit strips trailing empty fields. */
  if (limit == 0) {
    while (arr->len > 0 && arr->data[arr->len - 1][0] == '\0') arr->len--;
  }
  return arr;
}

sp_StrArray *sp_re_split(mrb_regexp_pattern *pat, const char *str) {
  return sp_re_split_limit(pat, str, 0);
}
mrb_int sp_re_rindex_opt(mrb_regexp_pattern *pat, const char *str)  { mrb_int n = sp_re_rindex(pat, str); return n < 0 ? SP_INT_NIL : n; }
sp_RbVal sp_re_rindex_poly(mrb_regexp_pattern *pat, const char *str) { mrb_int n = sp_re_rindex(pat, str); return n < 0 ? sp_box_nil() : sp_box_int(n); }
sp_RbVal sp_re_index_poly(mrb_regexp_pattern *pat, const char *str) { mrb_int n = sp_re_match(pat, str); return n < 0 ? sp_box_nil() : sp_box_int(n); }
sp_RbVal sp_re_match_poly(mrb_regexp_pattern *pat, const char *str) { mrb_int n = sp_re_match(pat, str); return n < 0 ? sp_box_nil() : sp_box_int(n); }
const char *sp_re_escape(const char *src) {
  size_t i, in_len = strlen(src);
  size_t out_len = 0;
  for (i = 0; i < in_len; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c == '\\' || c == '.' || c == '?' || c == '*' || c == '+' ||
        c == '^' || c == '$' || c == '|' || c == '(' || c == ')' ||
        c == '[' || c == ']' || c == '{' || c == '}' || c == '#' ||
        c == '-' || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
        c == '\f' || c == '\v') {
      out_len += 2;
    }
else {
      out_len += 1;
    }
  }
  if (out_len == in_len) {
    return src;
  }
  char *buf = sp_str_alloc(out_len);
  size_t j = 0;
  for (i = 0; i < in_len; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c == '\\' || c == '.' || c == '?' || c == '*' || c == '+' ||
        c == '^' || c == '$' || c == '|' || c == '(' || c == ')' ||
        c == '[' || c == ']' || c == '{' || c == '}' || c == '#' ||
        c == '-' || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
        c == '\f' || c == '\v') {
      buf[j++] = '\\';
      buf[j++] = (char)c;
    }
else {
      buf[j++] = (char)c;
    }
  }
  buf[j] = 0;
  return buf;
}
sp_PolyArray *sp_re_scan_poly(mrb_regexp_pattern *pat, const char *str) {
  sp_PolyArray *arr = sp_PolyArray_new();
  SP_GC_ROOT(arr);
  int64_t slen = (int64_t)strlen(str);
  int64_t pos = 0;
  int ncaps = 64;
  int caps[64];
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, ncaps);
    if (n <= 0 || caps[0] < 0) break;
    int pairs = (n > ncaps ? ncaps : n) / 2;
    if (pairs <= 1) {
      int len = caps[1] - caps[0];
      char *m = sp_str_alloc_raw(len + 1);
      memcpy(m, str + caps[0], len);
      m[len] = 0;
      sp_PolyArray_push(arr, sp_box_str(m));
    }
else {
      sp_PolyArray *row = sp_PolyArray_new();
      for (int gi = 1; gi < pairs; gi++) {
        if (caps[gi * 2] >= 0 && caps[(gi * 2) + 1] >= 0) {
          int glen = caps[(gi * 2) + 1] - caps[gi * 2];
          char *gm = sp_str_alloc_raw(glen + 1);
          memcpy(gm, str + caps[gi * 2], glen);
          gm[glen] = 0;
          sp_PolyArray_push(row, sp_box_str(gm));
        }
else {
          sp_PolyArray_push(row, sp_box_nil());
        }
      }
      sp_PolyArray_push(arr, sp_box_poly_array(row));
    }
    pos = caps[1];
    if (caps[0] == caps[1]) pos++;
  }
  return arr;
}
sp_PolyArray *sp_re_match_data(mrb_regexp_pattern *pat, const char *str) {
  int64_t slen = (int64_t)strlen(str);
  int ncaps = 64;
  int n = re_exec(pat, str, slen, 0, sp_re_caps, ncaps);
  if (n <= 0 || sp_re_caps[0] < 0) {
    for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
    sp_re_last_str = NULL;
    sp_re_match_str = NULL;
    sp_re_match_pre = NULL;
    sp_re_match_post = NULL;
    return NULL;
  }
  int pairs = (n > ncaps ? ncaps : n) / 2;
  sp_re_set_captures(str, sp_re_caps, pairs);
  sp_PolyArray *arr = sp_PolyArray_new();
  for (int i = 0; i < pairs; i++) {
    int start = sp_re_caps[i * 2];
    int end = sp_re_caps[(i * 2) + 1];
    if (start >= 0 && end >= start) {
      int len = end - start;
      char *buf = sp_str_alloc_raw(len + 1);
      memcpy(buf, str + start, len);
      buf[len] = 0;
      sp_PolyArray_push(arr, sp_box_str(buf));
    }
else {
      sp_PolyArray_push(arr, sp_box_nil());
    }
  }
  return arr;
}
void sp_MatchData_scan(void *p) { sp_MatchData *m = (sp_MatchData *)p; if (m->source) sp_mark_string(m->source); }
sp_MatchData *sp_re_matchdata(mrb_regexp_pattern *pat, const char *str) {
  int64_t slen = (int64_t)strlen(str);
  int caps[64];
  int n = re_exec(pat, str, slen, 0, caps, 64);
  if (n <= 0 || caps[0] < 0) {
    for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
    sp_re_last_str = NULL; sp_re_match_str = NULL;
    sp_re_match_pre = NULL; sp_re_match_post = NULL;
    return NULL;
  }
  int pairs = (n > 64 ? 64 : n) / 2;
  sp_re_set_captures(str, caps, pairs);
  sp_MatchData *m = (sp_MatchData *)sp_gc_alloc(sizeof(sp_MatchData), NULL, sp_MatchData_scan);
  m->source = str;
  m->ncap = pairs;
  m->pat = pat;
  for (int i = 0; i < pairs * 2; i++) m->caps[i] = caps[i];
  return m;
}
/* String#match(/re/, pos) — pos is a codepoint index (CRuby semantics). */
sp_MatchData *sp_re_matchdata_at(mrb_regexp_pattern *pat, const char *str, mrb_int cpos) {
  mrb_int cl = sp_str_length(str);
  if (cpos < 0) cpos += cl;
  if (cpos < 0 || cpos > cl) return NULL;
  size_t boff = sp_utf8_byte_offset(str, cpos);
  int64_t slen = (int64_t)strlen(str);
  int caps[64];
  int n = re_exec(pat, str, slen, (mrb_int)boff, caps, 64);
  if (n <= 0 || caps[0] < 0) {
    for (int i = 0; i < 10; i++) sp_re_captures[i] = NULL;
    sp_re_last_str = NULL; sp_re_match_str = NULL;
    sp_re_match_pre = NULL; sp_re_match_post = NULL;
    return NULL;
  }
  int pairs = (n > 64 ? 64 : n) / 2;
  sp_re_set_captures(str, caps, pairs);
  sp_MatchData *m = (sp_MatchData *)sp_gc_alloc(sizeof(sp_MatchData), NULL, sp_MatchData_scan);
  m->source = str;
  m->ncap = pairs;
  m->pat = pat;
  for (int i = 0; i < pairs * 2; i++) m->caps[i] = caps[i];
  return m;
}
/* group i substring, or NULL for a non-participating / out-of-range group */
const char *sp_MatchData_aref(sp_MatchData *m, mrb_int i) {
  if (!m || i < 0 || i >= m->ncap) return NULL;
  int s = m->caps[i * 2], e = m->caps[(i * 2) + 1];
  if (s < 0 || e < s) return NULL;
  int len = e - s;
  char *b = sp_str_alloc((size_t)len);
  memcpy(b, m->source + s, len);
  b[len] = 0;
  sp_str_set_len(b, (size_t)len);
  return b;
}
/* group by name (`md[:name]` / `md["name"]`): resolve the name to its capture
   group via the pattern, then return that group's substring (NULL if the name
   is unknown or the group did not participate). */
const char *sp_MatchData_aref_name(sp_MatchData *m, const char *name) {
  if (!m || !name) return NULL;
  int g = re_named_group(m->pat, name);
  if (g < 0) sp_raise_cls("IndexError", sp_sprintf("undefined group name reference: %s", name));
  return sp_MatchData_aref(m, g);
}
/* `md.names`: the capture names in declaration order. */
sp_StrArray *sp_MatchData_names(sp_MatchData *m) {
  sp_StrArray *a = sp_StrArray_new();
  if (!m) return a;
  int n = re_num_named(m->pat);
  for (int i = 0; i < n; i++) {
    const char *nm = re_named_name(m->pat, i, NULL);
    if (nm) sp_StrArray_push(a, sp_str_dup(nm));
  }
  return a;
}
mrb_int sp_MatchData_length(sp_MatchData *m) { return m ? m->ncap : 0; }
/* char offset of a byte position within source */
mrb_int sp_md_char_off(sp_MatchData *m, int byteoff) {
  if (byteoff < 0) return SP_INT_NIL;
  return sp_str_count_chars(m->source, (size_t)byteoff);
}
mrb_int sp_MatchData_begin(sp_MatchData *m, mrb_int i) {
  if (!m || i < 0 || i >= m->ncap) return SP_INT_NIL;
  return sp_md_char_off(m, m->caps[i * 2]);
}
mrb_int sp_MatchData_end(sp_MatchData *m, mrb_int i) {
  if (!m || i < 0 || i >= m->ncap) return SP_INT_NIL;
  return sp_md_char_off(m, m->caps[(i * 2) + 1]);
}
sp_IntArray *sp_MatchData_offset(sp_MatchData *m, mrb_int i) {
  sp_IntArray *a = sp_IntArray_new();
  if (!m || i < 0 || i >= m->ncap) { sp_IntArray_push(a, SP_INT_NIL); sp_IntArray_push(a, SP_INT_NIL); return a; }
  sp_IntArray_push(a, sp_md_char_off(m, m->caps[i * 2]));
  sp_IntArray_push(a, sp_md_char_off(m, m->caps[(i * 2) + 1]));
  return a;
}
/* whole-match string (group 0) — also MatchData#to_s */
const char *sp_MatchData_to_s(sp_MatchData *m) { const char *r = sp_MatchData_aref(m, 0); return r ? r : sp_str_empty; }
/* captures: groups 1..n-1 as a poly array (nil for non-participating) */
sp_PolyArray *sp_MatchData_captures(sp_MatchData *m) {
  sp_PolyArray *r = sp_PolyArray_new();
  if (!m) return r;
  SP_GC_ROOT(r);
  for (mrb_int i = 1; i < m->ncap; i++) {
    const char *g = sp_MatchData_aref(m, i);
    sp_PolyArray_push(r, g ? sp_box_str(g) : sp_box_nil());
  }
  return r;
}
/* to_a: group 0 + captures */
sp_PolyArray *sp_MatchData_to_a(sp_MatchData *m) {
  sp_PolyArray *r = sp_PolyArray_new();
  if (!m) return r;
  SP_GC_ROOT(r);
  for (mrb_int i = 0; i < m->ncap; i++) {
    const char *g = sp_MatchData_aref(m, i);
    sp_PolyArray_push(r, g ? sp_box_str(g) : sp_box_nil());
  }
  return r;
}
const char *sp_MatchData_pre_match(sp_MatchData *m) {
  if (!m) return sp_str_empty;
  int e = m->caps[0];
  if (e <= 0) return sp_str_empty;
  char *b = sp_str_alloc((size_t)e);
  memcpy(b, m->source, e); b[e] = 0; sp_str_set_len(b, (size_t)e);
  return b;
}
const char *sp_MatchData_post_match(sp_MatchData *m) {
  if (!m) return sp_str_empty;
  int s = m->caps[1];
  size_t sl = strlen(m->source);
  if (s < 0 || (size_t)s >= sl) return sp_str_empty;
  size_t len = sl - (size_t)s;
  char *b = sp_str_alloc(len);
  memcpy(b, m->source + s, len); b[len] = 0; sp_str_set_len(b, len);
  return b;
}
void sp_re_default_error_handler(const char *msg) {
  /* msg points at the regex compiler's stack buffer. sp_raise_cls stores
     the pointer and longjmps past that frame, leaving it dangling -- copy
     to a GC-managed string first (mirrors sp_re_startup_error_handler).
     gcc happened to leave the stack intact; clang reused it, so e.message
     read garbage (regexp_error_catchable). */
  if (msg) {
    size_t n = strlen(msg);
    char *buf = sp_str_alloc_raw(n + 1);
    memcpy(buf, msg, n);
    buf[n] = 0;
    msg = buf;
  }
  sp_raise_cls("RegexpError", msg);
}
