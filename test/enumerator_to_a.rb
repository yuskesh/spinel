# A bare (blockless) enumerator materializes with #to_a / #entries.
# Array#each, Array#reverse_each, and String#each_char return an external
# Enumerator; #to_a drains it into an array. Receivers are routed through a
# method param to exercise the runtime path rather than a folded constant.
# Distinct per-type helpers keep each one monomorphic (a shared helper would
# go polymorphic and lose the receiver's type).
def ints(x); x; end
def strs(x); x; end
def flts(x); x; end
def str(x); x; end

p ints([1, 2, 3]).each.to_a
p ints([1, 2, 3]).each.entries
p ints([1, 2, 3]).reverse_each.to_a
p strs(["a", "b", "c"]).reverse_each.to_a
p flts([1.5, 2.5]).each.to_a
p str("abc").each_char.to_a
p str("café").each_char.to_a

# reverse_each materializes a snapshot; the receiver is not mutated
nm = ints([1, 2, 3])
p nm.reverse_each.to_a
p nm

# directly-typed empty array
p [].each.to_a

# to_a result is an ordinary array
p ints([1, 2, 3]).each.to_a.map { |x| x * 2 }

# a generator Enumerator also drains
e = Enumerator.new { |y| y << 10; y << 20 }
p e.to_a

# the blockful forms are unchanged
acc = []
[1, 2, 3].reverse_each { |x| acc << x }
p acc
p "hi".each_char { |ch| }
