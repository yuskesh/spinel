# Array#fill's block form takes NO value argument: the positional args are the
# index span (start[, length] or a range) and the value at each index comes from
# the block, invoked with the index. Spinel previously treated the first arg as
# the fill value and ignored the block entirely.
def f2(a); a.fill(1, 2) { |i| i }; end
p f2([1, 2, 3, 4])            # [1, 1, 2, 4]

def f1(a); a.fill(2) { |i| i * 10 }; end
p f1([1, 2, 3, 4])            # [1, 2, 20, 30]

def f0(a); a.fill { |i| i }; end
p f0([9, 9, 9])               # [0, 1, 2]

def fr(a); a.fill(1..2) { |i| i + 100 }; end
p fr([0, 0, 0, 0])            # [0, 101, 102, 0]

def frx(a); a.fill(1...3) { |i| i }; end
p frx([9, 9, 9, 9])           # [9, 1, 2, 9]

# Negative start normalizes against the length.
def fneg(a); a.fill(-2) { |i| i }; end
p fneg([5, 5, 5, 5])          # [5, 5, 2, 3]

# String and float arrays.
def fs(a); a.fill(0, 2) { |i| "x#{i}" }; end
p fs(["a", "b", "c"])         # ["x0", "x1", "c"]

def ff(a); a.fill(1, 2) { |i| i.to_f }; end
p ff([0.0, 0.0, 0.0, 0.0])    # [0.0, 1.0, 2.0, 0.0]

# Out-of-range length clamps; zero length is a no-op.
def fclamp(a); a.fill(2, 10) { |i| i }; end
p fclamp([0, 0, 0, 0])        # [0, 0, 2, 3]

# The return value is the (mutated) receiver.
p f2([1, 2, 3, 4]).class      # Array
