# Issue #745. `[].min` / `[].max` used to read uninitialized memory
# in `sp_IntArray_min` / `_max` (no empty-array guard before reading
# `a->data[a->start]`). Now both return 0 (spinel's nil-collapse for
# int-typed slots; CRuby returns nil, which `inspect`s as "nil" --
# spinel's int model doesn't have a nil inhabitant for this slot).

# `[1].first(0)` produces an empty int-typed array, preserving the
# element type. Both min/max should return safely now.
a = [1].first(0)
puts a.min.inspect
puts a.max.inspect

# Non-empty: unchanged.
b = [3, 1, 4, 1, 5, 9, 2, 6]
puts b.min
puts b.max
