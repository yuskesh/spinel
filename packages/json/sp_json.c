/* sp_json.c -- JSON.generate serialization, split out of sp_runtime.h.

   This is a standalone translation unit: it owns no typed-array/hash structs.
   It reaches the generated program's containers only through the generic hooks
   in sp_gc.h (sp_json_kind/len/aref/hpair + sp_sym_name_fn), which the generated
   TU installs at startup. Result strings are built in an off-heap scratch buffer
   and finalized onto the shared GC string heap (sp_alloc.h), so a nested
   allocation can't free a piece already copied in. */
#include "spinel/runtime.h"  /* sp_RbVal, SP_TAG_*, hooks, sp_str_alloc, sp_int_to_s, sp_float_to_s */
#include "sp_json.h"          /* this package's sp_json_str / sp_json_val API */
#include <string.h>
#include <stdlib.h>           /* strtoll, strtod */

/* A 0xff-marked rodata literal, so sp_str_byte_len reads its length correctly
   (matches sp_runtime.h's SPL). Used for the fixed tokens true/false/null. */
#define JSPL(s) (&("\xff" s)[1])

/* Off-GC-heap growable buffer; finalized into a GC string at the end. */
typedef struct { char *p; size_t len, cap; } jbuf;
static void jb_add(jbuf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    b->cap = (b->len + n + 1) * 2;
    b->p = (char *)realloc(b->p, b->cap);
    if (!b->p) sp_oom_die();
  }
  memcpy(b->p + b->len, s, n);
  b->len += n;
}
static void jb_c(jbuf *b, char c) { jb_add(b, &c, 1); }
static const char *jb_finish(jbuf *b) {
  char *r = sp_str_alloc(b->len);
  if (b->len) memcpy(r, b->p, b->len);
  sp_str_set_len(r, b->len);
  free(b->p);
  return r;
}

const char *sp_json_str(const char *s) {
  jbuf b; memset(&b, 0, sizeof b);
  jb_c(&b, '"');
  if (s) {
    for (const char *p = s; *p; p++) {
      unsigned char c = (unsigned char)*p;
      if (c == '"') jb_add(&b, "\\\"", 2);
      else if (c == '\\') jb_add(&b, "\\\\", 2);
      else if (c == '\n') jb_add(&b, "\\n", 2);
      else if (c == '\t') jb_add(&b, "\\t", 2);
      else if (c == '\r') jb_add(&b, "\\r", 2);
      else if (c == '\b') jb_add(&b, "\\b", 2);
      else if (c == '\f') jb_add(&b, "\\f", 2);
      else if (c < 0x20) { char u[8]; int n = snprintf(u, sizeof u, "\\u%04x", (unsigned)c); jb_add(&b, u, (size_t)n); }
      else jb_c(&b, (char)c);
    }
  }
  jb_c(&b, '"');
  return jb_finish(&b);
}

static const char *sp_json_key(sp_RbVal k) {
  if (k.tag == SP_TAG_STR)  return sp_json_str(k.v.s);
  if (k.tag == SP_TAG_SYM)  return sp_json_str(sp_sym_name_fn ? sp_sym_name_fn((sp_sym)k.v.i) : "");
  if (k.tag == SP_TAG_INT)  return sp_json_str(sp_int_to_s(k.v.i));
  if (k.tag == SP_TAG_BOOL) return sp_json_str(k.v.b ? "true" : "false");
  return sp_json_str("");
}

const char *sp_json_val(sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_INT:  return sp_int_to_s(v.v.i);
    case SP_TAG_FLT:  return sp_float_to_s(v.v.f);
    case SP_TAG_BOOL: return v.v.b ? JSPL("true") : JSPL("false");
    case SP_TAG_NIL:  return JSPL("null");
    case SP_TAG_STR:  return sp_json_str(v.v.s);
    case SP_TAG_SYM:  return sp_json_str(sp_sym_name_fn ? sp_sym_name_fn((sp_sym)v.v.i) : "");
    case SP_TAG_OBJ: {
      int kind = sp_json_kind_fn ? sp_json_kind_fn(v) : 0;
      if (kind == 1) {  /* array */
        mrb_int n = sp_json_len_fn(v);
        jbuf b; memset(&b, 0, sizeof b); jb_c(&b, '[');
        for (mrb_int i = 0; i < n; i++) {
          if (i) jb_c(&b, ',');
          const char *e = sp_json_val(sp_json_aref_fn(v, i));
          jb_add(&b, e, strlen(e));
        }
        jb_c(&b, ']');
        return jb_finish(&b);
      }
      if (kind == 2) {  /* hash */
        mrb_int n = sp_json_len_fn(v);
        jbuf b; memset(&b, 0, sizeof b); jb_c(&b, '{');
        for (mrb_int i = 0; i < n; i++) {
          if (i) jb_c(&b, ',');
          sp_RbVal k, val;
          sp_json_hpair_fn(v, i, &k, &val);
          const char *ks = sp_json_key(k);
          jb_add(&b, ks, strlen(ks));
          jb_c(&b, ':');
          const char *vs = sp_json_val(val);
          jb_add(&b, vs, strlen(vs));
        }
        jb_c(&b, '}');
        return jb_finish(&b);
      }
      /* a plain object (Struct/Data): reflect it into a hash of its members
         (the generated program installs sp_obj_to_hash when it has Structs)
         and serialize that -- reusing the hash path above. No object-format
         knowledge lives here or in the compiler; only the generic reflection. */
      if (sp_obj_to_hash_fn) return sp_json_val(sp_obj_to_hash_fn(v));
      return JSPL("null");
    }
    default: return JSPL("null");
  }
}

/* ---------- JSON.parse ----------
   A recursive-descent parser producing boxed poly values: scalars and arrays
   are built directly from the package ABI (sp_box_*, sp_PolyArray); objects use
   the installed hash-builder hooks (the generated TU owns the hash type). Each
   in-progress container is GC-rooted so a nested allocation can't collect it. */

#define JP_MAX_DEPTH 200

typedef struct { const char *p, *end; } jrd;

static __attribute__((noreturn)) void jp_err(const char *msg) {
  sp_raise_cls("JSON::ParserError", msg);
}
static void jp_ws(jrd *j) {
  while (j->p < j->end) {
    char c = *j->p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') j->p++;
    else break;
  }
}
static sp_RbVal jp_value(jrd *j, int depth);

static void jp_hex4(jrd *j, jbuf *b, unsigned *out) {
  if (j->end - j->p < 4) { free(b->p); jp_err("incomplete \\u escape"); }
  unsigned cp = 0;
  for (int i = 0; i < 4; i++) {
    char h = *j->p++;
    cp <<= 4;
    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
    else { free(b->p); jp_err("invalid \\u hex digit"); }
  }
  *out = cp;
}
static void jp_utf8(jbuf *b, unsigned cp) {
  if (cp < 0x80) jb_c(b, (char)cp);
  else if (cp < 0x800) { jb_c(b, (char)(0xC0 | (cp >> 6))); jb_c(b, (char)(0x80 | (cp & 0x3F))); }
  else if (cp < 0x10000) { jb_c(b, (char)(0xE0 | (cp >> 12))); jb_c(b, (char)(0x80 | ((cp >> 6) & 0x3F))); jb_c(b, (char)(0x80 | (cp & 0x3F))); }
  else { jb_c(b, (char)(0xF0 | (cp >> 18))); jb_c(b, (char)(0x80 | ((cp >> 12) & 0x3F))); jb_c(b, (char)(0x80 | ((cp >> 6) & 0x3F))); jb_c(b, (char)(0x80 | (cp & 0x3F))); }
}
/* Parse a "..." string (cursor at the opening quote) into a GC string. */
static const char *jp_string(jrd *j) {
  if (j->p >= j->end || *j->p != '"') jp_err("expected a string");
  j->p++;
  jbuf b; memset(&b, 0, sizeof b);
  while (j->p < j->end && *j->p != '"') {
    char c = *j->p++;
    /* RFC 8259 s7: control chars U+0000..U+001F must be escaped */
    if ((unsigned char)c < 0x20) { free(b.p); jp_err("unescaped control character in string"); }
    if (c != '\\') { jb_c(&b, c); continue; }
    if (j->p >= j->end) { free(b.p); jp_err("unterminated escape"); }
    char e = *j->p++;
    switch (e) {
      case '"': jb_c(&b, '"'); break;   case '\\': jb_c(&b, '\\'); break;
      case '/': jb_c(&b, '/'); break;   case 'n': jb_c(&b, '\n'); break;
      case 't': jb_c(&b, '\t'); break;  case 'r': jb_c(&b, '\r'); break;
      case 'b': jb_c(&b, '\b'); break;  case 'f': jb_c(&b, '\f'); break;
      case 'u': {
        unsigned cp; jp_hex4(j, &b, &cp);
        if (cp >= 0xD800 && cp <= 0xDBFF && j->end - j->p >= 2 && j->p[0] == '\\' && j->p[1] == 'u') {
          j->p += 2; unsigned lo; jp_hex4(j, &b, &lo);
          if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          else jp_utf8(&b, cp), cp = lo;   /* lone high surrogate: emit both raw */
        }
        jp_utf8(&b, cp);
        break;
      }
      default: free(b.p); jp_err("invalid escape");
    }
  }
  if (j->p >= j->end) { free(b.p); jp_err("unterminated string"); }
  j->p++;  /* closing quote */
  return jb_finish(&b);
}
static sp_RbVal jp_number(jrd *j) {
  const char *start = j->p;
  int is_float = 0;
  if (j->p < j->end && *j->p == '-') j->p++;
  /* integer part (RFC 8259 s6): a lone 0, or 1-9 then digits -- no leading zeros */
  if (j->p < j->end && *j->p == '0') {
    j->p++;
    if (j->p < j->end && *j->p >= '0' && *j->p <= '9') jp_err("leading zero in number");
  } else {
    const char *ds = j->p;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    if (j->p == ds) jp_err("expected a digit in number");
  }
  /* fraction: '.' then at least one digit */
  if (j->p < j->end && *j->p == '.') {
    is_float = 1; j->p++;
    const char *fs = j->p;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    if (j->p == fs) jp_err("expected a digit after '.'");
  }
  /* exponent: e/E, optional sign, then at least one digit */
  if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
    is_float = 1; j->p++;
    if (j->p < j->end && (*j->p == '+' || *j->p == '-')) j->p++;
    const char *es = j->p;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    if (j->p == es) jp_err("expected a digit in exponent");
  }
  size_t n = (size_t)(j->p - start);
  char tmp[64];
  if (n == 0 || n >= sizeof tmp) jp_err("invalid number");
  memcpy(tmp, start, n); tmp[n] = 0;
  if (is_float) return sp_box_float(strtod(tmp, NULL));
  return sp_box_int((mrb_int)strtoll(tmp, NULL, 10));
}
static sp_RbVal jp_array(jrd *j, int depth) {
  j->p++;  /* '[' */
  sp_RbVal box = sp_box_poly_array(sp_PolyArray_new());
  SP_GC_ROOT_RBVAL(box);
  jp_ws(j);
  if (j->p < j->end && *j->p == ']') { j->p++; return box; }
  for (;;) {
    sp_RbVal v = jp_value(j, depth + 1);
    sp_PolyArray_push((sp_PolyArray *)box.v.p, v);
    jp_ws(j);
    if (j->p >= j->end) jp_err("unterminated array");
    if (*j->p == ',') { j->p++; continue; }
    if (*j->p == ']') { j->p++; break; }
    jp_err("expected ',' or ']' in array");
  }
  return box;
}
static sp_RbVal jp_object(jrd *j, int depth) {
  j->p++;  /* '{' */
  sp_RbVal box = sp_json_mk_hash_fn();
  SP_GC_ROOT_RBVAL(box);
  jp_ws(j);
  if (j->p < j->end && *j->p == '}') { j->p++; return box; }
  for (;;) {
    jp_ws(j);
    const char *key = jp_string(j);
    SP_GC_ROOT_STR(key);   /* survive the value parse; the hash stores the ptr */
    jp_ws(j);
    if (j->p >= j->end || *j->p != ':') jp_err("expected ':' in object");
    j->p++;
    sp_RbVal v = jp_value(j, depth + 1);
    sp_json_hash_set_fn(box, key, v);
    jp_ws(j);
    if (j->p >= j->end) jp_err("unterminated object");
    if (*j->p == ',') { j->p++; continue; }
    if (*j->p == '}') { j->p++; break; }
    jp_err("expected ',' or '}' in object");
  }
  return box;
}
static sp_RbVal jp_value(jrd *j, int depth) {
  if (depth > JP_MAX_DEPTH) jp_err("nesting too deep");
  jp_ws(j);
  if (j->p >= j->end) jp_err("unexpected end of input");
  char c = *j->p;
  if (c == '{') return jp_object(j, depth);
  if (c == '[') return jp_array(j, depth);
  if (c == '"') return sp_box_str(jp_string(j));
  if (c == 't') { if (j->end - j->p >= 4 && !memcmp(j->p, "true", 4))  { j->p += 4; return sp_box_bool(1); } jp_err("expected 'true'"); }
  if (c == 'f') { if (j->end - j->p >= 5 && !memcmp(j->p, "false", 5)) { j->p += 5; return sp_box_bool(0); } jp_err("expected 'false'"); }
  if (c == 'n') { if (j->end - j->p >= 4 && !memcmp(j->p, "null", 4))  { j->p += 4; return sp_box_nil(); } jp_err("expected 'null'"); }
  if (c == '-' || (c >= '0' && c <= '9')) return jp_number(j);
  jp_err("unexpected character");
}

sp_RbVal sp_json_parse(const char *s) {
  jrd j;
  j.p = s ? s : "";
  j.end = j.p + (s ? sp_str_byte_len(s) : 0);
  sp_RbVal v = jp_value(&j, 0);
  jp_ws(&j);
  if (j.p != j.end) jp_err("unexpected trailing characters");
  return v;
}
