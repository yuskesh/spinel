# Named Array set operations: variadic union/intersection/difference and the
# intersect? predicate. (The |/&/- operators and the single-argument named
# forms already worked; these add the multi-argument fold and intersect?.)
# Per-type helpers keep each receiver's concrete element type.
def ai(x); x; end   # int array
def as(x); x; end   # string array
def af(x); x; end   # float array
def ap(x); x; end   # poly (mixed) array

# union folds across every argument, keeping first occurrence, de-duped
p ai([1, 2, 3]).union([2, 3], [3, 4])            # [1, 2, 3, 4]
p ai([1, 2, 3]).union([3, 4])                    # [1, 2, 3, 4]  (single arg still works)

# intersection keeps elements present in the receiver and every argument
p ai([1, 2, 3, 4]).intersection([2, 3, 4], [3, 4, 5])  # [3, 4]

# difference removes every element found in any argument
p ai([1, 2, 3, 4]).difference([2], [4])          # [1, 3]
p ai([1, 2, 3]).difference([2])                  # [1, 3]  (single arg still works)

# intersect? is true when the receiver and the argument share any element
p ai([1, 2, 3]).intersect?([3, 4])               # true
p ai([1, 2, 3]).intersect?([7, 8])               # false
p ai([1, 2, 3]).intersect?([])                   # false (empty argument)

# string arrays
p as(["a", "b"]).union(["b", "c"], ["c", "d"])   # ["a", "b", "c", "d"]
p as(["a", "b", "c"]).intersect?(["c", "z"])     # true

# float arrays
p af([1.0, 2.0, 3.0]).difference([2.0], [3.0])   # [1.0]

# poly (mixed) arrays, with poly-typed arguments
p ap([1, "b", 3]).union([3, "x"], ["y", 9])      # [1, "b", 3, "x", "y", 9]
p ap([1, "b", 3, "x"]).difference([3, "q"], ["x", 0])  # [1, "b"]
p ap([1, "b", 3]).intersect?(["b", 9])           # true
