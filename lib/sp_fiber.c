/* sp_fiber.c -- Fiber runtime bodies (POSIX ucontext).
 * See sp_fiber.h. The collector roots, sp_gc_alloc, and sp_raise_cls live
 * in the generated TU and are reached by name; fiber-local storage is
 * self-contained here. */
#include "sp_fiber.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* ---- Reached by name in the generated TU ---- */
void *sp_gc_alloc(size_t sz, void (*fin)(void *), void (*scn)(void *));
void sp_raise_cls(const char *cls, const char *msg);

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
  return s;
}
static sp_RbVal sp_FiberStore_get(sp_FiberStore *s, sp_sym k) {
  for (mrb_int i = 0; i < s->len; i++) if (s->keys[i] == k) return s->vals[i];
  return sp_box_nil();
}
static void sp_FiberStore_set(sp_FiberStore *s, sp_sym k, sp_RbVal v) {
  for (mrb_int i = 0; i < s->len; i++) if (s->keys[i] == k) { s->vals[i] = v; return; }
  if (s->len == s->cap) {
    s->cap *= 2;
    s->keys = (sp_sym *)realloc(s->keys, sizeof(sp_sym) * s->cap);
    s->vals = (sp_RbVal *)realloc(s->vals, sizeof(sp_RbVal) * s->cap);
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
static void sp_mark_fiber_roots(sp_Fiber*f){if(f==sp_fiber_current)return;int i;for(i=0;i<f->saved_nroots;i++){void**e=f->saved_roots[i];if((uintptr_t)e&(uintptr_t)1){sp_gc_mark_root_entry(e);}else{void*obj=*e;if(obj)sp_gc_mark(obj);}}}
static void sp_mark_suspended_fibers(void){sp_mark_fiber_roots(&sp_fiber_root);sp_Fiber*f=sp_fiber_list_head;while(f){sp_mark_fiber_roots(f);f=f->fiber_next;}}
static void sp_fiber_install_gc_hook(void){if(!sp_gc_mark_suspended_fibers_hook)sp_gc_mark_suspended_fibers_hook=sp_mark_suspended_fibers;}
static void sp_Fiber_fin(void*p){sp_Fiber*f=(sp_Fiber*)p;if(f->stack)munmap(f->stack,SP_FIBER_STACK_SIZE);if(f->saved_roots)free(f->saved_roots);sp_fiber_list_remove(f);}
static void sp_Fiber_scan(void*p){sp_Fiber*f=(sp_Fiber*)p;if(f->user_data)sp_gc_mark(f->user_data);if(f->storage)sp_gc_mark(f->storage);}
sp_Fiber*sp_Fiber_new(void(*body)(sp_Fiber*)){sp_Fiber*f=(sp_Fiber*)sp_gc_alloc(sizeof(sp_Fiber),sp_Fiber_fin,sp_Fiber_scan);f->stack=(char*)mmap(NULL,SP_FIBER_STACK_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(f->stack==MAP_FAILED){f->stack=NULL;sp_raise_cls("FiberError","failed to allocate fiber stack");}f->state=0;f->transferred=0;f->body=body;f->yielded_value=sp_box_nil();f->resumed_value=sp_box_nil();f->user_data=NULL;f->saved_exc_top=0;f->saved_catch_top=0;f->storage=NULL;f->saved_roots=NULL;f->saved_nroots=0;f->saved_roots_cap=0;f->fiber_next=NULL;f->fiber_prev=NULL;sp_fiber_list_add(f);sp_fiber_install_gc_hook();if(sp_fiber_current&&sp_fiber_current->storage){sp_Fiber*volatile _froot=f;int _pushed=0;if(sp_gc_nroots<SP_GC_STACK_MAX){sp_gc_roots[sp_gc_nroots++]=(void**)&_froot;_pushed=1;}f->storage=sp_FiberStore_dup((sp_FiberStore*)sp_fiber_current->storage);if(_pushed)sp_gc_nroots--;}return f;}
sp_RbVal sp_Fiber_storage_get(sp_Fiber*f,sp_sym k){if(!f->storage)return sp_box_nil();return sp_FiberStore_get((sp_FiberStore*)f->storage,k);}
void sp_Fiber_storage_set(sp_Fiber*f,sp_sym k,sp_RbVal v){if(!f->storage)f->storage=sp_FiberStore_new();sp_FiberStore_set((sp_FiberStore*)f->storage,k,v);}
static void sp_fiber_trampoline(void){sp_Fiber*f=sp_fiber_current;f->body(f);f->state=3;if(f->transferred){sp_fiber_current=&sp_fiber_root;setcontext(&sp_fiber_root.ctx);}else{swapcontext(&f->ctx,&f->caller_ctx);}}
sp_RbVal sp_Fiber_resume(sp_Fiber*f,sp_RbVal val){if(f->state==3){sp_raise_cls("FiberError","attempt to resume a terminated fiber");}f->resumed_value=val;sp_Fiber*prev=sp_fiber_current;sp_fiber_save_roots(prev);sp_fiber_restore_roots(f);sp_fiber_current=f;if(f->state==0){f->state=1;getcontext(&f->ctx);f->ctx.uc_stack.ss_sp=f->stack;f->ctx.uc_stack.ss_size=SP_FIBER_STACK_SIZE;f->ctx.uc_link=&f->caller_ctx;makecontext(&f->ctx,(void(*)(void))sp_fiber_trampoline,0);swapcontext(&f->caller_ctx,&f->ctx);}else{f->state=1;swapcontext(&f->caller_ctx,&f->ctx);}sp_fiber_save_roots(f);sp_fiber_restore_roots(prev);sp_fiber_current=prev;return f->yielded_value;}
sp_RbVal sp_Fiber_yield(sp_RbVal val){sp_Fiber*f=sp_fiber_current;f->yielded_value=val;f->state=2;swapcontext(&f->ctx,&f->caller_ctx);return f->resumed_value;}
mrb_bool sp_Fiber_alive(sp_Fiber*f){return f->state!=3;}
sp_RbVal sp_Fiber_transfer(sp_Fiber*f,sp_RbVal val){f->resumed_value=val;sp_Fiber*prev=sp_fiber_current;sp_fiber_save_roots(prev);sp_fiber_restore_roots(f);sp_fiber_current=f;if(f->state==0){f->state=1;f->transferred=1;getcontext(&f->ctx);f->ctx.uc_stack.ss_sp=f->stack;f->ctx.uc_stack.ss_size=SP_FIBER_STACK_SIZE;f->ctx.uc_link=&prev->ctx;makecontext(&f->ctx,(void(*)(void))sp_fiber_trampoline,0);swapcontext(&prev->ctx,&f->ctx);}else{f->state=1;swapcontext(&prev->ctx,&f->ctx);}sp_fiber_save_roots(f);sp_fiber_restore_roots(prev);sp_fiber_current=prev;return prev->resumed_value;}

void sp_mark_fiber_root_storage(void){if(sp_fiber_root.storage)sp_gc_mark(sp_fiber_root.storage);}
