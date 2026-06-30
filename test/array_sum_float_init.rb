# Array#sum with a Float initial value returns a Float, even for an integer
# array: CRuby accumulates in floating point. Spinel had truncated the float
# initial value through the integer sum (returning an Integer). Distinct
# monomorphic helpers keep the int- and float-array receivers from unifying.
def ai(x); x; end
def af(x); x; end

p ai([1, 2, 3]).sum(0.0)      # => 6.0
p ai([1, 2, 3]).sum(10.5)     # => 16.5
p ai([10, 20]).sum(1.5)       # => 31.5
p ai([]).sum(2.0)             # => 2.0

# an integer (or no) initial value still yields an Integer
p ai([1, 2, 3]).sum(0)        # => 6
p ai([1, 2, 3]).sum           # => 6

# a float array is unchanged
p af([1.5, 2.5]).sum(1)       # => 5.0
p af([1.5, 2.5]).sum          # => 4.0

# the block form also promotes to Float with a float initial value
p ai([1, 2, 3]).sum(0.0) { |x| x }      # => 6.0
p ai([1, 2, 3]).sum(10.5) { |x| x }     # => 16.5
p ai([1, 2, 3]).sum(0.0) { |x| x * 2 }  # => 12.0
