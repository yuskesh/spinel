/* sp_json.c -- JSON.generate serialization, split out of sp_runtime.h.

   This is a standalone translation unit: it owns no typed-array/hash structs.
   It reaches the generated program's containers only through the generic hooks
   in sp_gc.h (sp_json_kind/len/aref/hpair + sp_sym_name_fn), which the generated
   TU installs at startup. Result strings are built in an off-heap scratch buffer
   and finalized onto the shared GC string heap (sp_alloc.h), so a nested
   allocation can't free a piece already copied in. */
#include "sp_alloc.h"   /* sp_str_alloc, sp_int_to_s, sp_float_to_s */
#include "sp_json.h"    /* sp_gc.h: sp_RbVal, SP_TAG_*, the sp_json_* hooks */
#include <string.h>

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
      return JSPL("null");
    }
    default: return JSPL("null");
  }
}
