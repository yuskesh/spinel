/* sp_fiber.c -- Fiber runtime bodies (POSIX ucontext).
 * See sp_fiber.h. The collector roots, sp_gc_alloc, and sp_raise_cls live
 * in the generated TU and are reached by name; fiber-local storage is
 * self-contained here. */
#include "sp_fiber.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* OpenBSD requires the stack a coroutine runs on to be MAP_STACK, else the
   first push faults. Linux defines MAP_STACK as a (currently) no-op; keep it
   portable. */
#ifndef MAP_STACK
#define MAP_STACK 0
#endif

/* ---- Portable coroutine context switch (replaces swapcontext) ----
   sp_ctx_swap saves the callee-saved registers onto the *current* stack, stows
   the resulting stack pointer in *from, loads *to's stack pointer, and pops the
   callee-saved registers (the very first switch into a coroutine pops the zeroed
   slots sp_ctx_make laid out and `ret`s into the trampoline). It lives in .text
   (W^X-safe) and issues no syscall. See sp_fiber_ctx.h for sp_ctx_make and the
   stack layout it must mirror. */
#if SP_FIBER_ASM
#if defined(__APPLE__)
  #define SP_CTX_SYM "_sp_ctx_swap"
#else
  #define SP_CTX_SYM "sp_ctx_swap"
#endif
#if defined(__x86_64__)
__asm__(
  ".text\n"
  ".globl " SP_CTX_SYM "\n"
  SP_CTX_SYM ":\n"
  "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
  "  movq %rsp, (%rdi)\n"        /* from->sp = rsp                      */
  "  movq (%rsi), %rsp\n"        /* rsp = to->sp                        */
  "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n"
  "  ret\n"
);
#elif defined(__aarch64__)
__asm__(
  ".text\n"
  ".globl " SP_CTX_SYM "\n"
  SP_CTX_SYM ":\n"
  /* save x19..x30 (12*8) + d8..d15 (8*8) = 160 bytes onto the current stack */
  "  stp x19, x20, [sp, #-160]!\n"
  "  stp x21, x22, [sp, #16]\n"
  "  stp x23, x24, [sp, #32]\n"
  "  stp x25, x26, [sp, #48]\n"
  "  stp x27, x28, [sp, #64]\n"
  "  stp x29, x30, [sp, #80]\n"
  "  stp d8,  d9,  [sp, #96]\n"
  "  stp d10, d11, [sp, #112]\n"
  "  stp d12, d13, [sp, #128]\n"
  "  stp d14, d15, [sp, #144]\n"
  "  mov x2, sp\n"
  "  str x2, [x0]\n"             /* from->sp = sp */
  "  ldr x2, [x1]\n"            /* sp = to->sp   */
  "  mov sp, x2\n"
  "  ldp d14, d15, [sp, #144]\n"
  "  ldp d12, d13, [sp, #128]\n"
  "  ldp d10, d11, [sp, #112]\n"
  "  ldp d8,  d9,  [sp, #96]\n"
  "  ldp x29, x30, [sp, #80]\n"
  "  ldp x27, x28, [sp, #64]\n"
  "  ldp x25, x26, [sp, #48]\n"
  "  ldp x23, x24, [sp, #32]\n"
  "  ldp x21, x22, [sp, #16]\n"
  "  ldp x19, x20, [sp], #160\n"
  "  ret\n"
);
#endif
#else  /* ucontext fallback */
void sp_ctx_swap(sp_fiber_ctx *from, sp_fiber_ctx *to) { swapcontext(&from->uc, &to->uc); }
#endif

/* ---- Reached by name in the generated TU ---- */
void *sp_gc_alloc(size_t sz, void (*fin)(void *), void (*scn)(void *));
SP_NORETURN void sp_raise_cls(const char *cls, const char *msg);

/* Per-fiber exception/catch handler context (#1474): defined in the generated
   TU (sp_runtime.h), reached here by name. A fiber's begin/rescue handlers and
   catch tags are saved/restored around each context switch so a raise/throw
   can't longjmp into a suspended fiber's stack frame, and an unhandled fiber
   raise is re-raised in the resumer's context rather than crossing stacks. */
#include <setjmp.h>
void *sp_exc_ctx_new(void);
void sp_exc_ctx_free(void *p);
void sp_exc_ctx_save(void *p);
void sp_exc_ctx_load(void *p);
void sp_exc_ctx_mark(void *p);
void sp_exc_arm(jmp_buf b);
void sp_exc_disarm(void);
const char *sp_exc_cur_cls(void);
const char *sp_exc_cur_msg(void);
void *sp_exc_cur_obj(void);
void sp_fiber_reraise(const char *cls, const char *msg, void *obj);

static inline sp_RbVal sp_box_nil(void) { sp_RbVal r; r.tag = SP_TAG_NIL; r.cls_id = 0; r.v.i = 0; return r; }
static void sp_raise(const char *msg) { sp_raise_cls("RuntimeError", msg); }

/* Fiber-local variable storage (Fiber#[] / Fiber#[]=). A small sym->RbVal
   map, GC-allocated so the collector marks its values via the scan hook;
   self-contained here rather than borrowing the runtime's sym_poly_hash so
   the runtime sym_poly_hash stays collector-private to the generated TU. */
typedef struct { sp_sym *keys; sp_RbVal *vals; mrb_int len, cap; } sp_FiberStore;
static void sp_FiberStore_scan(void *p) { sp_FiberStore *s = (sp_FiberStore *)p; for (mrb_int i = 0; i < s->len; i++) sp_mark_rbval(s->vals[i]); }
static void sp_FiberStore_fin(void *p) { sp_FiberStore *s = (sp_FiberStore *)p; free(s->keys); free(s->vals); }
static sp_FiberStore *sp_FiberStore_new(void) {
  sp_FiberStore *s = (sp_FiberStore *)sp_gc_alloc(sizeof(sp_FiberStore), sp_FiberStore_fin, sp_FiberStore_scan);
  s->cap = 8; s->len = 0;
  s->keys = (sp_sym *)malloc(sizeof(sp_sym) * s->cap);
  s->vals = (sp_RbVal *)malloc(sizeof(sp_RbVal) * s->cap);
  if (!s->keys || !s->vals) {
    /* If one side succeeded, release it now rather than waiting for the
       finalizer -- and NULL both so that finalizer's free stays a no-op. */
    free(s->keys); free(s->vals);
    s->keys = NULL; s->vals = NULL;
    sp_raise_cls("NoMemoryError", "failed to allocate fiber storage");
  }
  return s;
}
static sp_RbVal sp_FiberStore_get(sp_FiberStore *s, sp_sym k) {
  for (mrb_int i = 0; i < s->len; i++) if (s->keys[i] == k) return s->vals[i];
  return sp_box_nil();
}
static void sp_FiberStore_set(sp_FiberStore *s, sp_sym k, sp_RbVal v) {
  for (mrb_int i = 0; i < s->len; i++) if (s->keys[i] == k) { s->vals[i] = v; return; }
  if (s->len == s->cap) {
    /* Grow each array into a temp and commit only on success, so a failed
       realloc leaves the store intact (the old buffer survives) instead of
       NULL-deref'ing on the write below or double-freeing a moved buffer. */
    mrb_int nc = s->cap * 2;
    sp_sym *nk = (sp_sym *)realloc(s->keys, sizeof(sp_sym) * nc);
    if (!nk) sp_raise_cls("NoMemoryError", "failed to grow fiber storage");
    s->keys = nk;
    sp_RbVal *nv = (sp_RbVal *)realloc(s->vals, sizeof(sp_RbVal) * nc);
    if (!nv) sp_raise_cls("NoMemoryError", "failed to grow fiber storage");
    s->vals = nv; s->cap = nc;
  }
  s->keys[s->len] = k; s->vals[s->len] = v; s->len++;
}
static sp_FiberStore *sp_FiberStore_dup(sp_FiberStore *o) {
  sp_FiberStore *s = sp_FiberStore_new();
  for (mrb_int i = 0; i < o->len; i++) sp_FiberStore_set(s, o->keys[i], o->vals[i]);
  return s;
}

/* The root fiber is a static, not a GC allocation — but `Fiber.current`
   hands it to user code, where a rooted local makes the collector mark
   it. Lay a 0xfd skip byte directly before it so sp_gc_mark's tag-byte
   protocol bails out instead of treating .bss as a GC header. The guard
   array is exactly one alignment unit, so its last byte always directly
   precedes `root` (no compiler padding can slip in between). */
static struct { char guard[_Alignof(sp_Fiber)]; sp_Fiber root; } sp_fiber_root_box
    = { .guard = { [_Alignof(sp_Fiber) - 1] = (char)0xfd }, .root = {0} };
#define sp_fiber_root (sp_fiber_root_box.root)
sp_Fiber *sp_fiber_current = &sp_fiber_root;   /* extern: read by the generated TU */
static sp_Fiber *sp_fiber_list_head = NULL;

static void sp_fiber_save_roots(sp_Fiber*f){if(f->saved_roots_cap<sp_gc_nroots){int nc=sp_gc_nroots>64?sp_gc_nroots*2:64;void***nx=(void***)realloc(f->saved_roots,sizeof(void**)*nc);if(!nx)return;f->saved_roots=nx;f->saved_roots_cap=nc;}if(sp_gc_nroots>0)memcpy(f->saved_roots,sp_gc_roots,sizeof(void**)*sp_gc_nroots);f->saved_nroots=sp_gc_nroots;}
static void sp_fiber_restore_roots(sp_Fiber*f){if(f->saved_nroots>0)memcpy(sp_gc_roots,f->saved_roots,sizeof(void**)*f->saved_nroots);sp_gc_nroots=f->saved_nroots;}
static void sp_fiber_list_add(sp_Fiber*f){f->fiber_prev=NULL;f->fiber_next=sp_fiber_list_head;if(sp_fiber_list_head)sp_fiber_list_head->fiber_prev=f;sp_fiber_list_head=f;}
static void sp_fiber_list_remove(sp_Fiber*f){if(f->fiber_prev)f->fiber_prev->fiber_next=f->fiber_next;else if(sp_fiber_list_head==f)sp_fiber_list_head=f->fiber_next;if(f->fiber_next)f->fiber_next->fiber_prev=f->fiber_prev;f->fiber_prev=NULL;f->fiber_next=NULL;}
static void sp_mark_fiber_roots(sp_Fiber*f){if(f==sp_fiber_current)return;int i;for(i=0;i<f->saved_nroots;i++){void**e=f->saved_roots[i];if((uintptr_t)e&(uintptr_t)1){sp_gc_mark_root_entry(e);}else{void*obj=*e;if(obj)sp_gc_mark(obj);}}if(f->exc_ctx)sp_exc_ctx_mark(f->exc_ctx);if(f->raised_obj)sp_gc_mark(f->raised_obj);}
static void sp_mark_suspended_fibers(void){sp_mark_fiber_roots(&sp_fiber_root);sp_Fiber*f=sp_fiber_list_head;while(f){sp_mark_fiber_roots(f);f=f->fiber_next;}}
static void sp_fiber_install_gc_hook(void){if(!sp_gc_mark_suspended_fibers_hook)sp_gc_mark_suspended_fibers_hook=sp_mark_suspended_fibers;}
static void sp_Fiber_fin(void*p){sp_Fiber*f=(sp_Fiber*)p;if(f->stack)munmap(f->stack,SP_FIBER_STACK_SIZE);if(f->saved_roots)free(f->saved_roots);if(f->exc_ctx)sp_exc_ctx_free(f->exc_ctx);sp_fiber_list_remove(f);}
static void sp_Fiber_scan(void*p){sp_Fiber*f=(sp_Fiber*)p;if(f->user_data)sp_gc_mark(f->user_data);if(f->storage)sp_gc_mark(f->storage);}
sp_Fiber*sp_Fiber_new(void(*body)(sp_Fiber*)){sp_Fiber*f=(sp_Fiber*)sp_gc_alloc(sizeof(sp_Fiber),sp_Fiber_fin,sp_Fiber_scan);f->stack=(char*)mmap(NULL,SP_FIBER_STACK_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_STACK,-1,0);if(f->stack==MAP_FAILED){f->stack=NULL;sp_raise_cls("FiberError","failed to allocate fiber stack");}f->state=0;f->transferred=0;f->body=body;f->yielded_value=sp_box_nil();f->resumed_value=sp_box_nil();f->user_data=NULL;f->saved_exc_top=0;f->saved_catch_top=0;f->exc_ctx=sp_exc_ctx_new();f->raised=0;f->raised_cls=NULL;f->raised_msg=NULL;f->raised_obj=NULL;f->storage=NULL;f->saved_roots=NULL;f->saved_nroots=0;f->saved_roots_cap=0;f->fiber_next=NULL;f->fiber_prev=NULL;sp_fiber_list_add(f);sp_fiber_install_gc_hook();if(sp_fiber_current&&sp_fiber_current->storage){sp_Fiber*volatile _froot=f;int _pushed=0;if(sp_gc_nroots<SP_GC_STACK_MAX){sp_gc_roots[sp_gc_nroots++]=(void**)&_froot;_pushed=1;}f->storage=sp_FiberStore_dup((sp_FiberStore*)sp_fiber_current->storage);if(_pushed)sp_gc_nroots--;}return f;}
sp_RbVal sp_Fiber_storage_get(sp_Fiber*f,sp_sym k){if(!f->storage)return sp_box_nil();return sp_FiberStore_get((sp_FiberStore*)f->storage,k);}
void sp_Fiber_storage_set(sp_Fiber*f,sp_sym k,sp_RbVal v){if(!f->storage)f->storage=sp_FiberStore_new();sp_FiberStore_set((sp_FiberStore*)f->storage,k,v);}
static void sp_fiber_trampoline(void){sp_Fiber*f=sp_fiber_current;jmp_buf base;if(setjmp(base)==0){sp_exc_arm(base);f->body(f);sp_exc_disarm();}else{f->raised=1;f->raised_cls=sp_exc_cur_cls();f->raised_msg=sp_exc_cur_msg();f->raised_obj=sp_exc_cur_obj();}f->state=3;if(f->transferred){sp_fiber_current=&sp_fiber_root;sp_ctx_swap(&f->ctx,&sp_fiber_root.ctx);}else{sp_ctx_swap(&f->ctx,&f->caller_ctx);}}
sp_RbVal sp_Fiber_resume(sp_Fiber*f,sp_RbVal val){if(f->state==3){sp_raise_cls("FiberError","attempt to resume a terminated fiber");}if(f->transferred){sp_raise_cls("FiberError","attempt to resume a transferred fiber");}if(f->state==1){sp_raise_cls("FiberError","attempt to resume a resumed fiber (double resume)");}f->resumed_value=val;sp_Fiber*prev=sp_fiber_current;sp_fiber_save_roots(prev);sp_fiber_restore_roots(f);if(!prev->exc_ctx)prev->exc_ctx=sp_exc_ctx_new();sp_exc_ctx_save(prev->exc_ctx);sp_exc_ctx_load(f->exc_ctx);sp_fiber_current=f;if(f->state==0){f->state=1;sp_ctx_make(&f->ctx,f->stack,SP_FIBER_STACK_SIZE,sp_fiber_trampoline);sp_ctx_swap(&f->caller_ctx,&f->ctx);}else{f->state=1;sp_ctx_swap(&f->caller_ctx,&f->ctx);}sp_exc_ctx_save(f->exc_ctx);sp_exc_ctx_load(prev->exc_ctx);sp_fiber_save_roots(f);sp_fiber_restore_roots(prev);sp_fiber_current=prev;if(f->raised){f->raised=0;const char*rc=f->raised_cls;const char*rm=f->raised_msg;void*ro=f->raised_obj;f->raised_obj=NULL;sp_fiber_reraise(rc,rm,ro);}return f->yielded_value;}
/* Fiber.yield is only valid inside a fiber entered via #resume. The root fiber
   was never resumed, and a fiber entered via #transfer has no resumer to return
   to (its caller_ctx is unset) -- yielding from either would swap to a garbage
   context, so raise instead (matching CRuby's FiberError). */
sp_RbVal sp_Fiber_yield(sp_RbVal val){sp_Fiber*f=sp_fiber_current;if(f==&sp_fiber_root||f->transferred){sp_raise_cls("FiberError","attempt to yield on a not resumed fiber");}f->yielded_value=val;f->state=2;sp_ctx_swap(&f->ctx,&f->caller_ctx);return f->resumed_value;}
mrb_bool sp_Fiber_alive(sp_Fiber*f){return f->state!=3;}
sp_RbVal sp_Fiber_transfer(sp_Fiber*f,sp_RbVal val){f->resumed_value=val;sp_Fiber*prev=sp_fiber_current;sp_fiber_save_roots(prev);sp_fiber_restore_roots(f);if(!prev->exc_ctx)prev->exc_ctx=sp_exc_ctx_new();sp_exc_ctx_save(prev->exc_ctx);sp_exc_ctx_load(f->exc_ctx);sp_fiber_current=f;if(f->state==0){f->state=1;f->transferred=1;sp_ctx_make(&f->ctx,f->stack,SP_FIBER_STACK_SIZE,sp_fiber_trampoline);sp_ctx_swap(&prev->ctx,&f->ctx);}else{f->state=1;sp_ctx_swap(&prev->ctx,&f->ctx);}sp_exc_ctx_save(f->exc_ctx);sp_exc_ctx_load(prev->exc_ctx);sp_fiber_save_roots(f);sp_fiber_restore_roots(prev);sp_fiber_current=prev;if(f->raised){f->raised=0;const char*rc=f->raised_cls;const char*rm=f->raised_msg;void*ro=f->raised_obj;f->raised_obj=NULL;sp_fiber_reraise(rc,rm,ro);}return prev->resumed_value;}

void sp_mark_fiber_root_storage(void){if(sp_fiber_root.storage)sp_gc_mark(sp_fiber_root.storage);}
