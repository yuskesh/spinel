# In-place Hash#merge! / #update mutate the receiver and return it.
# Per-variant helpers keep each receiver's concrete hash type (a single shared
# helper would unify every variant to a poly-valued hash).
def hsym(x); x; end   # symbol-keyed, int-valued
def hpol(x); x; end   # symbol-keyed, mixed (poly) values
def hstr(x); x; end   # string-keyed, int-valued

# basic merge! adds the other hash's pairs
a = hsym({a: 1})
a.merge!({c: 3})
p a                 # {a: 1, c: 3}

# update is an alias; both return the receiver itself
b = hsym({a: 1})
r = b.update({b: 2})
p [r, b]            # [{a: 1, b: 2}, {a: 1, b: 2}]

# a conflicting key is resolved by the block (key, old, new)
cf = hsym({a: 1, b: 2})
cf.merge!({b: 10, c: 3}) { |k, old, new| old + new }
p cf                # {a: 1, b: 12, c: 3}

# without a block, the other hash wins on a conflict
ov = hsym({a: 1, b: 2})
ov.merge!({b: 99})
p ov                # {a: 1, b: 99}

# merging an empty hash is a no-op
em = hsym({a: 1})
p em.merge!({})     # {a: 1}

# chaining works because each call returns the receiver
ch = hsym({a: 1})
ch.merge!({b: 2}).merge!({c: 3})
p ch                # {a: 1, b: 2, c: 3}

# poly-valued receiver accepts further pairs
pv = hpol({a: 1, b: "x"})
pv.merge!({c: 9})
p pv                # {a: 1, b: "x", c: 9}

# string-keyed receiver
sk = hstr({"a" => 1})
sk.update({"b" => 2})
p sk                # {"a" => 1, "b" => 2}
