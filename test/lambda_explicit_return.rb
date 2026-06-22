# An explicit `return <value>` inside a lambda/proc body must carry its
# value out through `.call` instead of being lost (returning 0). The proc
# return-type inference has to consider the ReturnNode tail, not only an
# implicit tail expression, so the call site and the body's return channel
# agree on the type.
f = lambda { return 7 }
puts f.call

g = -> { return 42 }
puts g.call

# Explicit return of a non-int value.
s = lambda { return "hi" }
puts s.call

fl = lambda { return 1.5 }
puts fl.call

# Explicit return that uses the block parameter.
dbl = lambda { |x| return x * 2 }
puts dbl.call(5)

# Routed through a method parameter (defeats any constant-folding of the
# receiver, exercising the general proc-call path).
def via(p)
  p.call
end
puts via(lambda { return 99 })

# Conditional explicit return with a matching implicit tail type.
pick = lambda { |n| return 0 if n.negative?; n + 1 }
puts pick.call(-3)
puts pick.call(4)
