# ffi_cflags folds every compile-time string form: adjacent literals,
# String#+, __dir__, and File.expand_path — and each module's flags are
# emitted (multi-module programs contribute one SPINEL_CFLAGS marker each).
module MathA
  ffi_lib "m"
  ffi_cflags "-DSPINEL_TEST_A " \
             "-DSPINEL_TEST_B"
  ffi_func :fabs, [:double], :double
end

module MathB
  ffi_lib "m"
  ffi_cflags "-I" + File.expand_path(".", __dir__)
  ffi_func :cos, [:double], :double
end

puts MathA.fabs(-1.5)
puts MathB.cos(0.0)
