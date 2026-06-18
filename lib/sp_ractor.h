/* sp_ractor.h -- minimal pthread-backed Ractor runtime (RFC, Milestone 1).
 *
 * A Ractor is "a block body run on its own pthread, with deep-copy message
 * passing instead of shared state." Spinel's runtime is a pile of file-scope
 * statics; making the per-execution-unit mutable ones __thread (see sp_gc.h,
 * sp_fiber.h, sp_runtime.h) gives every Ractor a private GC heap, root stack,
 * string heap, and exception landing pads -- so Ractors collect independently
 * with no global GC lock. The only genuinely shared, lock-guarded state is the
 * per-Ractor mailbox/outgoing queues defined here.
 *
 * Values crossing the boundary are deep-copied through a heap-neutral, malloc'd
 * serialization blob: the sender serializes from its private heap into a blob
 * (no GC pointers), the blob sits in the queue, and the receiver deserializes
 * it into its own heap. The codec itself (sp_ractor_serialize / _deserialize)
 * lives in the generated TU -- it needs the heap allocators that are static
 * there -- and is installed into the hooks below at startup, the same way the
 * GC mark-globals hook is. Integer/Float/true/false/nil/Symbol/String and
 * Arrays (poly + typed) are shareable; objects/procs/hashes raise Ractor::Error.
 */
#ifndef SP_RACTOR_H
#define SP_RACTOR_H

#include <stddef.h>
#include "sp_gc.h"   /* sp_RbVal + tag constants */

typedef struct sp_Ractor sp_Ractor;

/* A heap-neutral serialized message: malloc'd bytes, no GC/heap pointers, so
   it is safe to hold in a queue across the Ractor heap boundary. */
typedef struct { void *data; size_t len; } sp_RactorBlob;

/* Codec hooks, installed by the generated TU at startup (see emit_regex_section
   / sp_re_init). The library calls through them so the heap-touching codec can
   live in the TU where the allocators are visible. */
extern sp_RactorBlob (*sp_ractor_serialize_hook)(sp_RbVal v);
extern sp_RbVal      (*sp_ractor_deserialize_hook)(sp_RactorBlob b);

/* The Ractor running on the calling pthread; NULL on the main thread.
   Read by Ractor.receive / Ractor.yield as the implicit receiver. */
extern __thread sp_Ractor *sp_ractor_current;

/* Public API, reached by name from the generated translation unit. */
sp_Ractor *sp_Ractor_new(void (*body)(sp_Ractor *)); /* Ractor.new { ... } */
void       sp_Ractor_send(sp_Ractor *r, sp_RbVal v); /* r.send(v) / r << v */
sp_RbVal   sp_Ractor_receive(void);                  /* Ractor.receive     */
void       sp_Ractor_yield(sp_RbVal v);              /* Ractor.yield(v)    */
sp_RbVal   sp_Ractor_take(sp_Ractor *r);             /* r.take             */

#endif
