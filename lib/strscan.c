/*
 * strscan.c — StringScanner C implementation for Spinel
 *
 * Provides sp_StringScanner_* functions that the generated C code calls.
 * Requires oniguruma: link with -lonig
 *
 * Usage: cc -O2 app.c lib/strscan.c -lonig -lm -o app
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <oniguruma.h>

typedef struct {
    const char *source;  /* original string (not owned) */
    int64_t source_len;
    int64_t pos;         /* current scan position */
    int64_t last_pos;    /* position before last scan (for unscan) */
    char *matched;       /* last matched string (heap-allocated) */
    int64_t matched_len;
    /* Issue #813: cache the peek buffer so repeated peek() calls
       don't leak. Reallocated on demand when n grows. */
    char *peek_buf;
    int64_t peek_buf_cap;
} sp_StringScanner;

/* Constructor.
   Issue #811: NULL str used to segfault at strlen(NULL); treat as
   the empty string. CRuby raises TypeError -- spinel can't easily
   raise from a typed-string constructor here, so the safer fallback
   is empty-string behaviour. */
sp_StringScanner *sp_StringScanner_new(const char *str) {
    if (!str) str = "";
    sp_StringScanner *sc = (sp_StringScanner *)calloc(1, sizeof(sp_StringScanner));
    if (!sc) return NULL;
    sc->source = str;
    sc->source_len = (int64_t)strlen(str);
    sc->pos = 0;
    sc->last_pos = 0;
    sc->matched = NULL;
    sc->matched_len = 0;
    return sc;
}

/* Internal: set matched string */
static void sc_set_matched(sp_StringScanner *sc, const char *start, int64_t len) {
    free(sc->matched);
    sc->matched = (char *)malloc(len + 1);
    memcpy(sc->matched, start, len);
    sc->matched[len] = '\0';
    sc->matched_len = len;
}

/* scan(pattern) — match at current position, advance if matched */
const char *sp_StringScanner_scan(sp_StringScanner *sc, regex_t *re) {
    if (sc->pos >= sc->source_len) return NULL;

    OnigRegion *region = onig_region_new();
    const OnigUChar *str = (const OnigUChar *)sc->source;
    const OnigUChar *end = str + sc->source_len;
    const OnigUChar *start = str + sc->pos;

    /* Match must start at current position */
    int r = onig_match(re, str, end, start, region, ONIG_OPTION_NONE);

    if (r >= 0) {
        sc->last_pos = sc->pos;
        int64_t match_len = (int64_t)r;
        sc_set_matched(sc, sc->source + sc->pos, match_len);
        sc->pos += match_len;
        onig_region_free(region, 1);
        return sc->matched;
    }

    onig_region_free(region, 1);
    free(sc->matched);
    sc->matched = NULL;
    sc->matched_len = 0;
    return NULL;
}

/* check(pattern) — match at current position, do NOT advance */
const char *sp_StringScanner_check(sp_StringScanner *sc, regex_t *re) {
    if (sc->pos >= sc->source_len) return NULL;

    OnigRegion *region = onig_region_new();
    const OnigUChar *str = (const OnigUChar *)sc->source;
    const OnigUChar *end = str + sc->source_len;
    const OnigUChar *start = str + sc->pos;

    int r = onig_match(re, str, end, start, region, ONIG_OPTION_NONE);

    if (r >= 0) {
        sc_set_matched(sc, sc->source + sc->pos, (int64_t)r);
        onig_region_free(region, 1);
        return sc->matched;
    }

    onig_region_free(region, 1);
    free(sc->matched);
    sc->matched = NULL;
    return NULL;
}

/* scan_until(pattern) — scan forward until pattern matches, return everything up to end of match */
const char *sp_StringScanner_scan_until(sp_StringScanner *sc, regex_t *re) {
    if (sc->pos >= sc->source_len) return NULL;

    OnigRegion *region = onig_region_new();
    const OnigUChar *str = (const OnigUChar *)sc->source;
    const OnigUChar *end = str + sc->source_len;
    const OnigUChar *start = str + sc->pos;

    int r = onig_search(re, str, end, start, end, region, ONIG_OPTION_NONE);

    if (r >= 0) {
        sc->last_pos = sc->pos;
        int64_t total_len = region->end[0] - sc->pos;
        sc_set_matched(sc, sc->source + sc->pos, total_len);
        sc->pos = region->end[0];
        onig_region_free(region, 1);
        return sc->matched;
    }

    onig_region_free(region, 1);
    return NULL;
}

/* matched — return last matched string */
const char *sp_StringScanner_matched(sp_StringScanner *sc) {
    return sc->matched ? sc->matched : "";
}

/* pos — current position */
int64_t sp_StringScanner_pos(sp_StringScanner *sc) {
    return sc->pos;
}

/* eos? — at end of string? */
int sp_StringScanner_eos_p(sp_StringScanner *sc) {
    return sc->pos >= sc->source_len;
}

/* getch — get one character and advance */
const char *sp_StringScanner_getch(sp_StringScanner *sc) {
    if (sc->pos >= sc->source_len) return NULL;
    sc->last_pos = sc->pos;
    char *ch = (char *)malloc(2);
    ch[0] = sc->source[sc->pos];
    ch[1] = '\0';
    free(sc->matched);
    sc->matched = ch;
    sc->matched_len = 1;
    sc->pos++;
    return sc->matched;
}

/* peek(n) — look ahead n characters without advancing.
   Issue #813: reuse the cached peek buffer across calls so a
   tight peek() loop doesn't leak one heap allocation per call.
   Grow the cap on demand. */
const char *sp_StringScanner_peek(sp_StringScanner *sc, int64_t n) {
    if (!sc || sc->pos >= sc->source_len) return "";
    int64_t remaining = sc->source_len - sc->pos;
    if (n > remaining) n = remaining;
    if (n < 0) n = 0;
    if (n + 1 > sc->peek_buf_cap) {
        int64_t nc = sc->peek_buf_cap ? sc->peek_buf_cap : 16;
        while (nc < n + 1) nc *= 2;
        char *nb = (char *)realloc(sc->peek_buf, (size_t)nc);
        if (!nb) return "";
        sc->peek_buf = nb;
        sc->peek_buf_cap = nc;
    }
    memcpy(sc->peek_buf, sc->source + sc->pos, n);
    sc->peek_buf[n] = '\0';
    return sc->peek_buf;
}

/* unscan — revert to position before last scan */
sp_StringScanner *sp_StringScanner_unscan(sp_StringScanner *sc) {
    sc->pos = sc->last_pos;
    return sc;
}

/* rest — remaining string from current position */
const char *sp_StringScanner_rest(sp_StringScanner *sc) {
    return sc->source + sc->pos;
}
