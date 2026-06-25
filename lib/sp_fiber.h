/* sp_fiber.h -- Fiber runtime surface.
 *
 * The sp_Fiber struct (a cooperative coroutine) is shared between the generated
 * translation unit and lib/sp_fiber.c, which holds the function bodies. The
 * context switch lives in sp_fiber_ctx.h (a portable asm switch on x86_64 /
 * aarch64, ucontext elsewhere) -- no <ucontext.h> here, so OpenBSD compiles.
 * `storage` is an opaque pointer to fiber-local variable storage. */
#ifndef SP_FIBER_H
#define SP_FIBER_H

#include "sp_gc.h"   /* sp_RbVal */
#include "sp_fiber_ctx.h"


#define SP_FIBER_STACK_SIZE (64*1024)

typedef struct sp_Fiber{sp_fiber_ctx ctx;sp_fiber_ctx caller_ctx;char*stack;int state;int transferred;sp_RbVal yielded_value;sp_RbVal resumed_value;void(*body)(struct sp_Fiber*);void*user_data;int saved_exc_top;int saved_catch_top;void*exc_ctx;int raised;const char*raised_cls;const char*raised_msg;void*raised_obj;int inject;const char*inj_cls;const char*inj_msg;void*inj_obj;void*storage;void***saved_roots;int saved_nroots;int saved_roots_cap;struct sp_Fiber*fiber_next;struct sp_Fiber*fiber_prev;}sp_Fiber;

/* The currently-running fiber; the generated TU reads it directly for
   `Fiber.current` and as the implicit receiver of Fiber operations. */
extern sp_Fiber *sp_fiber_current;

/* Public Fiber API (called from the generated TU). */
sp_Fiber *sp_Fiber_new(void (*body)(sp_Fiber *));
sp_RbVal sp_Fiber_resume(sp_Fiber *f, sp_RbVal val);
sp_RbVal sp_Fiber_yield(sp_RbVal val);
sp_RbVal sp_Fiber_transfer(sp_Fiber *f, sp_RbVal val);
/* Fiber#raise: inject an exception at the fiber's suspension point (or at entry
   for an unstarted fiber). cls/msg describe a class+message; obj, when non-NULL,
   is a pre-built exception object to raise instead. Returns the next yielded
   value, or re-raises in the caller if the fiber does not handle it. */
sp_RbVal sp_Fiber_raise(sp_Fiber *f, const char *cls, const char *msg, void *obj);
mrb_bool sp_Fiber_alive(sp_Fiber *f);
sp_RbVal sp_Fiber_storage_get(sp_Fiber *f, sp_sym k);
void sp_Fiber_storage_set(sp_Fiber *f, sp_sym k, sp_RbVal v);
/* Reached from sp_re_mark_globals in the generated TU during a GC pass. */
void sp_mark_fiber_root_storage(void);

#endif
