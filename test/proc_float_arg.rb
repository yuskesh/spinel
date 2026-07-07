# A Float argument to a proc / lambda `.call` (or a lowered `yield`) must read
# back as itself inside the block, not 0.0.
#
# Proc/lambda arguments ride an mrb_int[16] slot in the sp_proc_call ABI. A
# Float placed in that slot was written by an arithmetic double->mrb_int
# conversion (0.7 -> 0) and the callee, binding a concretely-Float-typed param,
# read the truncated int slot back as a double -> 0.0. An Integer arg rides the
# slot losslessly and a `def` method passes floats as real C doubles, so only
# proc/lambda Float args were silently corrupted.
#
# Fix: a Float arg is published boxed into the _sp_proc_poly_args side-channel
# (like a poly arg) and the callee reads a Float param back with sp_poly_to_f --
# mirroring how the proc *return* value already boxes a Float.

# 1. Single Float arg round-trips.
f = lambda { |a| a }
p f.call(0.7)                       #=> 0.7

# 2. Two Float args (add inside the block).
g = lambda { |a, b| a + b }
p g.call(0.5, 0.25)                 #=> 0.75

# 3. A plain proc (not a lambda).
h = proc { |a| a }
p h.call(0.5)                       #=> 0.5

# 4. Mixed Integer + Float args keep their kinds.
m = ->(a, b) { [a, b] }
p m.call(1, 2.5)                    #=> [1, 2.5]

# 5. A Float arg that feeds a built-up structure (the dangerous silent case).
acc = []
add = ->(v) { acc << v }
add.call(0.7)
add.call(1.3)
p acc                              #=> [0.7, 1.3]

# 6. A lowered `yield` of a Float.
def y
  yield 0.7
end
p(y { |x| x })                     #=> 0.7

# 7. The block computes with the Float arg (not just echoes it).
sq = ->(x) { x * x }
p sq.call(0.5)                      #=> 0.25

# 8. Integer args remain correct (regression guard).
i = ->(a) { a }
p i.call(42)                       #=> 42

puts "done"
