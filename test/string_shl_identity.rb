# String#<< returns the receiver itself (identity), not a copy (KieranP #2307).
# The mutable-buffer receiver appends in place; the expression value and a
# subsequent read of the variable are the same object.
s = "a"
p((s << "b").equal?(s))
p s
p((s.concat("e")).equal?(s))
p s
t = "x"
u = "y"
t << "z"
p t.equal?(u)          # distinct buffers stay distinct
p s.upcase.equal?(s)   # a non-mutating transform is a new object
