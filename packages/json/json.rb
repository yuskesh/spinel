# Spinel bundled `json` — a typed native binding (Path B).
#
# JSON is backed by C in lib/sp_json.c. Rather than the compiler hardcoding
# the dispatch, this package declares the binding: `native_lib` names the
# require-gate feature, and each `native_func` maps a Ruby method to a C
# symbol with spinel-typed args/return. The compiler reads these at analyze
# time and emits direct, typed C calls (JSON.generate(x) -> sp_json_val(<boxed
# x>)), no FFI boxing.
#
# native_func :name, [arg_specs], ret_spec, "c_symbol"
#   specs: any (sp_RbVal) | string | int | float | bool | nil
module JSON
  native_lib "json"
  native_obj "packages/json/sp_json.o"   # carried C, linked only when required
  native_obj_reflect                     # serialize a Struct via generic obj->hash reflection
  native_func :generate, [:any], :string, "sp_json_val"
  native_func :dump,     [:any], :string, "sp_json_val"
  native_func :parse,    [:string], :any,  "sp_json_parse"
end
