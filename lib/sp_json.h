#ifndef SP_JSON_H
#define SP_JSON_H
/* JSON.generate serialization, defined in lib/sp_json.c.

   The serializer owns no container types. It walks a boxed value through the
   generic sp_json_* / sp_sym_name_fn hooks (declared in sp_gc.h, installed by
   the generated TU at startup), so it can live in its own translation unit and
   allocate result strings on the shared string heap (sp_alloc.h). */
#include "sp_gc.h"   /* sp_RbVal */

const char *sp_json_str(const char *s);   /* quote + escape a string */
const char *sp_json_val(sp_RbVal v);      /* serialize any boxed value */
#endif /* SP_JSON_H */
