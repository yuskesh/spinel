#ifndef FFI_SPEC_H
#define FFI_SPEC_H

#include "types.h"   /* TyKind */

/* The FFI type-spec vocabulary in one place.
 *
 * An FFI declaration (`ffi_func :abs, [:int], :int`, and the forms layered on
 * top of it) describes each value by a spec token -- "int", "long", "ptr",
 * "str", "double", .... Every place that needs to know something about a spec
 * -- its inferred Ruby type (ffi_spec_to_ty) and its C type at the ABI boundary
 * (ffi_c_type) -- resolves it through this single table, so the two can never
 * disagree. Adding a type is one row here; there is no second table to keep in
 * sync. LP64 target (arm64-darwin / x86_64-linux). */
typedef struct {
  const char *spec;    /* the spec token */
  TyKind      ty;      /* inferred Ruby type of a value of this spec */
  const char *c_type;  /* C type at the ABI boundary */
} FfiSpecInfo;

/* The row for `spec`, or NULL if the token is not part of the vocabulary. */
const FfiSpecInfo *ffi_spec_lookup(const char *spec);

#endif /* FFI_SPEC_H */
