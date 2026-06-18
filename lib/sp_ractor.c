/* sp_ractor.c -- minimal pthread-backed Ractor runtime. See sp_ractor.h.
 * sp_raise_cls / sp_fiber_thread_init / sp_gc_thread_teardown live elsewhere
 * (the generated TU and the other libspinel_rt units) and are reached by
 * name, the same way sp_fiber.c reaches sp_gc_alloc. */
#include "sp_ractor.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ---- Reached by name ---- */
void sp_raise_cls(const char *cls, const char *msg);
void sp_fiber_thread_init(void);
void sp_gc_thread_teardown(void);

/* Codec hooks, installed by the generated TU at startup. */
sp_RactorBlob (*sp_ractor_serialize_hook)(sp_RbVal v) = NULL;
sp_RbVal      (*sp_ractor_deserialize_hook)(sp_RactorBlob b) = NULL;

/* A blocking single-ended FIFO of serialized message blobs guarded by a
   mutex+condvar. Each Ractor has two: `inbox` (messages sent to it; drained by
   Ractor.receive) and `outbox` (values it yields; drained by r.take). Blobs
   are heap-neutral, so they cross the Ractor heap boundary safely. */
typedef struct {
  pthread_mutex_t mtx;
  pthread_cond_t  cv;
  sp_RactorBlob  *buf;
  int             head, tail, len, cap;
  int             closed;
} sp_RactorQueue;

struct sp_Ractor {
  pthread_t       thread;
  void          (*body)(sp_Ractor *);
  sp_RactorQueue  inbox;
  sp_RactorQueue  outbox;
  sp_Ractor      *reg_next;   /* process-lifetime registry link */
};

__thread sp_Ractor *sp_ractor_current = NULL;

/* A Ractor handle is shared between its creator (which keeps it in a
   GC-untracked local) and its detached worker thread, so neither side has a
   safe point to free it mid-run. Rather than leak it, every Ractor is linked
   into this process-lifetime registry at creation and reclaimed by the OS at
   exit -- so it stays reachable (no "definitely lost" under valgrind) without
   a refcount/join protocol. The list is only ever appended to. */
static pthread_mutex_t sp_ractor_reg_mtx = PTHREAD_MUTEX_INITIALIZER;
static sp_Ractor      *sp_ractor_reg_head = NULL;
static void sp_ractor_register(sp_Ractor *r) {
  pthread_mutex_lock(&sp_ractor_reg_mtx);
  r->reg_next = sp_ractor_reg_head; sp_ractor_reg_head = r;
  pthread_mutex_unlock(&sp_ractor_reg_mtx);
}

static void sp_rq_init(sp_RactorQueue *q) {
  pthread_mutex_init(&q->mtx, NULL);
  pthread_cond_init(&q->cv, NULL);
  q->cap = 8;
  q->buf = (sp_RactorBlob *)malloc(sizeof(sp_RactorBlob) * q->cap);
  q->head = q->tail = q->len = 0;
  q->closed = 0;
}

static void sp_rq_destroy(sp_RactorQueue *q) {
  for (int i = 0; i < q->len; i++) free(q->buf[(q->head + i) % q->cap].data);
  free(q->buf); q->buf = NULL;
  pthread_mutex_destroy(&q->mtx);
  pthread_cond_destroy(&q->cv);
}

static void sp_rq_push(sp_RactorQueue *q, sp_RactorBlob v) {
  pthread_mutex_lock(&q->mtx);
  if (q->len == q->cap) {
    int ncap = q->cap * 2;
    sp_RactorBlob *nb = (sp_RactorBlob *)malloc(sizeof(sp_RactorBlob) * ncap);
    for (int i = 0; i < q->len; i++) nb[i] = q->buf[(q->head + i) % q->cap];
    free(q->buf); q->buf = nb; q->cap = ncap; q->head = 0; q->tail = q->len;
  }
  q->buf[q->tail] = v;
  q->tail = (q->tail + 1) % q->cap;
  q->len++;
  pthread_cond_signal(&q->cv);
  pthread_mutex_unlock(&q->mtx);
}

/* Block until a blob is available. Returns 1 with *out set, or 0 if the
   queue was closed and drained (no more values will ever arrive). */
static int sp_rq_pop(sp_RactorQueue *q, sp_RactorBlob *out) {
  pthread_mutex_lock(&q->mtx);
  while (q->len == 0 && !q->closed) pthread_cond_wait(&q->cv, &q->mtx);
  if (q->len == 0) { pthread_mutex_unlock(&q->mtx); return 0; }
  *out = q->buf[q->head];
  q->head = (q->head + 1) % q->cap;
  q->len--;
  pthread_mutex_unlock(&q->mtx);
  return 1;
}

static void sp_rq_close(sp_RactorQueue *q) {
  pthread_mutex_lock(&q->mtx);
  q->closed = 1;
  pthread_cond_broadcast(&q->cv);
  pthread_mutex_unlock(&q->mtx);
}

/* Serialize a value into a heap-neutral blob on the sender side. The codec is
   installed by the generated TU; refuse to run without it (a generated program
   always installs it at startup). */
static sp_RactorBlob sp_ractor_to_blob(sp_RbVal v) {
  if (!sp_ractor_serialize_hook)
    sp_raise_cls("Ractor::Error", "Ractor message codec not installed");
  return sp_ractor_serialize_hook(v);
}
/* Deserialize a blob into the receiver's heap, then free the blob. */
static sp_RbVal sp_ractor_from_blob(sp_RactorBlob b) {
  sp_RbVal v = sp_ractor_deserialize_hook ? sp_ractor_deserialize_hook(b)
                                          : (sp_RbVal){0};
  free(b.data);
  return v;
}

/* Entry trampoline for a Ractor pthread: establish this thread's Ractor and
   fiber identity, run the body, then close the outbox (so a waiting r.take
   unblocks) and reclaim the thread's private GC heaps. */
static void *sp_ractor_trampoline(void *arg) {
  sp_Ractor *r = (sp_Ractor *)arg;
  sp_ractor_current = r;
  sp_fiber_thread_init();
  r->body(r);                 /* body installs its own top-level rescue pad */
  sp_rq_close(&r->outbox);
  sp_gc_thread_teardown();
  return NULL;
}

sp_Ractor *sp_Ractor_new(void (*body)(sp_Ractor *)) {
  sp_Ractor *r = (sp_Ractor *)calloc(1, sizeof(sp_Ractor));
  if (!r) sp_raise_cls("Ractor::Error", "failed to allocate Ractor");
  r->body = body;
  sp_rq_init(&r->inbox);
  sp_rq_init(&r->outbox);
  sp_ractor_register(r);   /* retained for the process lifetime */
  if (pthread_create(&r->thread, NULL, sp_ractor_trampoline, r) != 0)
    sp_raise_cls("Ractor::Error", "failed to spawn Ractor thread");
  pthread_detach(r->thread);
  return r;
}

void sp_Ractor_send(sp_Ractor *r, sp_RbVal v) {
  /* Serialize on the sender (reads the sender's heap) before crossing. */
  sp_rq_push(&r->inbox, sp_ractor_to_blob(v));
}

sp_RbVal sp_Ractor_receive(void) {
  sp_Ractor *r = sp_ractor_current;
  if (!r) sp_raise_cls("Ractor::Error", "Ractor.receive called outside a Ractor");
  sp_RactorBlob b;
  if (!sp_rq_pop(&r->inbox, &b))
    sp_raise_cls("Ractor::Error", "Ractor mailbox closed");
  return sp_ractor_from_blob(b); /* rebuild into this Ractor's heap */
}

void sp_Ractor_yield(sp_RbVal v) {
  sp_Ractor *r = sp_ractor_current;
  if (!r) sp_raise_cls("Ractor::Error", "Ractor.yield called outside a Ractor");
  sp_rq_push(&r->outbox, sp_ractor_to_blob(v));
}

sp_RbVal sp_Ractor_take(sp_Ractor *r) {
  sp_RactorBlob b;
  if (!sp_rq_pop(&r->outbox, &b))
    sp_raise_cls("Ractor::Error", "Ractor terminated without yielding a value");
  return sp_ractor_from_blob(b); /* rebuild into the taker's heap */
}

/* Unused today but keeps -Wunused-function quiet about sp_rq_destroy and
   documents the queue teardown path for when Ractor structs become
   reclaimable (they are intentionally leaked in Milestone 1; see RFC). */
void sp_ractor_free(sp_Ractor *r) {
  sp_rq_destroy(&r->inbox);
  sp_rq_destroy(&r->outbox);
  free(r);
}
