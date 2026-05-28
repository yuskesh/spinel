# ffi_cflags / ffi_func args fold compile-time string forms:
# __dir__, File.expand_path(rel, base), and String#+. The folded
# -I<dir> is a harmless include path; the test passes iff the fold
# succeeds (a non-foldable arg would abort analyze with "string
# literal" before this runs).
module Pathy
  ffi_cflags("-I" + File.expand_path(".", __dir__))
  ffi_func :strlen, [:str], :size_t
end

puts Pathy.strlen("hello")
puts Pathy.strlen("")
