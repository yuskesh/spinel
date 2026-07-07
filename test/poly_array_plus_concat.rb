# `Array#+` on a POLY-typed array value silently returned 0 instead of
# concatenating. When a record field's only initializer is an empty `[]`, its
# element type is unresolvable, so it is stored as a boxed poly value and `+`
# compiles to sp_poly_add (the dynamic `+` dispatcher). sp_poly_add handled
# numeric and string operands but fell through to `0` for arrays -- so the
# append vanished. The dispatcher now concatenates when both operands are arrays.
M = Data.define(:xs)

m = M.new(xs: [])
m2 = m.with(xs: m.xs + ["a", "b"])
p m2.xs
p m2.xs.size

n = M.new(xs: [])
p (n.xs + [1, 2]).size          # poly array + int array
p (n.xs + [[1], [2]]).length    # poly array + nested array
