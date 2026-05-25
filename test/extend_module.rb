# Issue #721. `class C; extend M; end` should expose M's instance
# methods as class methods on C. spinel used to silently ignore the
# extend call; `C.<m>` would fall through to the unresolved-call
# warning + emit 0.

module M
  def ext_method; "extended"; end
  def hello(name); "hi " + name; end
end

class C
  extend M
end

puts C.ext_method
puts C.hello("world")

# `def self.X` on M stays on M and is NOT pulled into the extending
# class (CRuby semantics).
module N
  def self.solo; "solo-on-N"; end
  def shared; "shared"; end
end

class D
  extend N
end

puts D.shared
puts N.solo
