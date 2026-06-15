# A `require` is a compile-time directive in Spinel; one that survives into
# codegen -- indented inside an `if`, a method body, or used in value position
# -- is a runtime no-op rather than an "unsupported CallNode" hard error.

if 1 > 0
  require "set"
end

def helper
  require "json"
  "ok"
end

x = require("set")
puts helper
puts "done"
