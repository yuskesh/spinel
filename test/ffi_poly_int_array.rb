# An int array whose static type poly-collapsed (a param union widened it)
# must still marshal its element data at an :int_array FFI boundary. The
# poly arm used to reinterpret the boxed value's .v.p as sp_PolyArray*
# regardless of the runtime kind, reading a garbage length out of the boxed
# sp_IntArray and handing the callee NULL (toy's silent LoRA flatline).
#
# The observer is sp_json_str from the bundled json package object (linked
# into every test): plain NUL-terminated C-string semantics over the
# marshalled bytes, no libc builtin for clang to fight (wcslen's extern
# redeclaration was a clang hard error). int64 65 is "A\0..." little-endian
# -> "\"A\""; a NULL marshal distinguishes as "\"\"".
# The :float_array twin of the same fix is exercised end-to-end in
# tools/spin_e2e.sh (crossx), where a test-owned C archive is available.
require "json"   # links the bundled sp_json.o outside the test harness too

module L
  ffi_func :sp_json_str, [:int_array], :str
end

def widen(a)
  a
end

widen(["x", "y"])              # the trigger: widen's param/return go poly
puts L.sp_json_str(widen([65, 66]))

# a poly value holding a non-array raises loudly instead of marshalling NULL
begin
  L.sp_json_str(widen("not an array"))
  puts "no raise"
rescue TypeError => e
  puts "TypeError: #{e.message}"
end
