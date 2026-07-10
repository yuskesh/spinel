# `return *x` returns the splat-to-array of x: a scalar wraps in a one-element
# array, an array stays itself, nil becomes []. Spinel previously emitted C
# whose return type (the splat's element type) conflicted with the array value.
def f(x); return *x; end
p f(1)              # [1]
p f([1, 2, 3])      # [1, 2, 3]
p f(nil)            # []
p f("hi")           # ["hi"]
p f([])             # []

# The returned array is a real Array usable downstream.
p f(5).map { |n| n * 2 }        # [10]
p f([1, 2]).sum                 # 3

# A conditional splat return alongside an ordinary return keeps both typed as
# an array (the method return unifies to a poly array).
def g(x)
  return *x if x
  [0]
end
p g(3)              # [3]
p g(nil)            # [0]

# A conditional splat return whose other branch is a non-array (scalar) forces
# the method return type to poly (not a poly array), so the splat value is
# boxed rather than array-typed. It must still splat-to-array correctly.
def hp(cond, x)
  return *x if cond
  1
end
p hp(true, [1, 2])  # [1, 2]
p hp(false, nil)    # 1
p hp(true, 5)       # [5]
p hp(true, nil)     # []
