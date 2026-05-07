# Issue #341: user-defined `def [](k)` got its parameter type
# inferred as `mrb_int` regardless of caller arg types. When the
# receiver was poly (e.g. a method param widened to accept multiple
# user classes) and callers passed a String key, the per-class
# arms emitted by compile_poly_method_call called each class's
# C function with `const char *` arg → Wint-conversion error
# under -Werror, or hard incompatible-pointer error.
#
# A second symptom: the SP_TAG_INT branch of compile_poly_method_call
# emitted `(recv.v.i >> key) & 1` unconditionally, which is invalid
# C when `key` isn't an int. The same shape applies to the built-in
# array dispatches in emit_poly_builtin_dispatch (sp_IntArray_get
# etc. all expect `mrb_int` keys).
#
# Fix:
#   1. scan_new_calls' receiver-method widening adds a poly arm:
#      walk every user class that defines `mname` and unify each
#      class's ptypes with the call site's arg types.
#   2. compile_poly_method_call gates the int-bit-extract emit on
#      arg type being int or poly.
#   3. emit_poly_builtin_dispatch's `[]` block also gates on int/poly
#      key — for non-int keys the built-in arms are unreachable and
#      emitting them produces the same Wint-conversion warnings.

class A
  def [](k)
    "from-A:" + k
  end
end

class B
  def [](k)
    "from-B:" + k
  end
end

def lookup(receiver, key)
  receiver[key]
end

puts lookup(A.new, "alpha")
puts lookup(B.new, "beta")
