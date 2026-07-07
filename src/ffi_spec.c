#include <stddef.h>

#include "ffi_spec.h"

/* The single source of truth for the FFI type-spec vocabulary. Each row pairs a
 * spec token with its inferred Ruby type and its C type at the ABI boundary;
 * ffi_spec_to_ty and ffi_c_type are thin lookups into it. LP64 target. */
static const FfiSpecInfo FFI_SPECS[] = {
  { "int",         TY_INT,         "int"             },
  { "uint32",      TY_INT,         "uint32_t"        },
  { "int32",       TY_INT,         "int32_t"         },
  { "uint16",      TY_INT,         "uint16_t"        },
  { "int16",       TY_INT,         "int16_t"         },
  { "uint8",       TY_INT,         "uint8_t"         },
  { "int8",        TY_INT,         "int8_t"          },
  { "size_t",      TY_INT,         "size_t"          },
  { "long",        TY_INT,         "long"            },
  { "int64",       TY_INT,         "int64_t"         },
  { "float",       TY_FLOAT,       "float"           },
  { "double",      TY_FLOAT,       "double"          },
  { "bool",        TY_BOOL,        "int"             },
  { "str",         TY_STRING,      "const char *"    },
  { "binstr",      TY_STRING,      "const char *"    },  /* bytes + sp_net_bin_len */
  { "ptr",         TY_POLY,        "void *"          },
  { "float_array", TY_FLOAT_ARRAY, "const double *"  },
  { "int_array",   TY_INT_ARRAY,   "const int64_t *" },
  { "void",        TY_NIL,         "void"            },
};

const FfiSpecInfo *ffi_spec_lookup(const char *spec) {
  if (!spec) return NULL;
  for (unsigned i = 0; i < sizeof(FFI_SPECS) / sizeof(FFI_SPECS[0]); i++)
    if (sp_streq(FFI_SPECS[i].spec, spec)) return &FFI_SPECS[i];
  return NULL;
}
