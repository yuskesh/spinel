# Multiple assignment with fewer RHS values than LHS targets fills the extra
# targets with nil (not the type's zero), for scalar, literal-array, and
# mixed-splat RHS forms.
def f(x); a, b, c = x; [a, b, c]; end
p f(1)
def h; a, b = 10; [a, b]; end
p h
def lit; a, b, c = [1, 2]; [a, b, c]; end
p lit
def mid; a, *b, c = [1, 2, 3, 4]; [a, b, c]; end
p mid
def s(x); a, b = x; [a, b]; end
p s("hi")
