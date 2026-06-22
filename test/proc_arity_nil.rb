# A block/proc param past the supplied argument count binds nil, not the
# typed zero (CRuby fills missing params with nil).
f = proc { |a, b| p b }
f.call(1)                 # nil
f.call(10, 20)            # 20

# nil-awareness flows through .nil? and || default on the missing param.
g = proc { |a, b| p(b.nil?) }
g.call(1)                 # true

h = proc { |a, b| p(b || 99) }
h.call(1)                 # 99
h.call(1, 5)              # 5

# A block forwarded through &blk and called with too few args.
def take(&blk)
  blk.call(7)
end
take { |x, y| p y }       # nil

# The leading supplied param is still bound normally.
k = proc { |a, b| p a }
k.call(42)                # 42

# A lambda invoked with its exact arity is unaffected.
lam = ->(a, b) { p(a + b) }
lam.call(2, 3)            # 5
