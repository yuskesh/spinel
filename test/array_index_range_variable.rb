# Array#[] with a Range held in a variable/parameter (not a literal RangeNode)
# routes to the slice path instead of the scalar integer-index accessor, which
# previously passed an sp_Range to an mrb_int parameter and failed to compile.
# Each method keeps its array parameter monomorphic so the typed-array path is
# exercised.
def si(a, rng); a[rng]; end

p si([1, 2, 3, 4], 1..2)      # [2, 3]
p si([1, 2, 3, 4], 1...3)     # [2, 3]
p si([1, 2, 3, 4], 0..-1)     # [1, 2, 3, 4]
p si([1, 2, 3, 4], 2..)       # [3, 4]  (endless)
p si([1, 2, 3, 4], ..1)       # [1, 2]  (beginless)
p si([1, 2, 3, 4], -2..)      # [3, 4]  (negative start)
p si([1, 2, 3, 4], 4..5)      # []      (start == length)

def ss(a, rng); a[rng]; end
p ss(["a", "b", "c"], 0..1)   # ["a", "b"]

def sf(a, rng); a[rng]; end
p sf([1.5, 2.5, 3.5], 1..2)   # [2.5, 3.5]

# The value survives further chaining (proves the subarray type is correct).
p si([1, 2, 3, 4], 1..2).map { |x| x * 10 }   # [20, 30]

# A start outside [-length, length] is nil, not a clamped slice. Checked via
# .nil? -- p renders a nil array-typed value as [] (a separate display gap).
def sn(a, rng); a[rng].nil?; end
p sn([1, 2, 3, 4], -5..-1)    # true   (start < -length)
p sn([1, 2, 3, 4], -6..2)     # true
p sn([1, 2, 3, 4], 5..6)      # true   (start > length)
p sn([1, 2, 3, 4], -4..-1)    # false  (start == -length, in range)
p sn([1, 2, 3, 4], 1..2)      # false
