# Double-splatting a hash LITERAL at a call site (`f(**{ ... })`) must compile.
#
# The call-site double-splat pre-evaluates the spread hash to a temp. For a bare
# variable that emits with no prelude, but a hash literal drains its own
# construction into the prelude buffer -- which landed inside the temp's
# declaration line, so the emitted C was `sp_SymPolyHash * _t = sp_SymPolyHash
# *_t2 = ...` (a type name where an expression was expected). Render the literal
# into a side buffer first so its construction lands before the assignment.

# 1. Into a keyword-rest parameter.
def a(**k) = k
p a(**{ x: 1, y: 2 })              #=> {x: 1, y: 2}

# 2. Into explicit keyword parameters.
def b(x:, y:) = [x, y]
p b(**{ x: 3, y: 4 })              #=> [3, 4]

# 3. Literal splat plus a trailing explicit key.
def c(**k) = k
p c(**{ x: 1 }, z: 5)              #=> {x: 1, z: 5}

# 4. All keyword params supplied (including one with a default).
def r(x:, y:, z: 0) = [x, y, z]
p r(**{ x: 1, y: 2, z: 9 })        #=> [1, 2, 9]

# 5. String-value kwargs into a kwrest.
def kw(**opts) = opts
p kw(**{ color: "red", size: 3 })  #=> {color: "red", size: 3}

puts "done"
