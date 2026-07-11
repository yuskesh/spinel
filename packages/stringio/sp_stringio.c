/* sp_stringio.c -- StringIO methods, a carried-C spin package (Path B), linked
   on demand when `require "stringio"` appears. The buffer is a plain malloc'd
   char array; readers hand back GC strings via the shared allocator. Compiles
   against the stable package ABI; the struct + prototypes stay in lib's
   sp_stringio.h, the interface the compiler's StringIO dispatch emits calls to. */
#include "sp_stringio.h"      /* the StringIO struct + this unit's prototypes */
#include <stdlib.h>
#include <string.h>

/* Detach a borrowed (GC-string-backed) buffer into a private malloc'd copy
   before any mutation. Spinel strings are immutable char*, so a StringIO
   constructed over one shares it read-only ("string" keeps identity) and
   copies on the first write; CRuby's write-through into the original String
   object is not representable in this string model. */
static void sio_own(sp_StringIO *sio) {
  if (!sio->borrowed) return;
  int64_t nc = sio->len < 63 ? 63 : sio->len;
  char *nb = (char *)malloc(nc + 1);
  if (!nb) sp_oom_die();
  memcpy(nb, sio->buf, sio->len);
  nb[sio->len] = '\0';
  sio->buf = nb;
  sio->cap = nc;
  sio->borrowed = 0;
}
static void sio_grow(sp_StringIO *sio, int64_t need) { sio_own(sio); int64_t req = sio->pos + need; if (req <= sio->cap) return; int64_t nc = sio->cap ? sio->cap : 64; while (nc < req) nc *= 2; char *nb = (char *)realloc(sio->buf, nc + 1); if (!nb) sp_oom_die(); sio->buf = nb; sio->cap = nc; }
static int64_t sio_write(sp_StringIO *sio, const char *d, int64_t dl) { sio_grow(sio, dl); if (sio->pos > sio->len) memset(sio->buf + sio->len, 0, sio->pos - sio->len); memcpy(sio->buf + sio->pos, d, dl); sio->pos += dl; if (sio->pos > sio->len) sio->len = sio->pos; sio->buf[sio->len] = '\0'; return dl; }

void sp_StringIO_free(void *p) { sp_StringIO *s = (sp_StringIO *)p; if (!s->borrowed) free(s->buf); s->buf = NULL; }
static void sp_StringIO_scan_gc(void *p) { sp_StringIO *s = (sp_StringIO *)p; if (s->borrowed && s->buf) sp_mark_string(s->buf); }
sp_StringIO *sp_StringIO_new(mrb_int cls_id) { sp_StringIO *s = (sp_StringIO *)sp_gc_alloc(sizeof(sp_StringIO), sp_StringIO_free, sp_StringIO_scan_gc); memset(s, 0, sizeof *s); s->cls_id = cls_id; s->buf = (char *)calloc(1, 64); if (!s->buf) sp_oom_die(); s->cap = 63; return s; }
/* Adopt the incoming GC string without copying so #string keeps identity
   with the constructor argument; the first mutation copies (sio_own). */
sp_StringIO *sp_StringIO_new_s(mrb_int cls_id, const char *init) { if (!init) sp_raise_cls("TypeError", "no implicit conversion of nil into String"); sp_StringIO *s = (sp_StringIO *)sp_gc_alloc(sizeof(sp_StringIO), sp_StringIO_free, sp_StringIO_scan_gc); memset(s, 0, sizeof *s); s->cls_id = cls_id; int64_t l = (int64_t)strlen(init); s->buf = (char *)init; s->len = l; s->cap = l; s->borrowed = 1; return s; }
/* StringIO.new(str, mode): the mode's first char selects the initial
   content/position. "w"/"w+" truncate to empty; "a"/"a+" keep the content and
   seek to the end; "r"/"r+" and anything else keep the content at position 0.
   Read-only enforcement ("r" rejecting writes) is not modelled. */
sp_StringIO *sp_StringIO_new_sm(mrb_int cls_id, const char *init, const char *mode) {
  if (!init) sp_raise_cls("TypeError", "no implicit conversion of nil into String");
  if (!mode || !mode[0]) return sp_StringIO_new_s(cls_id, init);
  char m0 = mode[0];
  if (m0 == 'w') return sp_StringIO_new(cls_id);
  sp_StringIO *s = sp_StringIO_new_s(cls_id, init);
  if (m0 == 'a') s->pos = s->len;
  return s;
}
const char *sp_StringIO_string(sp_StringIO *s) { return s->buf ? s->buf : sp_str_empty; }
mrb_int sp_StringIO_pos(sp_StringIO *s) { return s->pos; }
mrb_int sp_StringIO_size(sp_StringIO *s) { return s->len; }
mrb_int sp_StringIO_write(sp_StringIO *s, const char *str) { return sio_write(s, str, (int64_t)strlen(str)); }
mrb_int sp_StringIO_puts(sp_StringIO *s, const char *str) { int64_t l = (int64_t)strlen(str); sio_write(s, str, l); if (l == 0 || str[l-1] != '\n') sio_write(s, "\n", 1); return 0; }
mrb_int sp_StringIO_puts_empty(sp_StringIO *s) { sio_write(s, "\n", 1); return 0; }
mrb_int sp_StringIO_print(sp_StringIO *s, const char *str) { return sio_write(s, str, (int64_t)strlen(str)); }
mrb_int sp_StringIO_putc(sp_StringIO *s, mrb_int ch) { char c = (char)(ch & 0xFF); sio_write(s, &c, 1); return ch; }
const char *sp_StringIO_read(sp_StringIO *s) { if (s->pos >= s->len) return sp_str_empty; size_t rem = s->len - s->pos; char *r = sp_str_alloc(rem); memcpy(r, s->buf + s->pos, rem); r[rem] = 0; s->pos = s->len; return r; }
const char *sp_StringIO_read_n(sp_StringIO *s, mrb_int n) { if (s->pos >= s->len) return sp_str_empty; int64_t rem = s->len - s->pos; if (n > rem) n = rem; char *r = sp_str_alloc_raw(n+1); memcpy(r, s->buf + s->pos, n); r[n] = '\0'; sp_str_set_len(r, (size_t)n); s->pos += n; return r; }
const char *sp_StringIO_gets(sp_StringIO *s) { if (s->pos >= s->len) return NULL; const char *st = s->buf + s->pos; const char *nl = memchr(st, '\n', s->len - s->pos); int64_t ll = nl ? (nl - st) + 1 : s->len - s->pos; char *r = sp_str_alloc_raw(ll+1); memcpy(r, st, ll); r[ll] = '\0'; sp_str_set_len(r, (size_t)ll); s->pos += ll; s->lineno++; return r; }
const char *sp_StringIO_getc(sp_StringIO *s) { if (s->pos >= s->len) return NULL; char *gc = sp_str_alloc_raw(2); gc[0] = s->buf[s->pos++]; gc[1] = '\0'; sp_str_set_len(gc, 1); return gc; }
mrb_int sp_StringIO_getbyte(sp_StringIO *s) { if (s->pos >= s->len) return -1; return (int64_t)(unsigned char)s->buf[s->pos++]; }
mrb_int sp_StringIO_rewind(sp_StringIO *s) { s->pos = 0; s->lineno = 0; return 0; }
mrb_int sp_StringIO_seek(sp_StringIO *s, mrb_int off) { if (off < 0) off = 0; s->pos = off; return 0; }
mrb_int sp_StringIO_tell(sp_StringIO *s) { return s->pos; }
mrb_bool sp_StringIO_eof_p(sp_StringIO *s) { return s->pos >= s->len; }
mrb_int sp_StringIO_truncate(sp_StringIO *s, mrb_int l) { if (l < 0) l = 0; if (l < s->len) { sio_own(s); s->len = l; s->buf[l] = '\0'; } return 0; }
mrb_int sp_StringIO_close(sp_StringIO *s) { s->closed = 1; return 0; }
mrb_bool sp_StringIO_closed_p(sp_StringIO *s) { return s->closed; }
sp_StringIO *sp_StringIO_flush(sp_StringIO *s) { return s; }
mrb_bool sp_StringIO_sync(sp_StringIO *s) { (void)s; return 1; }
mrb_bool sp_StringIO_isatty(sp_StringIO *s) { (void)s; return 0; }

/* Normalized helpers so the binding stays a plain method->symbol map:
   putc with a string arg writes its first byte; lineno is a field read;
   fsync/fileno/pid are always 0 on an in-memory stream. */
mrb_int sp_StringIO_putc_s(sp_StringIO *s, const char *str) { return sp_StringIO_putc(s, (mrb_int)(unsigned char)(str && str[0] ? str[0] : 0)); }
mrb_int sp_StringIO_lineno(sp_StringIO *s) { return s->lineno; }
mrb_int sp_StringIO_zero(sp_StringIO *s) { (void)s; return 0; }

/* << writes and returns the receiver (chainable), unlike write's byte count. */
sp_StringIO *sp_StringIO_shl(sp_StringIO *s, const char *str) { sio_write(s, str, (int64_t)strlen(str)); return s; }

/* gets(sep): read through the end of the first occurrence of `sep` (the
   whole rest on a miss); nil at EOF. A multi-byte separator is honored. */
const char *sp_StringIO_gets_sep(sp_StringIO *s, const char *sep) {
  if (s->pos >= s->len) return NULL;
  const char *st = s->buf + s->pos;
  int64_t rem = s->len - s->pos;
  int64_t sl = sep ? (int64_t)strlen(sep) : 0;
  int64_t ll = rem;
  if (sl > 0) {
    for (int64_t i = 0; i + sl <= rem; i++) {
      if (memcmp(st + i, sep, sl) == 0) { ll = i + sl; break; }
    }
  }
  char *r = sp_str_alloc_raw(ll + 1);
  memcpy(r, st, ll);
  r[ll] = '\0';
  sp_str_set_len(r, (size_t)ll);
  s->pos += ll;
  s->lineno++;
  return r;
}

/* seek(off, whence): 0=SET, 1=CUR, 2=END; a negative result is EINVAL. */
mrb_int sp_StringIO_seek2(sp_StringIO *s, mrb_int off, mrb_int whence) {
  int64_t base = whence == 1 ? s->pos : whence == 2 ? s->len : 0;
  int64_t np = base + off;
  if (np < 0) sp_raise_cls("Errno::EINVAL", "Invalid argument");
  s->pos = np;
  return 0;
}

/* readline: gets that raises EOFError at end of stream (CRuby IO#readline). */
const char *sp_StringIO_readline(sp_StringIO *s) {
  const char *r = sp_StringIO_gets(s);
  if (!r) sp_raise_cls("EOFError", "end of file reached");
  return r;
}

sp_RbVal sp_StringIO_readlines(sp_StringIO *s) {
  sp_PolyArray *a = sp_PolyArray_new();
  const char *l;
  while ((l = sp_StringIO_gets(s)) != NULL) sp_PolyArray_push(a, sp_box_str(l));
  return sp_box_poly_array(a);
}

/* Boxed-value writer behind the variadic print/puts arms. print writes the
   value's to_s; puts additionally flattens arrays (each element on its own
   line, via the generic container hooks) and terminates lines. A value kind
   this in-memory stream can't render raises rather than writing garbage. */
static void sio_write_val(sp_StringIO *s, sp_RbVal v, int is_puts) {
  int64_t p0 = s->pos;
  switch (v.tag) {
    case SP_TAG_STR: { const char *t = v.v.s ? v.v.s : ""; sio_write(s, t, (int64_t)strlen(t)); break; }
    case SP_TAG_INT: { const char *t = sp_int_to_s(v.v.i); sio_write(s, t, (int64_t)strlen(t)); break; }
    case SP_TAG_FLT: { const char *t = sp_float_to_s(v.v.f); sio_write(s, t, (int64_t)strlen(t)); break; }
    case SP_TAG_BOOL: { const char *t = v.v.b ? "true" : "false"; sio_write(s, t, (int64_t)strlen(t)); break; }
    case SP_TAG_NIL: break;  /* puts nil -> bare newline; print nil -> nothing */
    default:
      if (is_puts && sp_json_kind_fn && sp_json_kind_fn(v) == 1) {
        mrb_int n = sp_json_len_fn(v);
        for (mrb_int i = 0; i < n; i++) sio_write_val(s, sp_json_aref_fn(v, i), 1);
        return;  /* elements each terminated their own line */
      }
      sp_raise_cls("TypeError", "can't write value to StringIO");
  }
  /* terminate the line unless THIS value's bytes already ended with one */
  if (is_puts && (s->pos == p0 || s->buf[s->pos - 1] != '\n')) sio_write(s, "\n", 1);
}

mrb_int sp_StringIO_print_v1(sp_StringIO *s, sp_RbVal a) { sio_write_val(s, a, 0); return 0; }
mrb_int sp_StringIO_print_v2(sp_StringIO *s, sp_RbVal a, sp_RbVal b) { sio_write_val(s, a, 0); sio_write_val(s, b, 0); return 0; }
mrb_int sp_StringIO_print_v3(sp_StringIO *s, sp_RbVal a, sp_RbVal b, sp_RbVal c2) { sio_write_val(s, a, 0); sio_write_val(s, b, 0); sio_write_val(s, c2, 0); return 0; }
mrb_int sp_StringIO_puts_v1(sp_StringIO *s, sp_RbVal a) { sio_write_val(s, a, 1); return 0; }
mrb_int sp_StringIO_puts_v2(sp_StringIO *s, sp_RbVal a, sp_RbVal b) { sio_write_val(s, a, 1); sio_write_val(s, b, 1); return 0; }
mrb_int sp_StringIO_puts_v3(sp_StringIO *s, sp_RbVal a, sp_RbVal b, sp_RbVal c2) { sio_write_val(s, a, 1); sio_write_val(s, b, 1); sio_write_val(s, c2, 1); return 0; }
