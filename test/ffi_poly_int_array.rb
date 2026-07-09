# An int array whose static type poly-collapsed (a param union widened it)
# must still marshal its element data at an :int_array FFI boundary. The
# poly arm used to reinterpret the boxed value's .v.p as sp_PolyArray*
# regardless of the runtime kind, reading a garbage length out of the boxed
# sp_IntArray and handing the callee NULL (toy's silent LoRA flatline).
# wcslen observes the data (wchar_t is 4 bytes here): int64 65 is
# {65, 0, ...} as wchar_t -> 1; double 65.0 starts with a 0 word -> 0.
# A NULL marshal would segfault wcslen (loud test failure).
module L
  ffi_func :wcslen, [:int_array], :size_t
end
module LF
  ffi_func :wcsnlen, [:float_array, :size_t], :size_t
end

def widen(a)
  a
end

widen(["x", "y"])              # the trigger: widen's param/return go poly
p L.wcslen(widen([65, 66]))
p LF.wcsnlen(widen([65.0]), 2)

# a poly value holding a non-array raises loudly instead of marshalling NULL
begin
  L.wcslen(widen("not an array"))
  puts "no raise"
rescue TypeError => e
  puts "TypeError: #{e.message}"
end
