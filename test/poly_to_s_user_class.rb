# A user class defining an instance `to_s`/`inspect` must not capture boxed
# scalars in the polymorphic to_s/inspect dispatch. Every boxed scalar carries
# cls_id 0, which aliased the first user class (index 0): a poly `value.to_s`
# with a user class 0 defining `to_s` entered that class's method and
# dereferenced the scalar's payload, segfaulting. Issue #1576.

class Env
  def initialize(name)
    @name = name
  end

  def to_s
    @name
  end

  def inspect
    "#<Env #{@name}>"
  end
end

# `value` is untyped (called with mixed types -> poly); `.to_s` has no
# statically-known receiver type, so it lowers to the class-id dispatch.
def render(value)
  value.to_s
end

def show(value)
  value.inspect
end

# boxed scalars must fall through to the runtime poly converter, not Env
puts render(42)             # 42
puts render("hello")        # hello
puts render(3.14)           # 3.14
puts render(:sym)           # sym
puts render(nil)            # (empty line)

# the actual user object still dispatches to Env#to_s / #inspect
puts render(Env.new("env")) # env
puts show(Env.new("x"))     # #<Env x>

# inspect over scalars routes through the poly converter too
puts show(42)               # 42
puts show("hi")             # "hi"
puts show(:s)               # :s
