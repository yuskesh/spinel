#ifndef SP_JSON_H
#define SP_JSON_H
/* JSON serialization, defined in packages/json/sp_json.c -- a carried-C
   spin package (Path B), linked on demand when `require "json"` appears.

   The serializer owns no container types. It walks a boxed value through the
   generic container and symbol reflection hooks installed by the generated TU
   at startup, so it lives in its own translation unit and allocates result
   strings on the shared string heap. It compiles against the stable package
   ABI rather than the compiler internal headers. */
#include "spinel/runtime.h"   /* sp_RbVal, SP_TAG_*, hooks, sp_str_alloc, ... */

const char *sp_json_str(const char *s);   /* quote + escape a string */
const char *sp_json_val(sp_RbVal v);      /* serialize any boxed value */
sp_RbVal sp_json_parse(const char *s);    /* parse JSON text into a boxed value */
/* A plain object (Struct) is serialized by reflecting it into a hash via the
   generic sp_obj_to_hash_fn hook (declared in sp_gc.h, installed by the
   generated program); sp_json_val then serializes that hash. */
#endif /* SP_JSON_H */
