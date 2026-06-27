/* sp_sched.h -- cooperative M:N thread scheduler, Phase 0 (N=1).
 *
 * A Ruby Thread is a green thread (sp_thread) wrapping an sp_Fiber that the
 * codegen builds exactly like a Fiber.new block (so all the capture/self/cell
 * machinery is shared); the block's result lands in fiber->yielded_value. The
 * main program runs on the root fiber and IS the scheduler hub: it executes
 * top-level code directly and, whenever it blocks (#join / #value /
 * Thread.pass) or at program exit (drain), it pumps a FIFO run queue of
 * runnable green threads. A spawned thread yields by transferring back to the
 * root fiber, where the pump loop -- or the fiber trampoline, on termination --
 * resumes.
 *
 * With a single OS worker there is no concurrent heap mutation, so the GC and
 * allocator are untouched. This is the Phase 0 cooperative core of
 * docs/thread-mn-design.md; N>1 parallelism, preemption, and the
 * Mutex/Queue/ConditionVariable primitives build on this scaffolding later.
 */
#ifndef SP_SCHED_H
#define SP_SCHED_H

#include "sp_fiber.h"

typedef enum { SP_TH_RUNNABLE, SP_TH_RUNNING, SP_TH_BLOCKED, SP_TH_DEAD } sp_thread_state;

typedef struct sp_thread {
  sp_Fiber         *fiber;       /* the green thread's coroutine; NULL for the main thread (root) */
  sp_RbVal          arg;         /* Thread.new(arg) -> the block's first param, on first run */
  sp_RbVal          retval;      /* block result (copied from fiber->yielded_value at death) */
  sp_thread_state   state;
  int               has_exc;     /* body left an unhandled exception (re-raised at #join/#value) */
  const char       *exc_cls, *exc_msg;
  void             *exc_obj;
  unsigned char     report_on_exception;
  struct sp_thread *rq_next;     /* run-queue link while RUNNABLE */
  struct sp_thread *joiners;     /* threads parked in #join/#value on this one */
  struct sp_thread *join_next;   /* link within another thread's joiners list */
  struct sp_thread *wait_next;   /* link within a primitive's wait list (Queue/Mutex) */
  struct sp_thread *all_next, *all_prev;  /* registry of live threads (GC roots) */
  unsigned          id;
} sp_thread;

/* Called once from main()'s prologue (on the root fiber, after sp_re_init) when
   the program uses threads. Adopts the running context as the main thread and
   chains a GC mark hook that roots every live green thread. */
void       sp_sched_init(void);

/* Thread.new { ... }: wrap a fiber (built by the codegen via emit_fiber_new) in
   a green thread and enqueue it RUNNABLE. It runs the next time the current
   thread yields, or at drain. Returns the thread (boxed SP_BUILTIN_THREAD). */
sp_thread *sp_Thread_spawn_fiber(sp_Fiber *f, sp_RbVal arg);

/* #join: block until the thread has finished, re-raise its unhandled exception
   in the caller, then return the thread. #value: same, but return its result. */
sp_thread *sp_Thread_join(sp_thread *t);
sp_RbVal   sp_Thread_value(sp_thread *t);

void       sp_Thread_pass(void);          /* Thread.pass: cooperative yield */
sp_thread *sp_Thread_current(void);       /* Thread.current */
mrb_bool   sp_Thread_alive(sp_thread *t); /* #alive? */
mrb_bool   sp_Thread_set_report_default(mrb_bool v);  /* Thread.report_on_exception= */
mrb_bool   sp_Thread_get_report_default(void);        /* Thread.report_on_exception */
mrb_bool   sp_Thread_set_report(sp_thread *t, mrb_bool v); /* #report_on_exception= */
mrb_bool   sp_Thread_get_report(sp_thread *t);            /* #report_on_exception */

/* Run any remaining runnable threads to completion. Emitted at the end of
   main() so a fire-and-forget Thread still runs its body. */
void       sp_sched_drain(void);

/* ---- Queue (thread-safe FIFO) ----
 * A producer/consumer hand-off. #pop on an empty queue blocks the calling green
 * thread (parking it, yielding to the scheduler) until a #push wakes it; this is
 * the coordination the lazy Thread model could not express. */
typedef struct sp_queue {
  sp_RbVal         *buf;          /* ring buffer of queued values */
  mrb_int           head, len, cap;
  mrb_int           max;          /* SizedQueue capacity; 0 = unbounded Queue */
  struct sp_thread *pop_waiters;  /* threads blocked in #pop on an empty queue */
  struct sp_thread *push_waiters; /* threads blocked in #push on a full SizedQueue */
  int               closed;
} sp_queue;

sp_queue  *sp_Queue_new(void);
sp_queue  *sp_SizedQueue_new(mrb_int max);          /* SizedQueue.new(max) */
void       sp_Queue_push(sp_queue *q, sp_RbVal v);  /* #push / #<< / #enq (blocks when full) */
sp_RbVal   sp_Queue_pop(sp_queue *q);               /* #pop / #shift / #deq (blocks when empty) */
mrb_int    sp_Queue_size(sp_queue *q);              /* #size / #length */
mrb_bool   sp_Queue_empty(sp_queue *q);             /* #empty? */
mrb_int    sp_Queue_max(sp_queue *q);               /* SizedQueue#max */
void       sp_Queue_close(sp_queue *q);             /* #close */
mrb_bool   sp_Queue_closed(sp_queue *q);            /* #closed? */
void       sp_Queue_clear(sp_queue *q);             /* #clear */

/* ---- Mutex (non-recursive lock; owner + wait list) ----
 * Threads are kept alive by the scheduler registry, so neither struct needs a
 * GC scan over its owner/waiter pointers. */
typedef struct sp_mutex {
  struct sp_thread *owner;     /* NULL = unlocked */
  struct sp_thread *waiters;   /* threads blocked in #lock */
} sp_mutex;

sp_mutex  *sp_Mutex_new(void);
void       sp_Mutex_lock(sp_mutex *m);
void       sp_Mutex_unlock(sp_mutex *m);
mrb_bool   sp_Mutex_try_lock(sp_mutex *m);   /* #try_lock: true if acquired */
mrb_bool   sp_Mutex_locked(sp_mutex *m);     /* #locked? */
mrb_bool   sp_Mutex_owned(sp_mutex *m);      /* #owned? */

/* ---- ConditionVariable (wait/signal/broadcast over a Mutex) ---- */
typedef struct sp_condvar {
  struct sp_thread *waiters;   /* threads blocked in #wait */
} sp_condvar;

sp_condvar *sp_CondVar_new(void);
void        sp_CondVar_wait(sp_condvar *cv, sp_mutex *m);  /* release m, park, re-acquire m */
void        sp_CondVar_signal(sp_condvar *cv);             /* #signal */
void        sp_CondVar_broadcast(sp_condvar *cv);          /* #broadcast */

#endif /* SP_SCHED_H */
