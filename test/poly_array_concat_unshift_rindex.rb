# Poly (heterogeneous) array methods: concat (in-place append, incl. typed-array
# arg), unshift/prepend (front insert), rindex (last match or nil).
def cat(a, b); a.concat(b); end
p cat([1, "x"], [:y, 2])
def cat_typed(a); a.concat([9, 8]); end
p cat_typed([1, "x"])
def un(a); a.unshift(:z, 0); end
p un([1, "x"])
def ri(a, v); a.rindex(v); end
p ri([1, "x", 1, "x"], "x")
p ri([1, "x"], :nope)
def mut; a = [1, "x"]; a.concat([:y]); a; end
p mut
# self-concat must not corrupt when the receiver reallocs mid-copy
def selfcat(a); a.concat(a); end
p selfcat([1, "x", 2.0, :q, 5, 6, 7, 8, 9, 10, 11])
# multiple concat args (incl. a typed-array arg) are all evaluated before appending
def cat3(a, b); a.concat(b, [9, 8]); end
p cat3([1, "x"], [:y])
