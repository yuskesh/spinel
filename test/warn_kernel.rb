# Issue #734. `warn arg, ...` writes each arg to stderr with a
# newline (matching Kernel#warn). spinel used to emit the unresolved-
# call warning and drop the IO silently.
#
# at_exit + warn -- both used to fall through to the unresolved-call
# warning. The test runner only captures stdout, so warn's stderr
# output isn't part of the expected diff. at_exit now runs in LIFO
# after main returns (#990).
at_exit { puts "from at_exit" }
warn "hello stderr"
puts "after warn"
warn "two", "args"
puts "done"
