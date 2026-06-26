# Issue #734. `warn arg, ...` writes each arg to stderr with a
# newline (matching Kernel#warn). spinel used to emit the unresolved-
# call warning and drop the IO silently.
#
# at_exit + warn -- both used to fall through to the unresolved-call
# warning. warn's stderr output is asserted via the .err.expected file
# alongside this test. at_exit now runs in LIFO after main returns (#990).
at_exit { puts "from at_exit" }
warn "hello stderr"
puts "after warn"
warn "two", "args"
puts "done"
