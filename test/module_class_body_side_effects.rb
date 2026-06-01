# Side-effecting statements in a module/class body run at definition
# time (top-to-bottom), like CRuby. Before, only def/const declarations
# were processed and statements such as `puts` were silently dropped.
# The body is emitted as a function called from main at the definition
# site, so output interleaves correctly with surrounding statements.

puts "before module"

module M
  puts "in module body"
  VERSION = "1.0"
  puts "version is #{VERSION}"
end

puts "after module"

class C
  puts "in class body"
  def self.hi
    "hi"
  end
end

# Nested module bodies run when the enclosing module is defined
module Outer
  module Inner
    puts "deep body ran"
  end
end

# A class method defined in the body is still callable afterward
puts C.hi
