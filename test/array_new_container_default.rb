# Array.new(n, <empty container literal>) fills every slot with ONE
# shared container object (CRuby aliasing: mutating a[0] shows in a[1]).
p Array.new(2, [])
p Array.new(2, {})
a = Array.new(2, [])
a[0] << 1
p a
p Array.new(3, 7)
p Array.new(2, "s")
