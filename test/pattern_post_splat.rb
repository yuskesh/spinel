# Array and find patterns must bind fixed elements that come AFTER a splat.
#
# The array-pattern lowering handled `requireds` (before the splat) and `rest`
# (the splat), but ignored `posts` (fixed elements after the splat): the rest
# slice greedily grabbed everything to the end, so `[*pre, last]` bound
# pre=[1,2,3,4] and last=nil, and a post literal was never checked. The find
# pattern (`[*, lit => x, *]`) only bound plain LocalVariableTargetNode window
# elements, skipping a `lit => x` capture -- so x came back nil.
#
# Fix: count posts in the length check, value-check post literals from the tail,
# slice the rest as the middle (drop the posts), and bind posts by index from the
# end; bind a capture target in a find-pattern window.

# 1. Splat then one fixed (typed array).
case [1, 2, 3, 4]
in [*pre, last]; p [pre, last]; end          #=> [[1, 2, 3], 4]

# 2. Fixed, splat, fixed.
case [1, 2, 3, 4]
in [a, *mid, z]; p [a, mid, z]; end           #=> [1, [2, 3], 4]

# 3. Two leading, splat, one trailing.
case [1, 2, 3, 4, 5]
in [x, y, *rest, w]; p [x, y, rest, w]; end    #=> [1, 2, [3, 4], 5]

# 4. A post literal must match / mismatch drives the arm.
case [1, 2, 9]
in [*, 9]; p "matched"; end                    #=> "matched"
case [1, 2, 8]
in [*, 9]; p "no"
else; p "fellthrough"; end                     #=> "fellthrough"

# 5. Regression: leading fixed + trailing splat still works.
case [1, 2, 3]
in [first, *r]; p [first, r]; end              #=> [1, [2, 3]]

# 6. Regression: exact (no rest) still works.
case [1, 2]
in [a, b]; p [a, b]; end                       #=> [1, 2]

# 7. Find pattern: a `lit => x` capture binds x.
case [1, 2, 3, 4]
in [*, 3 => x, *]; p x; end                    #=> 3

# 8. Find pattern with leading and trailing splats bound too.
case [1, 2, 3, 4, 5]
in [*hd, 3 => m, *tl]; p [hd, m, tl]; end       #=> [[1, 2], 3, [4, 5]]

# 9. Poly array (routed through a single-call-site method to defeat
#    const-folding while keeping the scrutinee cleanly poly).
def poly(a) = a
case poly([1, "x", 3, 4])
in [*p2, l2]; p [p2, l2]; end                  #=> [[1, "x", 3], 4]

# 10. Anonymous rest between fixed head and tail (typed array).
case [1, 2, 3]
in [first, *, last]; p [first, last]; end       #=> [1, 3]

# A separate poly helper for the nested cases: sharing `poly` above would widen
# its inferred element type to include a nested array, which is an orthogonal
# poly-slice concern, not what these posts cases exercise.
def pnest(a) = a

# 11. A nested array pattern after a splat (poly) is shape-checked and its
#     inner names bound.
case pnest([1, 2, [3, 4]])
in [*, [a, b]]; p [a, b]; end                   #=> [3, 4]

# 12. A nested-array post whose element is not an array falls through.
case pnest([1, 2, 5])
in [*, [a, b]]; p "no"
else; p "fell"; end                             #=> "fell"

# 13. A `[a, b] => cap` capture after a splat binds the target and the inner names.
case pnest([1, [2, 3]])
in [*, [a, b] => cap]; p [a, b, cap]; end       #=> [2, 3, [2, 3]]

# 14. A find pattern with a nested array window element binds its inner names.
case pnest([1, [2, 3], 4])
in [*, [a, b], *]; p [a, b]; end                #=> [2, 3]

# 15. A typed array cannot hold a nested-array post, so it cannot match.
case [1, 2, 3]
in [*, [a, b]]; p "no"
else; p "typed-fell"; end                       #=> "typed-fell"

puts "done"
