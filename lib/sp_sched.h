/* sp_sched.h -- cooperative M:N thread scheduler, Phase 0 (N=1).
 *
 * A Ruby Thread is a green thread (sp_thread) backed by an sp_Fiber. The main
 * program runs on the root fiber and IS the scheduler hub: it executes
 * top-level code directly and, whenever it blocks (#join / #value /
 * Thread.pass) or at program exit (drain), it pumps a FIFO run queue of
 * runnable green threads. A spawned thread yields by transferring back to the
 * root fiber, where the pump loop resumes; when a thread's body returns, the
 * fiber trampoline also returns to root, so the pump observes the termination.
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
  void            (*ruby_body)(struct sp_thread *);  /* generated block body */
  sp_RbVal          arg;         /* single Thread.new(arg) argument, boxed */
  sp_RbVal          retval;      /* block result, read by #value */
  sp_thread_state   state;
  int               has_exc;     /* body left an unhandled exception (re-raised at #join/#value) */
  const char       *exc_cls, *exc_msg;
  void             *exc_obj;
  unsigned char     report_on_exception;
  struct sp_thread *rq_next;     /* run-queue link while RUNNABLE */
  struct sp_thread *joiners;     /* threads parked in #join/#value on this one */
  struct sp_thread *join_next;   /* link within another thread's joiners list */
  unsigned          id;
} sp_thread;

/* Called once from main()'s prologue (on the root fiber) when the program uses
   threads. Adopts the running context as the main thread. */
void       sp_sched_init(void);

/* Thread.new { body }(arg): create a green thread and enqueue it RUNNABLE. It
   runs the next time the current thread yields, or at drain. Returns the
   thread (the generated TU boxes it as SP_BUILTIN_THREAD). */
sp_thread *sp_Thread_spawn(void (*body)(sp_thread *), sp_RbVal arg);

/* Read inside the generated block body to record the Thread's value. */
void       sp_Thread_set_result(sp_thread *t, sp_RbVal v);

/* #join: block until t has finished; re-raises t's unhandled exception in the
   caller, then returns t. #value: same, but returns t's block result. */
sp_thread *sp_Thread_join(sp_thread *t);
sp_RbVal   sp_Thread_value(sp_thread *t);

void       sp_Thread_pass(void);          /* Thread.pass: cooperative yield */
sp_thread *sp_Thread_current(void);       /* Thread.current */
mrb_bool   sp_Thread_alive(sp_thread *t); /* #alive? */

/* Run any remaining runnable threads to completion. Emitted at the end of
   main() so a fire-and-forget Thread still runs its body. */
void       sp_sched_drain(void);

#endif /* SP_SCHED_H */
