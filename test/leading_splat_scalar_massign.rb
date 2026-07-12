# A leading/middle-splat massign with a scalar RHS aligns the value to the post-
# rest targets: the rest is empty and the first post-target takes the value.
def a1(x); *a, b = x; [a, b]; end
p a1(1)
def a2(x); *a, b, c = x; [a, b, c]; end
p a2(1)
def a3(x); a, *r, c = x; [a, r, c]; end
p a3(1)
def a4(x); a, b, *r = x; [a, b, r]; end
p a4(1)
def a5(x); *a = x; a; end
p a5(1)
def a6(x); *a, b = x; [a, b]; end
p a6([1, 2, 3])
