# An `exit(status)` whose status expression is poly-typed (here a poly-hash
# get) must unbox the value -- a bare (int)(sp_RbVal) cast is a cc error.
h = { "status" => 0, "note" => "text" }
puts "before exit"
exit(h["status"])
