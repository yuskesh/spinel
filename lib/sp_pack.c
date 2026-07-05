/*
 * sp_pack.c — Array#pack / String#unpack for Spinel
 *
 * Implements the common Perl/Ruby pack format specifiers. Built
 * as a separate translation unit and linked into libspinel_rt.a;
 * it allocates GC-managed strings and PolyArrays directly through
 * the shared headers (sp_alloc.h), so no sp_ext_* shim is needed.
 *
 * Supported specifiers (initial):
 *   C / c   unsigned / signed 8-bit
 *   n / N   unsigned 16 / 32-bit big-endian
 *   v / V   unsigned 16 / 32-bit little-endian
 *   s / S   signed / unsigned 16-bit native
 *   l / L   signed / unsigned 32-bit native
 *   q / Q   signed / unsigned 64-bit native
 *   a       binary string (NUL-padded on pack)
 *   A       text string (space-padded on pack, space-trimmed on unpack)
 *   Z       NUL-terminated string
 *   x       null byte
 *
 * Counts:  `<spec>N` packs N items; `<spec>*` consumes all remaining.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* The shared runtime types (sp_RbVal, sp_IntArray, sp_sym, mrb_*, SP_TAG_*) and
   the string allocator (sp_str_alloc / _set_len / _byte_len / sp_str_empty) come
   straight from the shared headers: this separate TU can now allocate GC strings
   directly onto the one shared heap, so no sp_ext_str_* shim is needed. */
#include "sp_alloc.h"   /* string + object allocation, sp_box_*, sp_PolyArray */
#include "sp_str.h"     /* sp_nil_recv for the nil-receiver unpack raise */

/* ---------- Helpers ---------- */

static void pk_append(char **buf, size_t *len, size_t *cap, const char *src, size_t n) {
  if (*len + n + 1 > *cap) {
    size_t nc = (*len + n + 1) * 2;
    char *nb = (char *)realloc(*buf, nc);
    if (!nb) { perror("realloc"); exit(1); }
    *buf = nb;
    *cap = nc;
  }
  memcpy(*buf + *len, src, n);
  *len += n;
}

/* Encode codepoint `v` as UTF-8 (the `U` directive) into out[0..5].
   Mirrors CRuby's permissive scheme: up to 6 bytes for values through
   0x7FFFFFFF. Negative / out-of-range values encode nothing (CRuby
   raises RangeError; spinel has no exception path here). Returns the
   byte count written. */
static int pk_utf8(unsigned char *out, int64_t v) {
  if (v < 0) return 0;
  uint64_t uv = (uint64_t)v;
  if (uv <= 0x7F) {
    out[0] = (unsigned char)uv;
    return 1;
  }
  if (uv <= 0x7FF) {
    out[0] = (unsigned char)(0xC0 | (uv >> 6));
    out[1] = (unsigned char)(0x80 | (uv & 0x3F));
    return 2;
  }
  if (uv <= 0xFFFF) {
    out[0] = (unsigned char)(0xE0 | (uv >> 12));
    out[1] = (unsigned char)(0x80 | ((uv >> 6) & 0x3F));
    out[2] = (unsigned char)(0x80 | (uv & 0x3F));
    return 3;
  }
  if (uv <= 0x1FFFFF) {
    out[0] = (unsigned char)(0xF0 | (uv >> 18));
    out[1] = (unsigned char)(0x80 | ((uv >> 12) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((uv >> 6) & 0x3F));
    out[3] = (unsigned char)(0x80 | (uv & 0x3F));
    return 4;
  }
  if (uv <= 0x3FFFFFF) {
    out[0] = (unsigned char)(0xF8 | (uv >> 24));
    out[1] = (unsigned char)(0x80 | ((uv >> 18) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((uv >> 12) & 0x3F));
    out[3] = (unsigned char)(0x80 | ((uv >> 6) & 0x3F));
    out[4] = (unsigned char)(0x80 | (uv & 0x3F));
    return 5;
  }
  if (uv <= 0x7FFFFFFF) {
    out[0] = (unsigned char)(0xFC | (uv >> 30));
    out[1] = (unsigned char)(0x80 | ((uv >> 24) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((uv >> 18) & 0x3F));
    out[3] = (unsigned char)(0x80 | ((uv >> 12) & 0x3F));
    out[4] = (unsigned char)(0x80 | ((uv >> 6) & 0x3F));
    out[5] = (unsigned char)(0x80 | (uv & 0x3F));
    return 6;
  }
  return 0;
}

/* Consume any '<' '>' '!' '_' modifiers after a directive letter, then the
   optional count. Without this, `pack('l<l<l<l<')` treated each '<' as its
   own directive -- and since the element cursor advances per directive, every
   other value was silently dropped (doom's SDL_Rect bytes came out as
   {x,w,0,0}). '!'/'_' pick the native size (what the plain directive
   already is here); '<'/'>' set *big. CRuby raises RangeError on a
   repeated endian modifier ("l><"); spinel's pack has no exception
   path, so the last one wins instead. */
static int64_t pk_parse_count_mods(const char **pp, int *big) {
  const char *p = *pp;
  while (*p == '<' || *p == '>' || *p == '!' || *p == '_') {
    if (big) {
      if (*p == '>') *big = 1;
      else if (*p == '<') *big = 0;
    }
    p++;
  }
  if (*p == '*') { *pp = p + 1; return -1; }
  if (*p < '0' || *p > '9') { *pp = p; return 1; }
  int64_t n = 0;
  while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
  *pp = p;
  return n;
}
static int64_t pk_parse_count(const char **pp) {
  return pk_parse_count_mods(pp, NULL);
}
/* Serialize / deserialize an integer as `n` bytes, little- or
   big-endian, via shifts (host-endianness independent, like the
   n/N/v/V paths). */
static void pk_put_int(char *out, int64_t v, size_t n, int big) {
  uint64_t uv = (uint64_t)v;
  for (size_t i = 0; i < n; i++) {
    size_t sh = big ? (n - 1 - i) : i;
    out[i] = (char)((uv >> (8 * sh)) & 0xff);
  }
}
static uint64_t pk_get_int(const unsigned char *u, size_t n, int big) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    size_t sh = big ? (n - 1 - i) : i;
    v |= (uint64_t)u[i] << (8 * sh);
  }
  return v;
}

static int64_t pk_poly_to_int(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_INT:  return v.v.i;
    case SP_TAG_BOOL: return v.v.i ? 1 : 0;
    case SP_TAG_FLT:  return (int64_t)v.v.f;
    case SP_TAG_STR:  return v.v.s ? strtoll(v.v.s, NULL, 0) : 0;
    case SP_TAG_NIL:  return 0;
    default:          return 0;
  }
}

static const char *pk_poly_to_str(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_STR: return v.v.s ? v.v.s : "";
    case SP_TAG_NIL: return "";
    default:         return "";
  }
}


/* ---------- Pack entry points ---------- */

const char *sp_IntArray_pack(sp_IntArray *arr, const char *fmt) {
  if (!arr || !fmt) return sp_str_empty;
  size_t cap = 64;
  char *buf = (char *)malloc(cap);
  if (!buf) { perror("malloc"); exit(1); }
  size_t len = 0;
  mrb_int idx = 0;
  const char *p = fmt;
  while (*p) {
    char spec = *p++;
    if (spec == ' ' || spec == '\t' || spec == '\n') continue;
    int big = 0;
    int64_t count = pk_parse_count_mods(&p, &big);
    if (count < 0) count = arr->len - idx;
    if (count < 0) count = 0;
    for (int64_t k = 0; k < count; k++) {
      int64_t v = (idx < arr->len) ? arr->data[arr->start + idx] : 0;
      idx++;
      char tmp[8];
      switch (spec) {
        case 'C': case 'c':
          tmp[0] = (char)(v & 0xff);
          pk_append(&buf, &len, &cap, tmp, 1);
          break;
        case 'n':
          tmp[0] = (char)((v >> 8) & 0xff); tmp[1] = (char)(v & 0xff);
          pk_append(&buf, &len, &cap, tmp, 2);
          break;
        case 'N':
          tmp[0] = (char)((v >> 24) & 0xff); tmp[1] = (char)((v >> 16) & 0xff);
          tmp[2] = (char)((v >> 8) & 0xff);  tmp[3] = (char)(v & 0xff);
          pk_append(&buf, &len, &cap, tmp, 4);
          break;
        case 'v':
          tmp[0] = (char)(v & 0xff); tmp[1] = (char)((v >> 8) & 0xff);
          pk_append(&buf, &len, &cap, tmp, 2);
          break;
        case 'V':
          tmp[0] = (char)(v & 0xff);        tmp[1] = (char)((v >> 8) & 0xff);
          tmp[2] = (char)((v >> 16) & 0xff); tmp[3] = (char)((v >> 24) & 0xff);
          pk_append(&buf, &len, &cap, tmp, 4);
          break;
        case 's': case 'S':
          pk_put_int(tmp, v, 2, big);
          pk_append(&buf, &len, &cap, tmp, 2);
          break;
        case 'l': case 'L':
          pk_put_int(tmp, v, 4, big);
          pk_append(&buf, &len, &cap, tmp, 4);
          break;
        case 'q': case 'Q':
          pk_put_int(tmp, v, 8, big);
          pk_append(&buf, &len, &cap, tmp, 8);
          break;
        case 'x':
          tmp[0] = 0;
          pk_append(&buf, &len, &cap, tmp, 1);
          idx--;
          break;
        case 'U': {
          unsigned char ub[6];
          int ulen = pk_utf8(ub, v);
          pk_append(&buf, &len, &cap, (const char *)ub, (size_t)ulen);
          break;
        }
        default:
          break;
      }
    }
  }
  /* Hand back via GC-tracked sp_str_alloc so the main file's GC
     can free the buffer. */
  char *r = sp_str_alloc(len);
  memcpy(r, buf, len);
  sp_str_set_len(r, len);
  free(buf);
  return r;
}

const char *sp_PolyArray_pack(sp_PolyArray *arr, const char *fmt) {
  if (!arr || !fmt) return sp_str_empty;
  size_t cap = 64;
  char *buf = (char *)malloc(cap);
  if (!buf) { perror("malloc"); exit(1); }
  size_t len = 0;
  mrb_int idx = 0;
  const char *p = fmt;
  while (*p) {
    char spec = *p++;
    if (spec == ' ' || spec == '\t' || spec == '\n') continue;
    int big = 0;
    int64_t count = pk_parse_count_mods(&p, &big);
    if (spec == 'a' || spec == 'A' || spec == 'Z') {
      const char *s = (idx < arr->len) ? pk_poly_to_str(arr->data[idx]) : "";
      idx++;
      size_t sl = strlen(s);
      size_t want = (count < 0) ? sl : (size_t)count;
      if (spec == 'Z' && count < 0) want = sl + 1;
      size_t take = sl < want ? sl : want;
      pk_append(&buf, &len, &cap, s, take);
      if (take < want) {
        char pad = (spec == 'A') ? ' ' : 0;
        for (size_t pi = 0; pi < want - take; pi++) pk_append(&buf, &len, &cap, &pad, 1);
      }
      continue;
    }
    if (count < 0) count = arr->len - idx;
    if (count < 0) count = 0;
    for (int64_t k = 0; k < count; k++) {
      int64_t v = (idx < arr->len) ? pk_poly_to_int(arr->data[idx]) : 0;
      idx++;
      char tmp[8];
      switch (spec) {
        case 'C': case 'c':
          tmp[0] = (char)(v & 0xff);
          pk_append(&buf, &len, &cap, tmp, 1);
          break;
        case 'n':
          tmp[0] = (char)((v >> 8) & 0xff); tmp[1] = (char)(v & 0xff);
          pk_append(&buf, &len, &cap, tmp, 2);
          break;
        case 'N':
          tmp[0] = (char)((v >> 24) & 0xff); tmp[1] = (char)((v >> 16) & 0xff);
          tmp[2] = (char)((v >> 8) & 0xff);  tmp[3] = (char)(v & 0xff);
          pk_append(&buf, &len, &cap, tmp, 4);
          break;
        case 'v':
          tmp[0] = (char)(v & 0xff); tmp[1] = (char)((v >> 8) & 0xff);
          pk_append(&buf, &len, &cap, tmp, 2);
          break;
        case 'V':
          tmp[0] = (char)(v & 0xff);        tmp[1] = (char)((v >> 8) & 0xff);
          tmp[2] = (char)((v >> 16) & 0xff); tmp[3] = (char)((v >> 24) & 0xff);
          pk_append(&buf, &len, &cap, tmp, 4);
          break;
        case 's': case 'S':
          pk_put_int(tmp, v, 2, big);
          pk_append(&buf, &len, &cap, tmp, 2);
          break;
        case 'l': case 'L':
          pk_put_int(tmp, v, 4, big);
          pk_append(&buf, &len, &cap, tmp, 4);
          break;
        case 'q': case 'Q':
          pk_put_int(tmp, v, 8, big);
          pk_append(&buf, &len, &cap, tmp, 8);
          break;
        case 'x':
          tmp[0] = 0;
          pk_append(&buf, &len, &cap, tmp, 1);
          idx--;
          break;
        case 'U': {
          unsigned char ub[6];
          int ulen = pk_utf8(ub, v);
          pk_append(&buf, &len, &cap, (const char *)ub, (size_t)ulen);
          break;
        }
        default:
          break;
      }
    }
  }
  char *r = sp_str_alloc(len);
  memcpy(r, buf, len);
  sp_str_set_len(r, len);
  free(buf);
  return r;
}

/* ---------- Unpack entry point ---------- */

sp_PolyArray *sp_str_unpack(const char *str, const char *fmt) {
  if (!str) sp_nil_recv("unpack");
  /* Root the source string across every allocation below: `str` is very often a
     fresh, otherwise-unrooted substring (`data[4, 4].unpack1('V')` in doom's
     binary WAD parsing). sp_PolyArray_new and the per-element boxes/pushes can
     each trigger a GC that would sweep the input out from under us (a
     use-after-free read of the freed bytes). Root the result array too. */
  SP_GC_ROOT_STR(str);
  /* `fmt` is walked (via `p`) across the very same allocations, so it is just as
     exposed as `str`: a fresh, unrooted format string (e.g. `unpack1("V" * n)`)
     could be swept mid-parse, leaving `p` dangling. Root it too. NUL-safe. */
  SP_GC_ROOT_STR(fmt);
  sp_PolyArray *out = sp_PolyArray_new();
  SP_GC_ROOT(out);
  if (!str || !fmt) return out;
  /* sp_ext_str_byte_len honors the heap-string header so embedded
     NULs (binary data) don't truncate the source. */
  size_t slen = sp_str_byte_len(str);
  size_t off = 0;
  const char *p = fmt;
  while (*p) {
    char spec = *p++;
    if (spec == ' ' || spec == '\t' || spec == '\n') continue;
    int big = 0;
    int64_t count = pk_parse_count_mods(&p, &big);
    size_t fsize = 0;
    switch (spec) {
      case 'C': case 'c': case 'x': fsize = 1; break;
      case 'n': case 'v': case 's': case 'S': fsize = 2; break;
      case 'N': case 'V': case 'l': case 'L': fsize = 4; break;
      case 'q': case 'Q': fsize = 8; break;
      default: fsize = 0; break;
    }
    if (spec == 'a' || spec == 'A' || spec == 'Z') {
      size_t take;
      if (count < 0) {
        take = slen - off;
      }
else {
        take = (size_t)count;
        if (off + take > slen) take = slen - off;
      }
      const char *src = str + off;
      if (spec == 'Z' && count < 0) {
        size_t z = 0;
        while (off + z < slen && src[z]) z++;
        char *s = sp_str_alloc(z);
        memcpy(s, src, z); s[z] = 0; sp_str_set_len(s, z);
        sp_PolyArray_push(out, sp_box_str(s));
        off += z;
        if (off < slen && str[off] == 0) off++;
      }
else {
        char *s = sp_str_alloc(take);
        memcpy(s, src, take); s[take] = 0;
        size_t real = take;
        if (spec == 'A') {
          while (real > 0 && (s[real - 1] == ' ' || s[real - 1] == 0)) real--;
          s[real] = 0;
        }
else if (spec == 'Z') {
          size_t z = 0;
          while (z < take && s[z]) z++;
          s[z] = 0;
          real = z;
        }
        sp_str_set_len(s, real);
        sp_PolyArray_push(out, sp_box_str(s));
        off += take;
      }
      continue;
    }
    if (fsize == 0) continue;
    if (count < 0) count = (slen - off) / fsize;
    for (int64_t k = 0; k < count; k++) {
      if (off + fsize > slen) break;
      int64_t v = 0;
      const unsigned char *u = (const unsigned char *)(str + off);
      switch (spec) {
        case 'C': v = u[0]; break;
        case 'c': v = (int8_t)u[0]; break;
        case 'n': v = ((int64_t)u[0] << 8) | u[1]; break;
        case 'N': v = ((int64_t)u[0] << 24) | ((int64_t)u[1] << 16) | ((int64_t)u[2] << 8) | u[3]; break;
        case 'v': v = ((int64_t)u[1] << 8) | u[0]; break;
        case 'V': v = ((int64_t)u[3] << 24) | ((int64_t)u[2] << 16) | ((int64_t)u[1] << 8) | u[0]; break;
        case 's': v = (int16_t)pk_get_int(u, 2, big); break;
        case 'S': v = (uint16_t)pk_get_int(u, 2, big); break;
        case 'l': v = (int32_t)pk_get_int(u, 4, big); break;
        case 'L': v = (uint32_t)pk_get_int(u, 4, big); break;
        case 'q': v = (int64_t)pk_get_int(u, 8, big); break;
        case 'Q': v = (int64_t)pk_get_int(u, 8, big); break;
        case 'x': break;
      }
      off += fsize;
      if (spec != 'x') sp_PolyArray_push(out, sp_box_int((mrb_int)v));
    }
  }
  return out;
}
