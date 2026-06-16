/* sp_fiber.h -- Fiber runtime surface.
 *
 * The sp_Fiber struct (a cooperative coroutine over POSIX ucontext) is
 * shared between the generated translation unit and lib/sp_fiber.c, which
 * holds the function bodies. `storage` is an opaque pointer to fiber-local
 * variable storage (defined in lib/sp_fiber.c). */
#ifndef SP_FIBER_H
#define SP_FIBER_H

#include "sp_gc.h"   /* sp_RbVal */
#include <ucontext.h>


#define SP_FIBER_STACK_SIZE (64*1024)

typedef struct sp_Fiber{ucontext_t ctx;ucontext_t caller_ctx;char*stack;int state;int transferred;sp_RbVal yielded_value;sp_RbVal resumed_value;void(*body)(struct sp_Fiber*);void*user_data;int saved_exc_top;int saved_catch_top;void*storage;void***saved_roots;int saved_nroots;int saved_roots_cap;struct sp_Fiber*fiber_next;struct sp_Fiber*fiber_prev;}sp_Fiber;

/* The currently-running fiber; the generated TU reads it directly for
   `Fiber.current` and as the implicit receiver of Fiber operations. */
extern sp_Fiber *sp_fiber_current;

/* Public Fiber API (called from the generated TU). */
sp_Fiber *sp_Fiber_new(void (*body)(sp_Fiber *));
sp_RbVal sp_Fiber_resume(sp_Fiber *f, sp_RbVal val);
sp_RbVal sp_Fiber_yield(sp_RbVal val);
sp_RbVal sp_Fiber_transfer(sp_Fiber *f, sp_RbVal val);
mrb_bool sp_Fiber_alive(sp_Fiber *f);
sp_RbVal sp_Fiber_storage_get(sp_Fiber *f, sp_sym k);
void sp_Fiber_storage_set(sp_Fiber *f, sp_sym k, sp_RbVal v);
/* Reached from sp_re_mark_globals in the generated TU during a GC pass. */
void sp_mark_fiber_root_storage(void);

#endif
