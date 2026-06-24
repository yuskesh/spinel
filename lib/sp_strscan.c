/*
 * sp_strscan.c — StringScanner for Spinel
 *
 * Implements the StringScanner methods: scan, check, scan_until,
 * matched, matched?, pos, eos?, getch, peek, unscan, rest,
 * terminate, pre_match, post_match, string. Uses spinel's
 * internal regex engine (re_compile / re_exec — see lib/regexp/)
 * rather than oniguruma; the existing lib/strscan.c is an
 * oniguruma-based draft the rest of the build doesn't link.
 *
 * Matching semantics: scan / check are anchored at the current
 * position (re_exec is run with start=pos and the result is
 * accepted only when caps[0] == pos). scan_until returns text
 * from pos through the end of the next match anywhere ahead.
 *
 * The scanner struct is GC-allocated via the sp_ext_* shim so
 * the lifetime tracks the main file's heap instead of leaking.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* String + object allocation, sp_mark_string, sp_raise_cls -- all shared now,
   so this TU allocates directly with no sp_ext_* shim. */
#include "sp_alloc.h"

/* Forward decl mirrors lib/regexp/re_internal.h's public API. */
typedef struct mrb_regexp_pattern mrb_regexp_pattern;
extern int re_exec(const mrb_regexp_pattern *pat, const char *str, int64_t len, int64_t start, int *captures, int captures_size);

/* The scanner struct lives in spinel's GC heap. `source` /
   `matched` are GC-tracked strings; the scan function below
   marks them so they survive collection. */
/* Capture slots stored from the last successful scan/check, so
   StringScanner#[] can return numbered groups. caps holds [start,end)
   byte offsets into `source`, group 0 first; unmatched groups are -1.
   ncaps is the number of valid ints (2 per group, group 0 included). */
#define SP_SS_MAXCAP 20
typedef struct {
  const char *source;
  const char *matched;
  int64_t     pos;
  int64_t     last_pos;
  int         matched_p; /* int (not mrb_bool) — keep the layout
                            compact; FALSE=0, TRUE=1 */
  int         ncaps;
  int         caps[SP_SS_MAXCAP];
} sp_StringScanner;

static void sp_StringScanner_scan_gc(void *p) {
  sp_StringScanner *sc = (sp_StringScanner *)p;
  if (sc->source) sp_mark_string(sc->source);
  if (sc->matched) sp_mark_string(sc->matched);
}

sp_StringScanner *sp_StringScanner_new(const char *str) {
  sp_StringScanner *sc = (sp_StringScanner *)sp_gc_alloc(sizeof(sp_StringScanner), NULL, sp_StringScanner_scan_gc);
  sc->source = str ? str : sp_str_empty;
  sc->matched = sp_str_empty;
  sc->pos = 0;
  sc->last_pos = 0;
  sc->matched_p = 0;
  sc->ncaps = 0;
  return sc;
}

/* Clamp re_exec's returned ncap to the stored caps[] size. */
static int sc_clamp_ncaps(int n) {
  if (n < 0) return 0;
  if (n > SP_SS_MAXCAP) return SP_SS_MAXCAP;
  return n;
}

/* Anchored match: succeed only when re_exec finds a match
   starting at exactly `pos`. Returns matched length, or -1. */
static int64_t sc_match_at_pos(const mrb_regexp_pattern *pat, const char *str, int64_t slen, int64_t pos, int *caps, int *ncap_out) {
  int n = re_exec(pat, str, slen, pos, caps, SP_SS_MAXCAP);
  *ncap_out = sc_clamp_ncaps(n);
  if (n <= 0 || caps[0] != pos) return -1;
  return caps[1] - caps[0];
}

/* Forward-search match: returns offset of first match at or
   after pos, and writes matched length to *mlen. -1 on miss. */
static int64_t sc_match_forward(const mrb_regexp_pattern *pat, const char *str, int64_t slen, int64_t pos, int64_t *mlen, int *caps, int *ncap_out) {
  int n = re_exec(pat, str, slen, pos, caps, SP_SS_MAXCAP);
  *ncap_out = sc_clamp_ncaps(n);
  if (n <= 0 || caps[0] < 0) return -1;
  *mlen = caps[1] - caps[0];
  return caps[0];
}

static char *sc_substr(const char *src, int64_t start, int64_t len) {
  char *out = sp_str_alloc((size_t)len);
  memcpy(out, src + start, (size_t)len);
  out[len] = 0;
  sp_str_set_len(out, (size_t)len);
  return out;
}

/* Byte length of the UTF-8 character starting at src[pos], clamped so a
   truncated or invalid lead byte near the end never reads past `len`.
   Mirrors sp_utf8_advance in sp_runtime.h, which isn't visible from here
   (this file includes only mruby_shim.h). */
static int64_t sc_char_len(const char *src, int64_t pos, int64_t len) {
  unsigned char c = (unsigned char)src[pos];
  int64_t n = 1;
  if (c >= 0xF0) n = 4;
  else if (c >= 0xE0) n = 3;
  else if (c >= 0xC0) n = 2;
  int64_t i = 1;
  while (i < n && pos + i < len && ((unsigned char)src[pos + i] & 0xC0) == 0x80) i++;
  return i;
}

const char *sp_StringScanner_scan(sp_StringScanner *sc, mrb_regexp_pattern *pat) {
  if (!sc || !pat) return NULL;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t mlen = sc_match_at_pos(pat, sc->source, slen, sc->pos, sc->caps, &sc->ncaps);
  if (mlen < 0) {
    sc->matched = sp_str_empty;
    sc->matched_p = 0;
    sc->ncaps = 0;
    return NULL;
  }
  char *m = sc_substr(sc->source, sc->pos, mlen);
  sc->last_pos = sc->pos;
  sc->matched = m;
  sc->matched_p = 1;
  sc->pos += mlen;
  return m;
}

const char *sp_StringScanner_check(sp_StringScanner *sc, mrb_regexp_pattern *pat) {
  if (!sc || !pat) return NULL;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t mlen = sc_match_at_pos(pat, sc->source, slen, sc->pos, sc->caps, &sc->ncaps);
  if (mlen < 0) {
    sc->matched = sp_str_empty;
    sc->matched_p = 0;
    sc->ncaps = 0;
    return NULL;
  }
  char *m = sc_substr(sc->source, sc->pos, mlen);
  sc->last_pos = sc->pos;
  sc->matched = m;
  sc->matched_p = 1;
  return m;
}

const char *sp_StringScanner_scan_until(sp_StringScanner *sc, mrb_regexp_pattern *pat) {
  if (!sc || !pat) return sp_str_empty;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t mlen = 0;
  int64_t mstart = sc_match_forward(pat, sc->source, slen, sc->pos, &mlen, sc->caps, &sc->ncaps);
  if (mstart < 0) {
    sc->matched = sp_str_empty;
    sc->matched_p = 0;
    sc->ncaps = 0;
    return NULL;
  }
  int64_t consumed = (mstart + mlen) - sc->pos;
  char *taken = sc_substr(sc->source, sc->pos, consumed);
  char *m = sc_substr(sc->source, mstart, mlen);
  sc->last_pos = sc->pos;
  sc->matched = m;
  sc->matched_p = 1;
  sc->pos = mstart + mlen;
  return taken;
}

/* StringScanner#[n] -- the nth capture group of the last scan/check.
   Group 0 is the whole match. Returns NULL (Ruby nil) when there was
   no match, the index is out of range, or the group didn't
   participate. */
const char *sp_StringScanner_aref(sp_StringScanner *sc, mrb_int n) {
  if (!sc || !sc->matched_p || n < 0) return NULL;
  int idx = (int)n;
  if (2 * idx + 1 >= sc->ncaps) return NULL;
  int st = sc->caps[2 * idx];
  int en = sc->caps[2 * idx + 1];
  if (st < 0 || en < st) return NULL;
  return sc_substr(sc->source, st, en - st);
}

const char *sp_StringScanner_matched(sp_StringScanner *sc) {
  if (!sc || !sc->matched_p) return NULL;
  return sc->matched ? sc->matched : NULL;
}

mrb_bool sp_StringScanner_matched_p(sp_StringScanner *sc) {
  if (!sc) return FALSE;
  return sc->matched_p ? TRUE : FALSE;
}

mrb_int sp_StringScanner_pos(sp_StringScanner *sc) {
  if (!sc) return 0;
  return (mrb_int)sc->pos;
}

mrb_int sp_StringScanner_pos_set(sp_StringScanner *sc, mrb_int p) {
  if (!sc) return 0;
  sc->pos = (int64_t)p;
  return p;
}

mrb_bool sp_StringScanner_eos_p(sp_StringScanner *sc) {
  if (!sc) return TRUE;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  return (sc->pos >= slen) ? TRUE : FALSE;
}

const char *sp_StringScanner_getch(sp_StringScanner *sc) {
  if (!sc) return sp_str_empty;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  if (sc->pos >= slen) {
    sc->matched = sp_str_empty;
    sc->matched_p = 0;
    return NULL;
  }
  int64_t clen = sc_char_len(sc->source, sc->pos, slen);
  char *c = sc_substr(sc->source, sc->pos, clen);
  sc->last_pos = sc->pos;
  sc->matched = c;
  sc->matched_p = 1;
  sc->pos += clen;
  return c;
}

const char *sp_StringScanner_peek(sp_StringScanner *sc, mrb_int n) {
  if (!sc) return sp_str_empty;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t avail = slen - sc->pos;
  int64_t take = n < avail ? n : avail;
  if (take < 0) take = 0;
  return sc_substr(sc->source, sc->pos, take);
}

sp_StringScanner *sp_StringScanner_unscan(sp_StringScanner *sc) {
  if (!sc) return sc;
  /* CRuby raises StringScanner::Error when there is no match record to
     rewind to: a double unscan, or unscan before any successful scan.
     Guard before mutating state so a failed unscan leaves pos intact. */
  if (!sc->matched_p) {
    sp_raise_cls("StringScanner_Error",
                 "unscan failed: previous match record not exist");
  }
  sc->pos = sc->last_pos;
  sc->matched = sp_str_empty;
  sc->matched_p = 0;
  return sc;
}

const char *sp_StringScanner_rest(sp_StringScanner *sc) {
  if (!sc) return sp_str_empty;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t rem = slen - sc->pos;
  if (rem <= 0) return sp_str_empty;
  return sc_substr(sc->source, sc->pos, rem);
}

mrb_int sp_StringScanner_rest_size(sp_StringScanner *sc) {
  if (!sc) return 0;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t rem = slen - sc->pos;
  return rem < 0 ? 0 : (mrb_int)rem;
}

mrb_bool sp_StringScanner_rest_p(sp_StringScanner *sc) {
  if (!sc) return FALSE;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  return (sc->pos < slen) ? TRUE : FALSE;
}

sp_StringScanner *sp_StringScanner_terminate(sp_StringScanner *sc) {
  if (!sc) return sc;
  sc->pos = (int64_t)sp_str_byte_len(sc->source);
  sc->matched = sp_str_empty;
  sc->matched_p = 0;
  return sc;
}

const char *sp_StringScanner_string(sp_StringScanner *sc) {
  if (!sc) return sp_str_empty;
  return sc->source ? sc->source : sp_str_empty;
}

const char *sp_StringScanner_pre_match(sp_StringScanner *sc) {
  if (!sc || !sc->matched_p) return NULL;
  if (sc->last_pos <= 0) return sp_str_empty;
  return sc_substr(sc->source, 0, sc->last_pos);
}

const char *sp_StringScanner_post_match(sp_StringScanner *sc) {
  if (!sc || !sc->matched_p) return NULL;
  int64_t mlen = (int64_t)sp_str_byte_len(sc->matched);
  int64_t start = sc->last_pos + mlen;
  int64_t slen = (int64_t)sp_str_byte_len(sc->source);
  int64_t rem = slen - start;
  if (rem <= 0) return sp_str_empty;
  return sc_substr(sc->source, start, rem);
}

sp_StringScanner *sp_StringScanner_reset(sp_StringScanner *sc) {
  if (!sc) return sc;
  sc->pos = 0;
  sc->last_pos = 0;
  sc->matched = sp_str_empty;
  sc->matched_p = 0;
  return sc;
}
