# ffi_callback declares a C function-pointer type. A method(:name) passed to an
# argument of that type becomes a compile-time trampoline that converts the C
# args, calls the compiled method, and converts the result back -- so a Ruby
# method can be handed to a C API (qsort's comparator, bsearch's, atexit's, ...).
#
# A function taking a callback has its extern skipped: the symbol is declared by
# a system header whose per-argument const qualification we can't reproduce
# (qsort takes `void *base`, bsearch takes `const void *base`), so we call the
# header prototype directly and cast pointer-data args to void*. Exercising both
# qsort and bsearch here proves that design against both const-qualifications.
# (Spinel-native FFI DSL, not valid CRuby; the .expected is authored against the
# deterministic libc behavior on a little-endian target.)
module L
  ffi_callback :cmp,  [:ptr, :ptr], :int
  ffi_func     :qsort,   [:int_array, :size_t, :size_t, :cmp], :void
  ffi_func     :bsearch, [:int_array, :int_array, :size_t, :size_t, :cmp], :ptr
  ffi_read_i32  :val, 0          # read an element's int value through its pointer

  ffi_callback :hook,   [], :void
  ffi_func     :atexit, [:hook], :int
end

def cmp(a, b)
  L.val(a) <=> L.val(b)
end

def rcmp(a, b)
  L.val(b) <=> L.val(a)
end

def bye
  puts "at exit"
end

arr = [3, 1, 4, 1, 5, 9, 2, 6]
L.qsort(arr, arr.size, 8, method(:cmp))
p arr                                        # ascending

arr2 = [10, 20, 30, 40]
L.qsort(arr2, arr2.size, 8, method(:rcmp))
p arr2                                       # descending

# bsearch over the ascending array (its `const void *base` differs from qsort's
# `void *base` -- the reason the extern is skipped rather than synthesized). The
# key is a one-element buffer holding the value to find.
hit = L.bsearch([5], arr, arr.size, 8, method(:cmp))
puts hit.nil? ? "miss" : L.val(hit)          # 5

miss = L.bsearch([7], arr, arr.size, 8, method(:cmp))
puts miss.nil? ? "miss" : L.val(miss)        # miss

L.atexit(method(:bye))    # the trampoline runs `bye` at program exit
puts "done"
