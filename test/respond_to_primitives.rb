# respond_to? on built-in (primitive) receivers, answered at compile time by
# probing spinel's own method resolution -- no hand-maintained method list.
# Per-type helpers keep each receiver's concrete type (a single shared helper
# would widen every receiver to a poly value, whose runtime class isn't static).
def st(x); x; end
def si(x); x; end
def sf(x); x; end
def sa(x); x; end
def sh(x); x; end
def sy(x); x; end

# String: no-arg, operator, index, block iterator, String-form argument, absent
p st("hi").respond_to?(:upcase)      # true
p st("hi").respond_to?(:+)           # true
p st("hi").respond_to?(:each_char)   # true
p st("hi").respond_to?("downcase")   # true
p st("hi").respond_to?(:no_such)     # false
p st("hi").respond_to?(:push)        # false

# Integer: block-returning-self, operator, Comparable mixin, absent
p si(5).respond_to?(:times)          # true
p si(5).respond_to?(:+)              # true
p si(5).respond_to?(:between?)       # true
p si(5).respond_to?(:upcase)         # false

# Float
p sf(1.5).respond_to?(:ceil)         # true
p sf(1.5).respond_to?(:round)        # true

# Array: self-returning iterators, map, operator, absent
p sa([1, 2]).respond_to?(:each)             # true
p sa([1, 2]).respond_to?(:each_with_index)  # true
p sa([1, 2]).respond_to?(:map)              # true
p sa([1, 2]).respond_to?(:reverse_each)     # true
p sa([1, 2]).respond_to?(:upcase)           # false

# Hash: iterators + lookups
p sh({a: 1}).respond_to?(:each)      # true
p sh({a: 1}).respond_to?(:each_pair) # true
p sh({a: 1}).respond_to?(:key?)      # true
p sh({a: 1}).respond_to?(:fetch)     # true
p sh({a: 1}).respond_to?(:upcase)    # false

# Symbol
p sy(:foo).respond_to?(:upcase)      # true
p sy(:foo).respond_to?(:to_sym)      # true
p sy(:foo).respond_to?(:each)        # false

# Kernel/Object universals resolve for any receiver
p st("x").respond_to?(:class)        # true
p si(5).respond_to?(:frozen?)        # true
p sa([1]).respond_to?(:tap)          # true

# literal receivers (already concrete-typed)
p nil.respond_to?(:to_a)             # true
p nil.respond_to?(:upcase)           # false
p true.respond_to?(:&)               # true
p (1..3).respond_to?(:each)          # true
p (1..3).respond_to?(:map)           # true
