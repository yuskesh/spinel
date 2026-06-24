/* sp_alloc.c -- the single, shared definitions backing sp_alloc.h.

   Owns the string heap so that both the generated program and every standalone
   lib/*.c allocate onto one heap. sp_str_sweep is registered with the object GC
   via a constructor, so a collection triggered from any TU also reaps strings. */
#include "sp_alloc.h"

sp_str_hdr *sp_str_heap = NULL;
size_t sp_str_heap_bytes = 0;
size_t sp_str_threshold = 256 * 1024;
size_t sp_str_threshold_init = 256 * 1024;
int sp_str_stress_checked = 0;

const char sp_str_empty_data[] = "\xff";

/* Object-heap collection threshold (was per-TU static in sp_runtime.h; now
   shared so sp_gc_alloc can live in sp_alloc.h and lib TUs allocate too). */
size_t sp_gc_threshold = 256 * 1024;
size_t sp_gc_threshold_init = 256 * 1024;
int sp_gc_stress_checked = 0;

void *sp_gc_alloc(size_t sz, void (*fin)(void *), void (*scn)(void *)) {
  if (!sp_gc_stress_checked) { sp_gc_stress_checked = 1; const char *e = getenv("SPINEL_GC_STRESS"); if (e && *e && *e != '0') { sp_gc_threshold = 2048; sp_gc_threshold_init = 2048; } }
  if (sp_gc_bytes > sp_gc_threshold) {
    size_t before = sp_gc_bytes;
    sp_gc_collect();
    size_t freed = before - sp_gc_bytes;
    if (freed < before / 4) { sp_gc_threshold = before * 2; }
    else if (sp_gc_bytes > 0) { sp_gc_threshold = sp_gc_bytes * 4; if (sp_gc_threshold < sp_gc_threshold_init) sp_gc_threshold = sp_gc_threshold_init; }
    else { sp_gc_threshold = sp_gc_threshold_init; }
    sp_gc_enforce_mem_limit();
  }
  size_t need = sizeof(sp_gc_hdr) + sz;
  sp_gc_hdr *h = (sp_gc_hdr *)calloc(1, need);
  if (!h) sp_oom_die();
  h->finalize = fin; h->scan = scn; h->size = need; h->marked = 0;
  h->next = sp_gc_heap; sp_gc_heap = h; sp_gc_bytes += need;
  return (char *)h + sizeof(sp_gc_hdr);
}
void *sp_gc_alloc_nogc(size_t sz, void (*fin)(void *), void (*scn)(void *)) {
  size_t need = sizeof(sp_gc_hdr) + sz;
  sp_gc_hdr *h = (sp_gc_hdr *)calloc(1, need);
  if (!h) sp_oom_die();
  h->finalize = fin; h->scan = scn; h->size = need; h->marked = 0;
  h->next = sp_gc_heap; sp_gc_heap = h; sp_gc_bytes += need;
  return (char *)h + sizeof(sp_gc_hdr);
}

struct sp_str_lcache_entry sp_str_lcache[SP_STR_LCACHE_SIZE];

void sp_str_lcache_clear(void) {
  for (unsigned i = 0; i < SP_STR_LCACHE_SIZE; i++) sp_str_lcache[i].s = NULL;
}

/* sp_mark_string (sp_gc.h) flips a live string's marker 0xfe->0xfc during the
   mark phase; sweep keeps the marked ones and frees the rest. A frozen heap
   string (0xf1) is kept across sweeps (a live frozen global must survive, and
   frozen literals are immortal). */
void sp_str_sweep(void) {
  sp_str_hdr **pp = &sp_str_heap;
  while (*pp) {
    sp_str_hdr *h = *pp;
    char *body = (char *)(h + 1);
    if ((unsigned char)body[0] == 0xfc) {
      body[0] = (char)0xfe;
      pp = &h->next;
    }
    else if ((unsigned char)body[0] == 0xf1) {
      pp = &h->next;
    }
    else {
      *pp = h->next;
      sp_str_heap_bytes -= h->size;   /* keep the string-heap live-byte count in step */
      free(h);
    }
  }
  sp_str_lcache_clear();
}

/* Wire string sweep into the object collector. Runs before main, so the hook is
   set before the first allocation can trigger a collection. */
__attribute__((constructor)) static void sp_alloc_install_hooks(void) {
  sp_gc_str_sweep_hook = sp_str_sweep;
}
