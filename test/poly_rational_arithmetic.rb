# A polymorphic method whose numeric params unify to poly (called with Integer
# then Rational) does Rational arithmetic when a Rational operand is present,
# instead of truncating it through integer division.
def dv(a, b); a / b; end
def ad(a, b); a + b; end
def ml(a, b); a * b; end
def sb(a, b); a - b; end
p dv(5, 2); p dv(5, 2r); p dv(6, 2r)
p ad(1, 2); p ad(1, 2r)
p ml(3, 4); p ml(3, Rational(1, 2))
p sb(10, 3); p sb(10, Rational(1, 3))

# A Rational operand mixed with a Float unifies to Float, matching CRuby --
# each param also sees a Float call site, so both are boxed poly at runtime.
ad(2, 1.0); sb(2, 1.0); ml(2, 1.0); dv(2, 1.0)
p ad(2r, 3.0); p ad(3.0, 2r)
p sb(2r, 3.0); p ml(2r, 3.0)
p dv(2r, 3.0); p dv(3.0, 2r)
