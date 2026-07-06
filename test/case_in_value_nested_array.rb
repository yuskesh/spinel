# A nested-array case/in used in value position (its result assigned).
def s(x); x; end
r = case s([1, [2, 3], 4])
    in [a, [b, c], d] then [a, b, c, d]
    end
p r
