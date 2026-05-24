# #688: empty `[]` followed by `.push(:ptr)` must promote to
# ptr_ptr_array, not silently retain the IntArray default (which
# would round-trip the pointer through mrb_int).

module LibC
  ffi_func :malloc, [:size_t], :ptr
  ffi_func :free,   [:ptr],    :void
end

p1 = LibC.malloc(64)
p2 = LibC.malloc(64)

arr = []
arr.push(p1)
arr.push(p2)

puts arr.length

# Read back the pointers and confirm they're identical to what we pushed.
got1 = arr[0]
got2 = arr[1]
puts (got1 == p1) ? "p1=match" : "p1=MISMATCH"
puts (got2 == p2) ? "p2=match" : "p2=MISMATCH"

LibC.free(p1)
LibC.free(p2)
