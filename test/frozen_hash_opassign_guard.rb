# A compound assignment (h[k] += v) to a frozen Hash raises FrozenError instead
# of silently mutating; a non-frozen hash updates normally.
def inc(h); h[:x] += 1; h[:x]; rescue FrozenError => e; e.class.name; end
p inc({x: 5}.freeze)
p inc({x: 5})
