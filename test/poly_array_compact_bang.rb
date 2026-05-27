# Array#compact! on poly_array mutates in place — shift non-nil
# elements to the front, truncate. Returns the receiver (CRuby
# returns nil when nothing was removed; the subset always returns
# self since the typed array can't carry that signal).
a = [1, nil, 2, nil, 3]
a.compact!
puts a.inspect

# Mixed types with nils interleaved
b = ["a", nil, :sym, nil, 42]
b.compact!
puts b.inspect

# Already nil-free
c = [1, "two", :three]
c.compact!
puts c.inspect
