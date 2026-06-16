# A module instance method with body locals, included into a user class.
# `include` clones the method into a scope owned by the class, but that clone
# shares the source's body AST and only registers the params -- the body locals
# (`found`, `doubled`) stay on the source scope. Codegen emits the clone, so
# without copying the source's body locals onto it, every `lv_<name>` reference
# in the emitted body is undeclared C ("use of undeclared identifier"). The
# helper lives on a separate class so its return type resolves cleanly (the
# trigger is the included method's own body locals, not the callee).
class Result
  def initialize(v)
    @v = v
  end
  def v
    @v
  end
end

class Factory
  def self.make(n)
    Result.new(n * 2)
  end
end

module Dispatcher
  def run(n)
    found = Factory.make(n)   # object-typed local, declared in the clone
    doubled = found.v
    doubled + 1
  end
end

class Runner
  include Dispatcher
end

puts Runner.new.run(5).to_s
