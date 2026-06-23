#ifndef SP_ALLOC_H
#define SP_ALLOC_H
/* Shared GC-string allocation.

   The string-heap state below is `extern` -- defined once in sp_alloc.c and
   linked from libspinel_rt.a -- so the generated program's single translation
   unit and every standalone lib C file share ONE string heap. A cold runtime C file
   (marshal, pack, json, ...) can therefore allocate GC-tracked strings directly
   without #including sp_runtime.h and inheriting its per-TU `static` heap state
   (which would otherwise create a second, never-swept string heap and leak).

   The hot allocators stay `static inline` here so each including TU still
   inlines them over the shared extern state -- the same shape sp_gc_alloc
   already uses for the extern sp_gc_heap / sp_gc_bytes object heap. */
#include "sp_types.h"   /* sp_str_hdr, mrb_int, mrb_float */
#include "sp_gc.h"      /* sp_gc_collect, sp_oom_die, sp_gc_str_sweep_hook */
#include <stdlib.h>
#include <string.h>

/* ---- shared string-heap state (defined in sp_alloc.c) ---- */
extern sp_str_hdr *sp_str_heap;          /* live-string singly-linked list head */
extern size_t sp_str_heap_bytes;         /* live string-heap bytes */
extern size_t sp_str_threshold;          /* string-GC trigger (own heuristic) */
extern size_t sp_str_threshold_init;     /* recompute floor */
extern int    sp_str_stress_checked;     /* one-shot SPINEL_GC_STRESS check */

extern const char sp_str_empty_data[];
#define sp_str_empty (sp_str_empty_data + 1)

/* UTF-8 char-length cache. Shared (extern) so sp_str_sweep flushes the same
   table the length helpers in sp_runtime.h populate: a per-TU split would leave
   the generated TU's cache pointing at strings the archive-side sweep already
   freed. */
#define SP_STR_LCACHE_BITS 5
#define SP_STR_LCACHE_SIZE (1u << SP_STR_LCACHE_BITS)
struct sp_str_lcache_entry {
  const char *s;
  size_t byte_len;
  mrb_int char_len;
};
extern struct sp_str_lcache_entry sp_str_lcache[SP_STR_LCACHE_SIZE];

/* Cold; single definitions in sp_alloc.c. sp_str_sweep is wired to the GC via a
   constructor so it runs from sp_gc_collect regardless of which TU triggered
   the collection. */
void sp_str_sweep(void);
void sp_str_lcache_clear(void);

static inline char *sp_str_alloc(size_t len) {
  size_t total = sizeof(sp_str_hdr) + 1 + len + 1;
  /* String-heap pressure drives its own collection (see sp_str_heap_bytes).
     Collect BEFORE the new allocation, like sp_gc_alloc, so the string being
     built isn't yet live during the sweep. Operands of the calling op (e.g. the
     arguments to sp_str_concat) must be reachable across this point -- they are
     for rooted locals; the codegen's SP_GC_ROOT discipline is what keeps them
     so. Threshold recompute mirrors sp_gc_alloc's. */
  if (!sp_str_stress_checked) { sp_str_stress_checked = 1; const char *e = getenv("SPINEL_GC_STRESS"); if (e && *e && *e != '0') { sp_str_threshold = 2048; sp_str_threshold_init = 2048; } }
  if (sp_str_heap_bytes > sp_str_threshold) {
    size_t before = sp_str_heap_bytes;
    sp_gc_collect();                 /* runs sp_str_sweep, which decrements sp_str_heap_bytes */
    size_t freed = before - sp_str_heap_bytes;
    if (freed < before/4) sp_str_threshold = before*2;
    else if (sp_str_heap_bytes > 0) { sp_str_threshold = sp_str_heap_bytes*4; if (sp_str_threshold < sp_str_threshold_init) sp_str_threshold = sp_str_threshold_init; }
    else sp_str_threshold = sp_str_threshold_init;
  }
  sp_str_hdr *h = (sp_str_hdr *)malloc(total);
  if (!h) sp_oom_die();
  h->next = sp_str_heap;
  h->size = (uint32_t)total;
  h->len = (uint32_t)len;
  h->hash = 0;
  sp_str_heap = h;
  sp_str_heap_bytes += total;
  /* Don't fold string-heap pressure into sp_gc_bytes: the threshold heuristic
     in sp_gc_alloc is keyed on object-heap survivors, and the str-heap sweep
     that runs alongside (sp_str_sweep, from sp_gc_collect) doesn't add surviving
     strings back into sp_gc_bytes. Folding them in over-fires the object
     heuristic (the reason they're excluded). */
  char *body = (char *)(h + 1);
  body[0] = (char)0xfe;
  body[1 + len] = 0;
  return body + 1;
}

static inline char *sp_str_alloc_raw(size_t total_with_null) {
  return sp_str_alloc(total_with_null > 0 ? total_with_null - 1 : 0);
}

static inline size_t sp_str_byte_len(const char *s) {
  if (!s) return 0;
  unsigned char marker = ((const unsigned char *)s)[-1];
  if (marker == 0xfe || marker == 0xfc || marker == 0xfd) {
    return (((const sp_str_hdr *)(s - 1)) - 1)->len;
  }
  return strlen(s);
}

static inline void sp_str_set_len(char *s, size_t len) {
  if (!s) return;
  unsigned char marker = ((unsigned char *)s)[-1];
  if (marker == 0xfe || marker == 0xfc || marker == 0xfd) {
    sp_str_hdr *hd = ((sp_str_hdr *)(s - 1)) - 1;
    hd->len = (uint32_t)len;
    hd->hash = 0;  /* length change implies content change: invalidate cached hash */
  }
}

static inline const char *sp_str_from_bytes(const char *data, size_t len) {
  char *s = sp_str_alloc(len);
  if (data) memcpy(s, data, len);
  s[len] = 0;
  return s;
}
static inline const char *sp_str_dup_external(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s);
  char *r = sp_str_alloc(n);
  memcpy(r, s, n);
  return r;
}
#endif /* SP_ALLOC_H */
