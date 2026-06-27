/* sp_sched.c -- cooperative M:N thread scheduler bodies, Phase 0 (N=1).
 * See sp_sched.h. Built on the sp_fiber context switch: the main thread runs on
 * the root fiber and pumps a run queue of green threads whenever it blocks. */
#include "sp_sched.h"
#include "sp_alloc.h"   /* sp_box_nil / sp_box_obj */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Reached by name (defined in lib/sp_alloc.c or the generated TU), exactly as
   lib/sp_fiber.c reaches them. */
void *sp_gc_alloc(size_t sz, void (*fin)(void *), void (*scn)(void *));
SP_NORETURN void sp_raise_cls(const char *cls, const char *msg);
void sp_fiber_reraise(const char *cls, const char *msg, void *obj);

/* ---- scheduler state (single OS worker, so plain globals) ---- */
static sp_Fiber  *g_root_fiber = NULL;   /* the main thread's context, captured at init */
static sp_thread  g_main_thread;         /* the main thread: runs on root, fiber == NULL */
static sp_thread *g_current = NULL;      /* the green thread running right now */
static sp_thread *g_rq_head = NULL, *g_rq_tail = NULL;  /* FIFO run queue (RUNNABLE) */
static sp_thread *g_all = NULL;          /* registry of live threads, for GC rooting */
static unsigned   g_next_id = 1;
static unsigned char g_report_default = 1;  /* Thread.report_on_exception default */

static void rq_push(sp_thread *t) {
  t->state = SP_TH_RUNNABLE;
  t->rq_next = NULL;
  if (g_rq_tail) g_rq_tail->rq_next = t; else g_rq_head = t;
  g_rq_tail = t;
}
static sp_thread *rq_pop(void) {
  sp_thread *t = g_rq_head;
  if (t) { g_rq_head = t->rq_next; if (!g_rq_head) g_rq_tail = NULL; t->rq_next = NULL; }
  return t;
}

static void reg_add(sp_thread *t) {
  t->all_prev = NULL; t->all_next = g_all;
  if (g_all) g_all->all_prev = t;
  g_all = t;
}
static void reg_remove(sp_thread *t) {
  if (t->all_prev) t->all_prev->all_next = t->all_next;
  else if (g_all == t) g_all = t->all_next;
  if (t->all_next) t->all_next->all_prev = t->all_prev;
  t->all_prev = t->all_next = NULL;
}

/* GC: root every live green thread (and thus its fiber, stack roots, and
   pending result) so a fire-and-forget thread with no user reference is not
   collected mid-run. Chained ahead of whatever globals hook was installed. */
static void (*g_prev_globals_hook)(void) = NULL;
static void sp_sched_globals_mark(void) {
  for (sp_thread *t = g_all; t; t = t->all_next) sp_gc_mark(t);
  if (g_prev_globals_hook) g_prev_globals_hook();
}

void sp_sched_init(void) {
  /* Called from main() on the root fiber, so sp_fiber_current is the root. */
  g_root_fiber = sp_fiber_current;
  memset(&g_main_thread, 0, sizeof g_main_thread);
  g_main_thread.fiber = NULL;
  g_main_thread.state = SP_TH_RUNNING;
  g_main_thread.report_on_exception = 1;
  g_main_thread.arg = sp_box_nil();
  g_main_thread.retval = sp_box_nil();
  g_main_thread.name = sp_box_nil();
  g_current = &g_main_thread;
  g_prev_globals_hook = sp_gc_mark_globals_hook;
  sp_gc_mark_globals_hook = sp_sched_globals_mark;
}

static void sp_thread_report(sp_thread *t) {
  fprintf(stderr, "#<Thread:%u> terminated with exception: %s (%s)\n",
          t->id, t->exc_msg ? t->exc_msg : "", t->exc_cls ? t->exc_cls : "Exception");
}

/* A finished thread's parked joiners become runnable again (the main thread,
   if it was waiting, is released by the pump's target check, not the queue). */
static void sp_thread_wake_joiners(sp_thread *t) {
  sp_thread *j = t->joiners;
  t->joiners = NULL;
  while (j) {
    sp_thread *n = j->join_next;
    j->join_next = NULL;
    if (j != &g_main_thread) rq_push(j);
    j = n;
  }
}

/* Run runnable green threads until `target` is DEAD, until the main thread is
   woken back to RUNNABLE (it had blocked on a primitive), or until the run queue
   drains. Runs on the root fiber (the main thread). */
static void sp_sched_pump(sp_thread *target) {
  for (;;) {
    if (target && target->state == SP_TH_DEAD) return;
    /* main blocked on a Queue/Mutex and a runnable thread just woke it */
    if (g_main_thread.state == SP_TH_RUNNABLE) { g_main_thread.state = SP_TH_RUNNING; return; }
    sp_thread *t = rq_pop();
    if (!t) return;   /* nothing runnable: drained, or a deadlock the caller observes */
    sp_thread *saved = g_current;
    g_current = t;
    t->state = SP_TH_RUNNING;
    int raised = 0;
    const char *ec = NULL, *em = NULL;
    void *eo = NULL;
    /* On the body's first entry the block's param reads the fiber's resumed
       value, so hand it Thread.new's argument then; later resumes pass nil. */
    sp_RbVal in = (t->fiber->state == 0) ? t->arg : sp_box_nil();
    sp_Fiber_transfer_catch(t->fiber, in, &raised, &ec, &em, &eo);
    g_current = saved;
    if (t->fiber->state == 3) {   /* the body returned (terminated) */
      t->retval = t->fiber->yielded_value;
      t->state = SP_TH_DEAD;
      if (raised) {
        t->has_exc = 1; t->exc_cls = ec; t->exc_msg = em; t->exc_obj = eo;
        if (t->report_on_exception) sp_thread_report(t);
      }
      sp_thread_wake_joiners(t);
      reg_remove(t);   /* collectable once no user reference remains */
    }
    /* otherwise t yielded back: it parked itself (joiners list) or re-queued
       itself (Thread.pass) before transferring, so the queue state is correct. */
  }
}

static void sp_thread_scan(void *p) {
  sp_thread *t = (sp_thread *)p;
  if (t->fiber) sp_gc_mark(t->fiber);
  sp_mark_rbval(t->arg);
  sp_mark_rbval(t->retval);
  sp_mark_rbval(t->name);
  if (t->exc_obj) sp_gc_mark(t->exc_obj);
  if (t->tls) sp_gc_mark(t->tls);
}

sp_thread *sp_Thread_spawn_fiber(sp_Fiber *f, sp_RbVal arg) {
  SP_GC_ROOT(f);   /* root the freshly-built fiber across the allocation below */
  SP_GC_ROOT_RBVAL(arg);
  sp_thread *volatile t = (sp_thread *)sp_gc_alloc(sizeof(sp_thread), NULL, sp_thread_scan);
  memset(t, 0, sizeof *t);
  t->fiber = f;
  t->arg = arg;
  t->retval = sp_box_nil();
  t->name = sp_box_nil();
  t->report_on_exception = g_report_default;
  t->id = g_next_id++;
  reg_add(t);
  rq_push(t);
  return t;
}

/* Block the calling thread until `t` is dead. The main thread pumps the queue;
   a spawned thread parks on t's joiners and yields to the scheduler. */
static void sp_thread_await(sp_thread *t) {
  if (t->state == SP_TH_DEAD) return;
  sp_thread *self = g_current;
  if (self == &g_main_thread) {
    sp_sched_pump(t);
    if (t->state != SP_TH_DEAD)
      sp_raise_cls("ThreadError", "deadlock detected: no runnable thread");
  } else {
    self->state = SP_TH_BLOCKED;
    self->join_next = t->joiners;
    t->joiners = self;
    sp_Fiber_transfer(g_root_fiber, sp_box_nil());   /* resumes once t is dead */
  }
}

/* CRuby: #join and #value re-raise the thread's unhandled exception in the
   joining thread. */
static void sp_thread_reraise_if_exc(sp_thread *t) {
  if (t->has_exc) sp_fiber_reraise(t->exc_cls, t->exc_msg, t->exc_obj);
}

sp_thread *sp_Thread_join(sp_thread *t) {
  sp_thread_await(t);
  sp_thread_reraise_if_exc(t);
  return t;
}

sp_RbVal sp_Thread_value(sp_thread *t) {
  sp_thread_await(t);
  sp_thread_reraise_if_exc(t);
  return t->retval;
}

void sp_Thread_pass(void) {
  sp_thread *self = g_current;
  if (self == &g_main_thread) {
    sp_sched_pump(NULL);   /* give every currently-runnable thread a turn */
  } else {
    rq_push(self);
    sp_Fiber_transfer(g_root_fiber, sp_box_nil());
  }
}

sp_thread *sp_Thread_current(void) { return g_current; }

mrb_bool sp_Thread_alive(sp_thread *t) { return t->state != SP_TH_DEAD; }

/* Thread.report_on_exception=(v): set the default for threads spawned after.
   Thread.report_on_exception: read the default. Per-thread #report_on_exception
   reads/sets the thread's own flag. */
mrb_bool sp_Thread_set_report_default(mrb_bool v) { g_report_default = v ? 1 : 0; return v; }
mrb_bool sp_Thread_get_report_default(void) { return g_report_default; }
mrb_bool sp_Thread_set_report(sp_thread *t, mrb_bool v) { t->report_on_exception = v ? 1 : 0; return v; }
mrb_bool sp_Thread_get_report(sp_thread *t) { return t->report_on_exception; }

sp_thread *sp_Thread_main(void) { return &g_main_thread; }

sp_RbVal sp_Thread_get_name(sp_thread *t) { return t->name; }
sp_RbVal sp_Thread_set_name(sp_thread *t, sp_RbVal v) { t->name = v; return v; }

/* Thread.list enumeration: the main thread followed by every live spawned
   thread (dead ones are off the registry). The generated TU builds the array
   over these accessors since it owns sp_PolyArray. */
mrb_int sp_Thread_list_count(void) {
  mrb_int n = 1;   /* the main thread */
  for (sp_thread *t = g_all; t; t = t->all_next) n++;
  return n;
}
sp_thread *sp_Thread_list_at(mrb_int i) {
  if (i <= 0) return &g_main_thread;
  sp_thread *t = g_all;
  for (mrb_int k = 1; t && k < i; k++) t = t->all_next;
  return t ? t : &g_main_thread;
}

/* #status: "run" while runnable/running, "sleep" while blocked, false when it
   finished normally, nil when it died with an unhandled exception. */
sp_RbVal sp_Thread_status(sp_thread *t) {
  switch (t->state) {
    case SP_TH_RUNNING: case SP_TH_RUNNABLE: return sp_box_str(&("\xff" "run")[1]);
    case SP_TH_BLOCKED:                       return sp_box_str(&("\xff" "sleep")[1]);
    default:                                  return t->has_exc ? sp_box_nil() : sp_box_bool(0);
  }
}

/* ---- thread-local storage (Thread#[] / #[]=), a small sym->value map ---- */
typedef struct { sp_sym *keys; sp_RbVal *vals; mrb_int len, cap; } sp_tls_map;
static void sp_tls_scan(void *p) { sp_tls_map *m = (sp_tls_map *)p; for (mrb_int i = 0; i < m->len; i++) sp_mark_rbval(m->vals[i]); }
static void sp_tls_fin(void *p)  { sp_tls_map *m = (sp_tls_map *)p; free(m->keys); free(m->vals); }

sp_RbVal sp_Thread_tls_get(sp_thread *t, sp_sym k) {
  sp_tls_map *m = (sp_tls_map *)t->tls;
  if (m) for (mrb_int i = 0; i < m->len; i++) if (m->keys[i] == k) return m->vals[i];
  return sp_box_nil();
}
mrb_bool sp_Thread_tls_key(sp_thread *t, sp_sym k) {
  sp_tls_map *m = (sp_tls_map *)t->tls;
  if (m) for (mrb_int i = 0; i < m->len; i++) if (m->keys[i] == k) return 1;
  return 0;
}
sp_RbVal sp_Thread_tls_set(sp_thread *t, sp_sym k, sp_RbVal v) {
  sp_tls_map *m = (sp_tls_map *)t->tls;
  if (m) for (mrb_int i = 0; i < m->len; i++) if (m->keys[i] == k) { m->vals[i] = v; return v; }
  if (!m) {
    SP_GC_ROOT(t); SP_GC_ROOT_RBVAL(v);
    m = (sp_tls_map *)sp_gc_alloc(sizeof(sp_tls_map), sp_tls_fin, sp_tls_scan);
    m->cap = 4; m->len = 0;
    m->keys = (sp_sym *)malloc(sizeof(sp_sym) * m->cap);
    m->vals = (sp_RbVal *)malloc(sizeof(sp_RbVal) * m->cap);
    if (!m->keys || !m->vals) sp_raise_cls("NoMemoryError", "failed to allocate thread storage");
    t->tls = m;
  }
  if (m->len == m->cap) {
    mrb_int nc = m->cap * 2;
    sp_sym *nk = (sp_sym *)realloc(m->keys, sizeof(sp_sym) * nc);
    if (!nk) sp_raise_cls("NoMemoryError", "failed to grow thread storage");
    m->keys = nk;
    sp_RbVal *nv = (sp_RbVal *)realloc(m->vals, sizeof(sp_RbVal) * nc);
    if (!nv) sp_raise_cls("NoMemoryError", "failed to grow thread storage");
    m->vals = nv; m->cap = nc;
  }
  m->keys[m->len] = k; m->vals[m->len] = v; m->len++;
  return v;
}

void sp_sched_drain(void) {
  /* main() is finishing: run remaining runnable threads so fire-and-forget
     side effects happen. Only meaningful when called from the main thread. */
  if (g_current == &g_main_thread) sp_sched_pump(NULL);
}

/* ---- generic park / wake on a primitive wait list ---- */

/* Block the current green thread on `*waitlist` until a wake moves it back to
   runnable. The main thread pumps the scheduler (it cannot transfer away from
   root); a spawned thread transfers back to the scheduler hub. */
static void sp_sched_block(sp_thread **waitlist) {
  sp_thread *self = g_current;
  self->state = SP_TH_BLOCKED;
  self->wait_next = *waitlist;
  *waitlist = self;
  if (self == &g_main_thread) {
    sp_sched_pump(NULL);   /* returns once a waker marks main RUNNABLE */
    if (self->state != SP_TH_RUNNING)
      sp_raise_cls("ThreadError", "deadlock detected: all threads blocked");
  } else {
    sp_Fiber_transfer(g_root_fiber, sp_box_nil());
  }
}

/* Move one thread off `*waitlist` back onto the run queue (or mark the main
   thread runnable so its pump returns). Returns the woken thread, or NULL. */
static sp_thread *sp_sched_wake_one(sp_thread **waitlist) {
  sp_thread *t = *waitlist;
  if (!t) return NULL;
  *waitlist = t->wait_next;
  t->wait_next = NULL;
  if (t == &g_main_thread) t->state = SP_TH_RUNNABLE;   /* the pump observes this */
  else rq_push(t);
  return t;
}

/* ---- Queue ---- */

static void sp_queue_scan(void *p) {
  sp_queue *q = (sp_queue *)p;
  for (mrb_int i = 0; i < q->len; i++)
    sp_mark_rbval(q->buf[(q->head + i) % q->cap]);
}
static void sp_queue_fin(void *p) { sp_queue *q = (sp_queue *)p; free(q->buf); }

sp_queue *sp_Queue_new(void) {
  sp_queue *q = (sp_queue *)sp_gc_alloc(sizeof(sp_queue), sp_queue_fin, sp_queue_scan);
  q->cap = 8;
  q->head = q->len = 0;
  q->max = 0;
  q->pop_waiters = NULL;
  q->push_waiters = NULL;
  q->closed = 0;
  q->buf = (sp_RbVal *)malloc(sizeof(sp_RbVal) * q->cap);
  if (!q->buf) sp_raise_cls("NoMemoryError", "failed to allocate queue");
  return q;
}

sp_queue *sp_SizedQueue_new(mrb_int max) {
  if (max <= 0) sp_raise_cls("ArgumentError", "queue size must be positive");
  sp_queue *q = sp_Queue_new();
  q->max = max;
  return q;
}

void sp_Queue_push(sp_queue *q, sp_RbVal v) {
  /* On a full SizedQueue, block until a #pop frees a slot. Root v across the
     block: it lives in this (possibly suspended) frame, and the parking
     thread's saved roots only cover the shadow stack. */
  SP_GC_ROOT_RBVAL(v);
  for (;;) {
    if (q->closed) sp_raise_cls("ClosedQueueError", "queue closed");
    if (q->max <= 0 || q->len < q->max) break;
    sp_sched_block(&q->push_waiters);
  }
  if (q->len == q->cap) {
    mrb_int nc = q->cap * 2;
    sp_RbVal *nb = (sp_RbVal *)malloc(sizeof(sp_RbVal) * nc);
    if (!nb) sp_raise_cls("NoMemoryError", "failed to grow queue");
    for (mrb_int i = 0; i < q->len; i++) nb[i] = q->buf[(q->head + i) % q->cap];
    free(q->buf);
    q->buf = nb; q->cap = nc; q->head = 0;
  }
  q->buf[(q->head + q->len) % q->cap] = v;
  q->len++;
  sp_sched_wake_one(&q->pop_waiters);   /* hand the new value to a waiting popper */
}

sp_RbVal sp_Queue_pop(sp_queue *q) {
  /* Block until an element is available. A closed, drained queue returns nil
     rather than blocking forever (CRuby behaviour). */
  while (q->len == 0) {
    if (q->closed) return sp_box_nil();
    sp_sched_block(&q->pop_waiters);
  }
  sp_RbVal v = q->buf[q->head];
  q->head = (q->head + 1) % q->cap;
  q->len--;
  if (q->max > 0) sp_sched_wake_one(&q->push_waiters);   /* a slot freed up */
  return v;
}

mrb_int  sp_Queue_size(sp_queue *q)   { return q->len; }
mrb_bool sp_Queue_empty(sp_queue *q)  { return q->len == 0; }
mrb_int  sp_Queue_max(sp_queue *q)    { return q->max; }
mrb_bool sp_Queue_closed(sp_queue *q) { return q->closed != 0; }
void     sp_Queue_clear(sp_queue *q)  {
  q->head = q->len = 0;
  if (q->max > 0) while (sp_sched_wake_one(&q->push_waiters)) { }   /* all slots free */
}

void sp_Queue_close(sp_queue *q) {
  q->closed = 1;
  /* wake every blocked popper (they return nil) and pusher (they raise) */
  while (sp_sched_wake_one(&q->pop_waiters)) { }
  while (sp_sched_wake_one(&q->push_waiters)) { }
}

/* ---- Mutex ----
 * A non-recursive lock. At N=1 there is no preemption, so a Mutex only matters
 * across a yield: a green thread holding the lock blocks (Queue/CondVar/IO) and
 * another tries to acquire it. unlock hands ownership directly to the next
 * waiter, so the wakeup is a clean transfer (no re-contention). */

sp_mutex *sp_Mutex_new(void) {
  sp_mutex *m = (sp_mutex *)sp_gc_alloc(sizeof(sp_mutex), NULL, NULL);
  m->owner = NULL;
  m->waiters = NULL;
  return m;
}

void sp_Mutex_lock(sp_mutex *m) {
  sp_thread *self = g_current;
  if (m->owner == self) sp_raise_cls("ThreadError", "deadlock; recursive locking");
  if (m->owner == NULL) { m->owner = self; return; }
  /* unlock hands ownership to us (sets m->owner) before waking us. */
  sp_sched_block(&m->waiters);
}

void sp_Mutex_unlock(sp_mutex *m) {
  if (m->owner != g_current)
    sp_raise_cls("ThreadError", "Attempt to unlock a mutex which is not locked");
  m->owner = sp_sched_wake_one(&m->waiters);   /* hand off, or NULL => unlocked */
}

mrb_bool sp_Mutex_try_lock(sp_mutex *m) {
  if (m->owner != NULL) return 0;
  m->owner = g_current;
  return 1;
}
mrb_bool sp_Mutex_locked(sp_mutex *m) { return m->owner != NULL; }
mrb_bool sp_Mutex_owned(sp_mutex *m)  { return m->owner == g_current; }

/* ---- ConditionVariable ----
 * #wait releases the mutex, parks on the CV, and re-acquires the mutex on
 * wake; #signal wakes one waiter, #broadcast wakes all. */

sp_condvar *sp_CondVar_new(void) {
  sp_condvar *cv = (sp_condvar *)sp_gc_alloc(sizeof(sp_condvar), NULL, NULL);
  cv->waiters = NULL;
  return cv;
}

void sp_CondVar_wait(sp_condvar *cv, sp_mutex *m) {
  sp_Mutex_unlock(m);
  sp_sched_block(&cv->waiters);
  sp_Mutex_lock(m);   /* re-acquire (may block again on the mutex) */
}

void sp_CondVar_signal(sp_condvar *cv)    { sp_sched_wake_one(&cv->waiters); }
void sp_CondVar_broadcast(sp_condvar *cv) { while (sp_sched_wake_one(&cv->waiters)) { } }
