#ifndef SP_MARSHAL_H
#define SP_MARSHAL_H
/* lib/sp_marshal.h -- Marshal.dump / Marshal.load (Phase 1).
   A fragment #included into sp_runtime.h (not a standalone TU): it allocates
   GC strings/arrays/hashes and reads hash structs, all of which live in
   sp_runtime.h with per-TU static heap state, so it must compile in the same
   translation unit (same pattern as sp_time.h). */
/* ===== Marshal (Phase 1): nil/true/false/Integer/Float/String/Symbol + Array
   + Hash, in CRuby 4.8 wire format. Round-trip oriented: the dumper emits a
   valid CRuby stream (no symbol-link dedup) and the loader resolves CRuby
   symbol links. User objects, Bignum, cycles, and shared references are out of
   scope for this phase (they raise / are unsupported). ===== */
static sp_sym sp_sym_intern(const char *s);  /* generated per-TU; fwd decl */

typedef struct { char *p; size_t len, cap; } sp_mar_buf;
static void sp_mar_b(sp_mar_buf *b, unsigned char c) {
  if (b->len >= b->cap) { b->cap = b->cap ? b->cap * 2 : 64; b->p = (char *)realloc(b->p, b->cap); if (!b->p) sp_oom_die(); }
  b->p[b->len++] = (char)c;
}
static void sp_mar_raw(sp_mar_buf *b, const char *s, size_t n) { for (size_t i = 0; i < n; i++) sp_mar_b(b, (unsigned char)s[i]); }
/* CRuby's variable-length signed long encoding (the `long` primitive used for
   Fixnums and for every length/count in the stream). */
static void sp_mar_long(sp_mar_buf *b, long n) {
  if (n == 0) { sp_mar_b(b, 0); return; }
  if (0 < n && n < 123) { sp_mar_b(b, (unsigned char)(n + 5)); return; }
  if (-124 < n && n < 0) { sp_mar_b(b, (unsigned char)((n - 5) & 0xff)); return; }
  unsigned char buf[1 + sizeof(long)];
  long v = n; int i;
  for (i = 1; i <= (int)sizeof(long); i++) {
    buf[i] = (unsigned char)(v & 0xff);
    v >>= 8;
    if (v == 0) { buf[0] = (unsigned char)i; break; }
    if (v == -1) { buf[0] = (unsigned char)(256 - i); break; }
  }
  sp_mar_raw(b, (char *)buf, (size_t)i + 1);
}
static void sp_mar_bytes(sp_mar_buf *b, const char *s, size_t n) { sp_mar_long(b, (long)n); sp_mar_raw(b, s, n); }
static void sp_mar_sym(sp_mar_buf *b, const char *name) { sp_mar_b(b, ':'); sp_mar_bytes(b, name, strlen(name)); }
static void sp_mar_w(sp_mar_buf *b, sp_RbVal v);
static void sp_mar_w_hash(sp_mar_buf *b, sp_RbVal v) {
  sp_mar_b(b, '{');
  sp_mar_long(b, sp_poly_length(v));
  switch (v.cls_id) {
    case SP_BUILTIN_STR_INT_HASH: { sp_StrIntHash *h=(sp_StrIntHash*)v.v.p; for(mrb_int i=0;i<h->len;i++){ sp_mar_w(b, sp_box_str(h->order[i])); sp_mar_w(b, sp_box_int(sp_StrIntHash_get(h,h->order[i]))); } break; }
    case SP_BUILTIN_STR_STR_HASH: { sp_StrStrHash *h=(sp_StrStrHash*)v.v.p; for(mrb_int i=0;i<h->len;i++){ sp_mar_w(b, sp_box_str(h->order[i])); sp_mar_w(b, sp_box_str(sp_StrStrHash_get(h,h->order[i]))); } break; }
    case SP_BUILTIN_INT_STR_HASH: { sp_IntStrHash *h=(sp_IntStrHash*)v.v.p; for(mrb_int i=0;i<h->len;i++){ sp_mar_w(b, sp_box_int(h->order[i])); sp_mar_w(b, sp_box_str(sp_IntStrHash_get(h,h->order[i]))); } break; }
    case SP_BUILTIN_STR_POLY_HASH: { sp_StrPolyHash *h=(sp_StrPolyHash*)v.v.p; for(mrb_int i=0;i<h->len;i++){ sp_mar_w(b, sp_box_str(h->order[i])); sp_mar_w(b, sp_StrPolyHash_get(h,h->order[i])); } break; }
    case SP_BUILTIN_SYM_POLY_HASH: { sp_SymPolyHash *h=(sp_SymPolyHash*)v.v.p; for(mrb_int i=0;i<h->len;i++){ sp_mar_w(b, sp_box_sym(h->order[i])); sp_mar_w(b, sp_SymPolyHash_get(h,h->order[i])); } break; }
    case SP_BUILTIN_POLY_POLY_HASH: { sp_PolyPolyHash *h=(sp_PolyPolyHash*)v.v.p; for(mrb_int i=0;i<h->len;i++){ mrb_int oi=h->order[i]; sp_mar_w(b, h->keys[oi]); sp_mar_w(b, h->vals[oi]); } break; }
    default: sp_raise_cls("TypeError", "unsupported Hash variant for Marshal.dump");
  }
}
static void sp_mar_w(sp_mar_buf *b, sp_RbVal v) {
  switch (v.tag) {
    case SP_TAG_NIL:  sp_mar_b(b, '0'); break;
    case SP_TAG_BOOL: sp_mar_b(b, v.v.b ? 'T' : 'F'); break;
    case SP_TAG_INT:  sp_mar_b(b, 'i'); sp_mar_long(b, (long)v.v.i); break;
    case SP_TAG_FLT: {
      sp_mar_b(b, 'f');
      mrb_float f = v.v.f; const char *fs;
      if (isnan(f)) fs = "nan";
      else if (isinf(f)) fs = f < 0 ? "-inf" : "inf";
      else fs = sp_float_to_s(f);
      sp_mar_bytes(b, fs, strlen(fs));
      break;
    }
    case SP_TAG_SYM:  sp_mar_sym(b, sp_sym_to_s((sp_sym)v.v.i)); break;
    case SP_TAG_STR: {
      const char *s = v.v.s ? v.v.s : "";
      /* CRuby wraps a String in an ivar carrying its encoding (:E => true for
         UTF-8): I "<bytes>" <1 ivar> :E T. */
      sp_mar_b(b, 'I'); sp_mar_b(b, '"'); sp_mar_bytes(b, s, sp_str_byte_len(s));
      sp_mar_long(b, 1); sp_mar_sym(b, "E"); sp_mar_b(b, 'T');
      break;
    }
    case SP_TAG_OBJ:
      switch (v.cls_id) {
        case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_FLT_ARRAY:
        case SP_BUILTIN_STR_ARRAY: case SP_BUILTIN_SYM_ARRAY:
        case SP_BUILTIN_POLY_ARRAY: {
          sp_mar_b(b, '['); mrb_int n = sp_poly_length(v); sp_mar_long(b, n);
          for (mrb_int i = 0; i < n; i++) sp_mar_w(b, sp_poly_arr_get(v, i));
          break;
        }
        case SP_BUILTIN_STR_INT_HASH: case SP_BUILTIN_STR_STR_HASH:
        case SP_BUILTIN_INT_STR_HASH: case SP_BUILTIN_STR_POLY_HASH:
        case SP_BUILTIN_SYM_POLY_HASH: case SP_BUILTIN_POLY_POLY_HASH:
          sp_mar_w_hash(b, v); break;
        default:
          sp_raise_cls("TypeError", "no marshal_dump is defined for this object");
      }
      break;
    case SP_TAG_BIGINT:
      sp_raise_cls("TypeError", "Marshal.dump of a Bignum is not yet supported");
      break;
    default:
      sp_raise_cls("TypeError", "unsupported value for Marshal.dump");
  }
}
static const char *sp_marshal_dump(sp_RbVal v) {
  sp_mar_buf b; b.p = NULL; b.len = 0; b.cap = 0;
  sp_mar_b(&b, 4); sp_mar_b(&b, 8);  /* major.minor = 4.8 */
  sp_mar_w(&b, v);
  char *out = sp_str_alloc_raw(b.len + 1);
  memcpy(out, b.p, b.len); out[b.len] = 0;
  sp_str_set_len(out, b.len);
  free(b.p);
  return out;
}

typedef struct { const char *s; size_t pos, len; char **syms; int nsym, csym; } sp_mar_rd;
static unsigned char sp_mar_rb(sp_mar_rd *r) { return r->pos < r->len ? (unsigned char)r->s[r->pos++] : 0; }
static long sp_mar_rlong(sp_mar_rd *r) {
  int c = (signed char)sp_mar_rb(r);
  if (c == 0) return 0;
  if (c > 0) {
    if (4 < c && c < 128) return (long)c - 5;
    long x = 0;
    for (int i = 0; i < c && i < (int)sizeof(long); i++) x |= (long)sp_mar_rb(r) << (8 * i);
    return x;
  }
  if (-129 < c && c < -4) return (long)c + 5;
  c = -c;
  long x = -1;
  for (int i = 0; i < c && i < (int)sizeof(long); i++) {
    x &= ~((long)0xff << (8 * i));
    x |= (long)sp_mar_rb(r) << (8 * i);
  }
  return x;
}
static char *sp_mar_rstr(sp_mar_rd *r, long n) {
  if (n < 0) n = 0;
  char *out = sp_str_alloc_raw((size_t)n + 1);
  for (long i = 0; i < n; i++) out[i] = (char)sp_mar_rb(r);
  out[n] = 0;
  sp_str_set_len(out, (size_t)n);
  return out;
}
static sp_RbVal sp_mar_r(sp_mar_rd *r) {
  unsigned char t = sp_mar_rb(r);
  switch (t) {
    case '0': return sp_box_nil();
    case 'T': return sp_box_bool(TRUE);
    case 'F': return sp_box_bool(FALSE);
    case 'i': return sp_box_int((mrb_int)sp_mar_rlong(r));
    case 'f': {
      long n = sp_mar_rlong(r);
      char *s = sp_mar_rstr(r, n); SP_GC_ROOT(s);
      mrb_float f;
      if (!strcmp(s, "inf")) f = (mrb_float)INFINITY;
      else if (!strcmp(s, "-inf")) f = -(mrb_float)INFINITY;
      else if (!strcmp(s, "nan")) f = (mrb_float)NAN;
      else f = strtod(s, NULL);
      return sp_box_float(f);
    }
    case ':': {
      long n = sp_mar_rlong(r);
      char *name = sp_mar_rstr(r, n); SP_GC_ROOT(name);
      if (r->nsym >= r->csym) { r->csym = r->csym ? r->csym * 2 : 8; r->syms = (char **)realloc(r->syms, sizeof(char *) * (size_t)r->csym); }
      r->syms[r->nsym++] = strdup(name);
      return sp_box_sym(sp_sym_intern(name));
    }
    case ';': {
      long idx = sp_mar_rlong(r);
      const char *name = (idx >= 0 && idx < r->nsym) ? r->syms[idx] : "";
      return sp_box_sym(sp_sym_intern(name));
    }
    case '"': return sp_box_str(sp_mar_rstr(r, sp_mar_rlong(r)));
    case 'I': {
      sp_RbVal inner = sp_mar_r(r);
      long nivar = sp_mar_rlong(r);
      for (long i = 0; i < nivar; i++) { sp_mar_r(r); sp_mar_r(r); }  /* encoding ivar: ignored */
      return inner;
    }
    case '[': {
      long n = sp_mar_rlong(r);
      sp_PolyArray *a = sp_PolyArray_new(); SP_GC_ROOT(a);
      for (long i = 0; i < n; i++) sp_PolyArray_push(a, sp_mar_r(r));
      return sp_box_poly_array(a);
    }
    case '{': {
      long n = sp_mar_rlong(r);
      sp_PolyPolyHash *h = sp_PolyPolyHash_new(); SP_GC_ROOT(h);
      for (long i = 0; i < n; i++) { sp_RbVal k = sp_mar_r(r); sp_RbVal val = sp_mar_r(r); sp_PolyPolyHash_set(h, k, val); }
      return sp_box_obj(h, SP_BUILTIN_POLY_POLY_HASH);
    }
    default: sp_raise_cls("ArgumentError", "unsupported type in Marshal.load"); return sp_box_nil();
  }
}
static sp_RbVal sp_marshal_load(const char *s, mrb_int len) {
  sp_mar_rd r; r.s = s ? s : ""; r.pos = 0; r.len = s ? (size_t)len : 0; r.syms = NULL; r.nsym = 0; r.csym = 0;
  if (r.len >= 2) r.pos = 2;  /* skip the 4.8 version header */
  sp_RbVal v = sp_mar_r(&r);
  for (int i = 0; i < r.nsym; i++) free(r.syms[i]);
  free(r.syms);
  return v;
}
#endif /* SP_MARSHAL_H */
