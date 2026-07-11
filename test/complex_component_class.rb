# Complex components carry their CRuby class (Integer vs Float): rendering,
# real/imaginary, abs/abs2, and arithmetic propagation.
def ts(c) = c.to_s
def ins(c) = c.inspect
def re(c) = c.real
def im(c) = c.imag
def ab(c) = c.abs
def ab2(c) = c.abs2

# rendering keeps each component's class
puts ts(Complex(1, 0.0))     # 1+0.0i
puts ts(Complex(1.0, 2))     # 1.0+2i
puts ins(Complex(3, 4))      # (3+4i)
puts ins(Complex(3.0, 4.0))  # (3.0+4.0i)
puts ins(3.0i)               # (0+3.0i)
puts ins(4i)                 # (0+4i)
# real / imaginary keep the component class
p re(Complex(5, 2))          # 5
p re(Complex(5.0, 2))        # 5.0
p im(Complex(5, 2))          # 2
p im(Complex(5, 2.5))        # 2.5
# abs: Integer only via the zero-component shortcut on an all-Integer complex
p ab(Complex(0, 2))          # 2
p ab(Complex(-3, 0))         # 3
p ab(Complex(3, 4))          # 5.0
p ab(Complex(2, 0.0))        # 2.0
p ab(Complex(0.5, 0))        # 0.5
# abs2: Integer iff both components are Integer-classed
p ab2(Complex(3, 4))         # 25
p ab2(Complex(3.0, 4))       # 25.0
p ab2(Complex(0.5, 0.5))     # 0.5
# arithmetic propagates per component (add/sub) or poisons both (mul/div)
p Complex(1, 2) + Complex(1, 3)      # (2+5i)
p Complex(1.0, 2) + Complex(1, 3)    # (2.0+5i)
p Complex(1, 2) * Complex(2, 3)      # (-4+7i)
p Complex(1.0, 2) * Complex(2, 3)    # (-4.0+7.0i)
p Complex(1, 2) - 1                  # (0+2i)
p Complex(1, 2) + 0.5                # (1.5+2i)
# polar constructor: exact right angles keep the magnitude's class
p Complex.polar(2, 0)                # (2+0.0i)
p Complex.polar(2.0, 0)              # (2.0+0.0i)
p Complex.polar(5, Math::PI / 2)     # (0.0+5i)
p Complex.polar(3, Math::PI)         # (-3+0.0i)
p Complex.polar(2, 1)                # (1.0806046117362795+1.682941969615793i)
# conjugate and negation keep classes
p Complex(1, 2.5).conjugate          # (1-2.5i)
p (-Complex(1.5, 2))                 # (-1.5-2i)
