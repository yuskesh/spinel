# `|*|` (anonymous rest) accepts any arity in a lambda; the arity check keys
# on rest PRESENCE, not on it having a name.
l = lambda { |*| 1 }
p l.call(2, 3, 4)
p l.call
pr = proc { |*| 7 }
p pr.call(1)
