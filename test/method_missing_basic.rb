# method_missing fallback dispatch: an unresolved call on an object whose class
# chain defines method_missing routes there, receiving the method name as a
# Symbol and the call's arguments as a rest array. Real methods and universal
# builtins still take precedence.
class Proxy
  def initialize(label)
    @label = label
  end

  def real_method
    "real"
  end

  def method_missing(name, *args)
    "#{@label}:#{name}/#{args.length}"
  end
end

p = Proxy.new("px")
puts p.real_method            # real method wins
puts p.anything               # -> method_missing, no args
puts p.greet("a", "b")        # -> method_missing, two args
puts p.class.name             # universal builtin wins (not method_missing)

# inherited method_missing through the superclass chain
class Sub < Proxy
end
s = Sub.new("sub")
puts s.whatever(1)

# method_missing return value used in an expression context
def describe(o)
  o.mystery.upcase
end
puts describe(Proxy.new("z"))
