# An ffi_const referenced through a NESTED constant path (Outer::CMath::MODE)
# must still resolve parent-qualified by the module's leaf name -- the
# leaf-keyed plain-constant table would otherwise claim the reference for a
# same-leaf constant in another module. (ffi_* is a spinel extension, so the
# expected output is authored, not ruby-generated, like the other ffi tests.)
module Verbs
  MODE = "verbose"
end

module Outer
  module CMath
    ffi_lib "m"
    ffi_const :MODE, 7
    ffi_func :fabs, [:double], :double
  end
end

m = 7
puts(m == Outer::CMath::MODE ? "int" : "collision")
puts Verbs::MODE
puts Outer::CMath::MODE + 1
puts Outer::CMath.fabs(-1.5)
