# Float#% (and Integer % Float) were rejected at compile time. Ruby's float
# modulo is floored: a NONZERO result takes the sign of the DIVISOR, unlike C
# fmod which follows the dividend (-5.5 % 2 == 0.5). A ZERO result instead keeps
# the sign of the DIVIDEND (matching fmod / CRuby's flodivmod), so -2.0 % 2.0 is
# -0.0 while 2.0 % -2.0 is 0.0. The `modulo` alias shares these semantics
# (previously it used plain fmod and was wrong for negative operands).
def f(x); x; end
def i(x); x; end

# Nonzero results: sign follows the divisor.
p(f(5.5) % 2)        # 1.5
p(f(5.5) % 2.0)      # 1.5
p(f(-5.5) % 2)       # 0.5
p(f(5.5) % -2)       # -0.5
p(f(-5.5) % -2)      # -1.5
p(f(5.5) % 2.5)      # 0.5
p(f(-5.5) % 2.5)     # 2.0
p(i(5) % 2.0)        # 1.0   (Integer % Float -> Float)
p(f(7.0) % 3)        # 1.0
p(5.5 % 2)           # 1.5   (constant-folds, but exercises the literal path)

# Zero results: sign follows the dividend.
p(f(-2.0) % 2.0)     # -0.0
p(f(2.0) % -2.0)     # 0.0
p(f(2.0) % 2.0)      # 0.0
p(f(-2.0) % -2.0)    # -0.0
p(i(4) % 2.0)        # 0.0
p(i(-4) % 2.0)       # -0.0

# modulo alias
p(f(5.5).modulo(2))    # 1.5
p(f(-5.5).modulo(2))   # 0.5
p(f(5.5).modulo(-2))   # -0.5
p(f(-2.0).modulo(2.0)) # -0.0
p(f(2.0).modulo(-2.0)) # 0.0
