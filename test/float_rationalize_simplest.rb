# Float#rationalize returns the SIMPLEST rational that round-trips to the float,
# not the exact power-of-two bit ratio that Float#to_r yields.
p 0.75.rationalize    # (3/4)
p 0.1.rationalize     # (1/10)
p 0.3.rationalize     # (3/10)
p 0.5.rationalize     # (1/2)
p 6.0.rationalize     # (6/1)
p 2.5.rationalize     # (5/2)
p(-0.75.rationalize)  # (-3/4)
p 3.14.rationalize    # (157/50)
p 0.0.rationalize     # (0/1)

# to_r stays the exact ratio (unchanged), and the eps arg form still works.
p 0.75.to_r == Rational(3, 4)       # true
p 0.3.rationalize(0.01)             # (3/10)

# param-routed so it exercises the runtime helper, not a folded literal.
def r(x); x.rationalize; end
p r(0.1)              # (1/10)
