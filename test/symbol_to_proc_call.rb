# Explicit Symbol#to_proc applied with a call: :sym.to_proc.call(recv, *args)
# invokes the named method on the first argument. With the symbol and the call
# site statically known, it lowers to an ordinary recv.sym(*args) method call.
# (The &:sym block form is unaffected; a to_proc held in a variable is a
# separate, still-unsupported case.)
def ss(x); x; end   # string receiver (per-type helper avoids poly widening)
def si(x); x; end   # integer receiver

# a no-argument method on the receiver
p :upcase.to_proc.call("hi")          # "HI"
p :length.to_proc.call("hello")       # 5
p :succ.to_proc.call(5)               # 6
p :to_s.to_proc.call(42)              # "42"

# a method that takes extra arguments (receiver first, then the rest)
p :+.to_proc.call(2, 3)               # 5
p :*.to_proc.call(3, 4)               # 12

# the .() call shorthand lowers the same way
p :downcase.to_proc.("HI")            # "hi"

# receiver routed through a method parameter (runtime-typed path)
p :upcase.to_proc.call(ss("world"))   # "WORLD"
p :abs.to_proc.call(si(-7))           # 7

# the &:sym block form still works
p ["a", "b", "c"].map(&:upcase)       # ["A", "B", "C"]
