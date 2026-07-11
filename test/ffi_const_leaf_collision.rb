# An ffi_const whose leaf name collides with a plain constant in another
# module must resolve parent-qualified (the plain constant used to claim
# the leaf-keyed slot, silently rebinding the reference and its type).
module Verbs
  TEXT = "download-text"
end

module CMath
  ffi_lib "m"
  ffi_const :TEXT, 3
  ffi_func :fabs, [:double], :double
end

t = 3
puts(t == CMath::TEXT ? "int" : "collision")
puts Verbs::TEXT
puts CMath::TEXT + 1
puts CMath.fabs(-2.5)
