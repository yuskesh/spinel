# `yield(*arr)`: a single splat spreads its array across the block's parameters
# (auto-splat), exactly like passing the elements positionally. Previously
# rejected ("unsupported expression: SplatNode") because each yield argument was
# emitted as an expression and a SplatNode has no value form. The array is now
# evaluated once into a rooted temp and the block params (plus any rest param)
# bind from its elements, with a short array binding the surplus params to nil.
def s(x); x; end

# splat of a literal, a local, and a method-call result
def m_lit; yield(*[1, 2]); end
p(m_lit { |a, b| a + b })

def m_loc(xs); yield(*xs); end
p(m_loc([3, 4]) { |a, b| a * b })

def m_call; yield(*s([7, 8])); end
p(m_call { |a, b| a + b })

# single param, string elements
def m_one; yield(*[9]); end
p(m_one { |a| a * 10 })

def m_str; yield(*["x", "y"]); end
p(m_str { |a, b| a + b })

# extra elements are dropped; a short array binds the surplus param to nil
def m_extra; yield(*[1, 2, 3]); end
p(m_extra { |a, b| [a, b] })

def m_short; yield(*[1]); end
p(m_short { |a, b| [a, b.nil?] })

# rest parameter collects the remainder
def m_rest; yield(*[1, 2, 3]); end
p(m_rest { |*xs| xs.sum })

def m_fixed_rest; yield(*[1, 2, 3, 4]); end
p(m_fixed_rest { |a, *rest| [a, rest] })

# value position: the yield result is used
def m_value; r = yield(*[2, 3]); r * 2; end
p(m_value { |a, b| a + b })

# multiple yields in one method
def m_twice; yield(*[1, 2]); yield(*[3, 4]); end
m_twice { |a, b| p a + b }

# plain (non-splat) yield is unaffected
def m_plain; yield(10, 20); end
p(m_plain { |a, b| a + b })
