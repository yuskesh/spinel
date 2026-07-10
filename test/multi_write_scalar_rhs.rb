# Scalar int RHS to multi-assign: `a, b = 1` previously emitted
# `sp_IntArray *_t = 1LL;` which failed to compile. CRuby treats
# the scalar as `[scalar]` so the first slot gets it; the extra
# targets are nil (an under-filled slot lands its nil sentinel).
a, b = 1
puts a
puts b

e, f, g = 42
puts e
puts f
puts g
